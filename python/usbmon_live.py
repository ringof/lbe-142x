#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2026 Benjamin Vernoux
#
# Live LBE-142x HID command decoder. Reads the kernel usbmon *text* stream
# and prints each HID Feature report (SET_REPORT / GET_REPORT) with a
# wall-clock timestamp and decoded opcode, so you can click a control in the
# vendor tool and immediately see which opcode/argument it produced.
#
# This reads /sys/kernel/debug/usb/usbmon/<bus>u directly -- no tshark, no
# AppArmor, no pcap. The device can stay redirected to a VM running the
# vendor tool; the URBs still pass through the host kernel.
#
# Usage (bus 3, device address 63 -- from `lsusb -d 1dd2:`):
#   sudo modprobe usbmon
#   sudo python3 python/usbmon_live.py --dev 63 /sys/kernel/debug/usb/usbmon/3u
# or pipe it (use stdbuf so cat doesn't block-buffer the live stream):
#   sudo stdbuf -oL cat /sys/kernel/debug/usb/usbmon/3u | python3 python/usbmon_live.py --dev 63
#
# Tip: if debugfs isn't mounted, `sudo mount -t debugfs none /sys/kernel/debug`.
# Leave it running, then operate ONE control at a time in the vendor UI.

import argparse
import sys
from datetime import datetime

OPCODE_NAMES = {
    0x01: "EN_OUT",
    0x02: "BLINK",
    0x03: "1425-NEW (stepper?)",
    0x04: "1425-NEW (multi-level?)",
    0x05: "SET_F1_TEMP",
    0x06: "SET_F1",
    0x08: "1425-NEW (telemetry poll?)",
    0x09: "SET_F2_TEMP",
    0x0A: "SET_F2",
    0x0B: "SET_PLL (0=PLL,1=FLL)",
    0x0C: "SET_PPS (0=off,1=on)",
    0x0D: "SET_PWR1 (0=norm,1=low)",
    0x0E: "SET_PWR2 (0=norm,1=low)",
    0x0F: "1425-NEW (toggle 0/1)",
}


def u32le(b, off):
    if len(b) < off + 4:
        return None
    return b[off] | (b[off + 1] << 8) | (b[off + 2] << 16) | (b[off + 3] << 24)


def parse_line(line):
    """Parse one usbmon 'u'-format line. Returns a dict or None.

    Format:  tag  ts_us  S|C|E  <type><dir>:bus:dev:ep  [s bmRT bReq wVal wIdx wLen | status]
             datalen  =|<|>  [data words...]
    """
    tok = line.split()
    if len(tok) < 5:
        return None
    etype = tok[2]                       # S / C / E
    addr = tok[3].split(":")             # e.g. Co:3:063:0
    if len(addr) < 4 or not addr[0] or addr[0][0] != "C":   # control only
        return None
    direction = addr[0][1] if len(addr[0]) > 1 else "?"     # i / o
    try:
        bus, dev = int(addr[1]), int(addr[2])
    except ValueError:
        return None

    i = 4
    setup = None
    if tok[i] == "s":                    # setup packet present
        try:
            bm = int(tok[i + 1], 16); req = int(tok[i + 2], 16)
            wval = int(tok[i + 3], 16); widx = int(tok[i + 4], 16)
            wlen = int(tok[i + 5], 16)
        except (ValueError, IndexError):
            return None
        setup = (bm, req, wval, widx, wlen)
        i += 6
    else:
        i += 1                           # status word

    data = b""
    # remaining: datalen, tag, words...
    if i + 1 < len(tok) and tok[i + 1] == "=":
        words = "".join(tok[i + 2:])
        try:
            data = bytes.fromhex(words)
        except ValueError:
            data = b""
    return {"etype": etype, "dir": direction, "bus": bus, "dev": dev,
            "setup": setup, "data": data}


def describe(ev):
    bm, req = ev["setup"][0], ev["setup"][1]
    data = ev["data"]
    if bm == 0x21 and req == 0x09:        # SET_REPORT (write) -- the user's action
        if not data:
            return None
        op = data[0]
        name = OPCODE_NAMES.get(op, f"op 0x{op:02X} (unknown)")
        arg = data[1] if len(data) > 1 else 0
        extra = f"arg=0x{arg:02X}"
        if op in (0x05, 0x06, 0x09, 0x0A):
            f = u32le(data, 5)
            if f is not None:
                extra = f"freq={f} Hz (@byte5)"
        hexd = " ".join(f"{b:02X}" for b in data[:12])
        return f"SET_REPORT  op=0x{op:02X} {name:24} {extra}   [{hexd}]"
    if bm == 0xA1 and req == 0x01:        # GET_REPORT response (status read)
        if not data:
            return None                   # submit has no data; the C line does
        hexd = " ".join(f"{b:02X}" for b in data[:24])
        return f"GET_REPORT  (status read) [{hexd}...]"
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dev", type=int, default=None,
                    help="usb device address to watch (the 'Device NNN' from "
                         "lsusb); omit to watch all control transfers")
    ap.add_argument("--reads", action="store_true",
                    help="also print GET_REPORT status reads (noisy: the GUI polls)")
    ap.add_argument("node", nargs="?", default=None,
                    help="usbmon text node to read (e.g. /sys/kernel/debug/usb/"
                         "usbmon/3u); omit to read stdin")
    args = ap.parse_args()

    # readline() (not `for line in f`) so lines surface as the kernel emits
    # them, instead of waiting for Python's iterator read-ahead to fill.
    stream = open(args.node) if args.node else sys.stdin

    print(f"# watching usbmon{' dev '+str(args.dev) if args.dev else ' (all devices)'}"
          f" -- operate the vendor UI now; Ctrl-C to stop", file=sys.stderr)
    while True:
        line = stream.readline()
        if not line:
            break
        ev = parse_line(line)
        if not ev or ev["setup"] is None:
            continue
        if args.dev is not None and ev["dev"] != args.dev:
            continue
        desc = describe(ev)
        if not desc:
            continue
        if desc.startswith("GET_REPORT") and not args.reads:
            continue
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        print(f"[{ts}] {desc}", flush=True)


if __name__ == "__main__":
    main()
