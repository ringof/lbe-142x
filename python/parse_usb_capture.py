#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2026 Benjamin Vernoux
#
# Reverse-engineering helper for LBE-142x USB captures.
#
# Feeds an usbmon/USBPcap capture of the *vendor* tool (or this one) to
# tshark, extracts every HID Feature report (SET_REPORT 0x09 / GET_REPORT
# 0x01) and any interrupt-IN data, decodes the report-ID/opcode, and
# annotates it against the known LBE-1420/1421/Mini opcode map. The point
# is to answer, at a glance: "does the LBE-1425 speak the existing 1421
# wire protocol, or something new?"
#
# Usage:
#   python3 parse_usb_capture.py capture.pcapng
#   python3 parse_usb_capture.py capture.pcapng --vid 0x1dd2 --tshark /path/to/tshark
#
# Capture recipe (Linux host, device redirected into a VM running the
# vendor tool -- the URBs still traverse the host kernel):
#   sudo modprobe usbmon
#   lsusb -d 1dd2:                 # note the "Bus 0NN" number
#   sudo tshark -i usbmon<NN> -w capture.pcapng
# Perform ONE operation in the vendor tool, stop the capture, run this.

import argparse
import shutil
import subprocess
import sys

# Opcode == HID Report ID (low byte of wValue) for the 1420/1421/1423
# family: the firmware reads the opcode out of wValue. Mini opcodes collide
# numerically (see lbe_common.h) so they're listed as alternates.
OPCODE_NAMES = {
    0x01: "EN_OUT (outputs on/off)",
    0x02: "BLINK_OUT",
    0x03: "1420_SET_F1_TEMP / MINI_SET_DRIVE",
    0x04: "1420_SET_F1 / MINI_SET_PLL",
    0x05: "1421_SET_F1_TEMP",
    0x06: "1421_SET_F1",
    0x07: "1420_SET_PWR1",
    0x08: "MINI_UBX_WRAP",
    0x09: "1421_SET_F2_TEMP",
    0x0A: "1421_SET_F2 / MINI_NAV_STREAM",
    0x0B: "SET_PLL (PLL/FLL mode)",
    0x0C: "1421_SET_PPS",
    0x0D: "1421_SET_PWR1",
    0x0E: "1421_SET_PWR2",
    0x0F: "1425-NEW (toggle 0/1, function TBD)",
    0x4B: "STATUS_REPORT (read)",
}

# Opcodes the LBE-1425 vendor tool uses that the 1421 does not. Flagged so a
# capture makes the new surface obvious; functions documented (TBD) in
# docs/reverse/LBE-1425-config-v1.10.md. (0x03/0x04/0x08 also appear in the
# 1420/Mini maps above with different meanings; on the 1425 they're new.)
LBE1425_NEW_OPCODES = {0x03, 0x04, 0x08, 0x0F}

REPORT_TYPE = {1: "Input", 2: "Output", 3: "Feature"}


def parse_capdata(s):
    """Turn a tshark hex field into a list of byte values, tolerant of the
    several shapes tshark emits: ':'-separated ('b5:62'), comma-joined
    repeated fields, or one continuous hex string ('b562...'). Skips any
    token that isn't a single byte instead of crashing."""
    if not s:
        return []
    s = s.strip()
    if "," in s:                       # repeated field occurrences
        s = max(s.split(","), key=len)
    toks = s.split(":") if ":" in s else [s[i:i + 2] for i in range(0, len(s), 2)]
    out = []
    for t in toks:
        t = t.strip()
        if not t:
            continue
        try:
            v = int(t, 16)
        except ValueError:
            continue
        if 0 <= v <= 0xFF:
            out.append(v)
    return out


def hexbytes(byte_list):
    return " ".join(f"{b:02X}" for b in byte_list)


def decode_u32_le(byte_list, offset):
    if len(byte_list) < offset + 4:
        return None
    b = byte_list[offset:offset + 4]
    return b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)


def run_tshark(tshark, pcap, display_filter, fields):
    cmd = [tshark, "-r", pcap, "-Y", display_filter, "-T", "fields"]
    for f in fields:
        cmd += ["-e", f]
    cmd += ["-E", "separator=\t"]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as e:
        sys.exit("tshark failed:\n" + e.stderr)
    return out.stdout.splitlines()


def parse(pcap, tshark, vid, addr):
    # usbmon captures a whole bus, so a laptop's camera / fingerprint / BT
    # share the file. Narrow to the device's address when known (get it from
    # `lsusb -d 1dd2:` -> "Bus 0NN Device 0MM", pass MM as --addr).
    addr_clause = f"usb.device_address == {addr} && " if addr else ""

    # --- HID control transfers (SET_REPORT 0x09 host->dev, GET_REPORT 0x01) ---
    ctrl_filter = (f"{addr_clause}"
                   "(usb.setup.bRequest == 0x09 || usb.setup.bRequest == 0x01)")
    rows = run_tshark(tshark, pcap, ctrl_filter, [
        "frame.number", "frame.time_relative",
        "usb.bmRequestType", "usb.setup.bRequest",
        "usb.setup.wValue", "usb.capdata", "usb.data_fragment",
    ])

    print("=== HID Feature reports (control transfers) ===")
    print("opcode = payload byte 0 (these devices read the command from the")
    print("first data byte; the HID report-ID in wValue may just be 0).\n")
    print(f"{'#':>5} {'time':>10}  {'dir':<4} {'req':<11} {'rptID':>5} "
          f"{'op':>4}  annotation")
    print("-" * 78)
    seen_opcodes = set()
    for line in rows:
        cols = line.split("\t")
        cols += [""] * (7 - len(cols))
        num, t, brt, breq, wval, capdata, datafrag = cols[:7]
        try:
            wval_i = int(wval, 16) if wval else 0
        except ValueError:
            wval_i = 0
        report_id = wval_i & 0xFF
        breq_i = int(breq, 16) if breq else 0
        direction = "OUT" if breq_i == 0x09 else "IN"
        reqname = "SET_REPORT" if breq_i == 0x09 else "GET_REPORT"

        # The OUT payload may land in usb.capdata or usb.data_fragment
        # depending on tshark version / usbmon framing -- take whichever
        # is present.
        databytes = parse_capdata(capdata) or parse_capdata(datafrag)
        # The real opcode is the first payload byte; fall back to the
        # wValue report-id only if there's no payload (e.g. GET_REPORT).
        opcode = databytes[0] if databytes else report_id
        anno = OPCODE_NAMES.get(opcode, "*** UNKNOWN opcode ***")
        seen_opcodes.add(opcode)

        extra = ""
        # Frequency commands carry a LE u32; show both 1420 (offset 1) and
        # 1421 (offset 5) interpretations so the layout is obvious.
        if opcode in (0x03, 0x04, 0x05, 0x06, 0x09, 0x0A) and databytes:
            f1 = decode_u32_le(databytes, 1)
            f5 = decode_u32_le(databytes, 5)
            parts = []
            if f1 is not None:
                parts.append(f"@1={f1}")
            if f5 is not None:
                parts.append(f"@5={f5}")
            if parts:
                extra = "  freqLE " + " ".join(parts)

        tfmt = f"{float(t):.3f}" if t else ""
        print(f"{num:>5} {tfmt:>10}  {direction:<4} {reqname:<11} "
              f"0x{report_id:02X} 0x{opcode:02X}  {anno}{extra}")
        if databytes:
            print(f"{'':>5} {'':>10}  data: {hexbytes(databytes)}")

    # --- interrupt-IN (GPS stream: NMEA over CDC, or UBX over HID) ---
    print("\n=== Interrupt-IN frames (first 20; GPS stream?) ===")
    int_rows = run_tshark(tshark, pcap,
                          f"{addr_clause}usb.transfer_type == 0x01 && "
                          "usb.endpoint_address.direction == 1",
                          ["frame.number", "usb.capdata"])
    shown = 0
    for line in int_rows:
        cols = line.split("\t")
        cols += [""] * (2 - len(cols))
        num, capdata = cols[:2]
        databytes = parse_capdata(capdata)
        if not databytes:
            continue
        hint = ""
        if databytes[:2] == [0xB5, 0x62]:
            hint = "  <- UBX sync (b5 62) -- Mini-style"
        elif databytes[0] == ord("$"):
            hint = "  <- NMEA '$' -- 1421-style"
        ascii_preview = "".join(
            chr(c) if 0x20 <= c < 0x7F else "." for c in databytes[:24])
        print(f"{num:>5}  {hexbytes(databytes)[:48]:<48} |{ascii_preview}|{hint}")
        shown += 1
        if shown >= 20:
            break
    if shown == 0:
        print("  (none -- GPS stream is probably on a CDC/ACM serial port, "
              "not HID. Check /dev/ttyACM*.)")

    # --- verdict ---
    print("\n=== Verdict ===")
    unknown = sorted(o for o in seen_opcodes if o not in OPCODE_NAMES)
    new1425 = sorted(o for o in seen_opcodes if o in LBE1425_NEW_OPCODES)
    if not seen_opcodes:
        print("No HID feature reports seen. Either the capture missed the "
              "control transfers or this device uses a different mechanism.")
        return
    if unknown:
        print("Fully-unknown opcodes seen: " +
              ", ".join(f"0x{o:02X}" for o in unknown) +
              "  -> new protocol bits to map.")
    if new1425:
        print("LBE-1425-specific opcodes seen: " +
              ", ".join(f"0x{o:02X}" for o in new1425) +
              "  -> beyond the 1421 set; see docs/reverse/LBE-1425-config-v1.10.md.")
    if not unknown and not new1425:
        print("All opcodes match the known 1420/1421 map -> reuses the 1421 "
              "wire protocol. Confirm freq offsets above (@5 = 1421 layout).")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pcap", help="usbmon/USBPcap capture file (.pcapng/.pcap)")
    ap.add_argument("--vid", default="0x1dd2",
                    help="USB vendor id to focus on (default 0x1dd2 = Leo Bodnar)")
    ap.add_argument("--addr", type=int, default=None,
                    help="usb device address to filter to (the 'Device NNN' "
                         "from lsusb); recommended for whole-bus usbmon captures")
    ap.add_argument("--tshark", default=None, help="path to tshark binary")
    args = ap.parse_args()

    tshark = args.tshark or shutil.which("tshark")
    if not tshark:
        sys.exit("tshark not found on PATH; install wireshark-cli or pass --tshark")

    try:
        vid = int(args.vid, 0)
    except ValueError:
        sys.exit(f"bad --vid: {args.vid}")

    parse(args.pcap, tshark, vid, args.addr)


if __name__ == "__main__":
    main()
