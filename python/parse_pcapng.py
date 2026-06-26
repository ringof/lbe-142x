#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2026 Benjamin Vernoux
#
# Standalone pcapng + usbmon decoder for LBE-142x reverse engineering.
# NO tshark / wireshark / pcap library needed -- parses the pcapng blocks and
# the Linux usbmon (DLT 220) headers directly. Decodes HID Feature writes
# (SET_REPORT) and reads (GET_REPORT), the EP 0x83 interrupt-IN telemetry
# stream, the 1425 GNSS/dynModel commands, and flags unknown opcodes.
#
# Capture (keep the file in /tmp -- the tshark AppArmor profile blocks $HOME,
# but this tool doesn't use tshark so any path works for it):
#   sudo modprobe usbmon
#   sudo dumpcap -i usbmon3 -w /tmp/cap.pcapng      # Ctrl-C when done
#   python3 python/parse_pcapng.py /tmp/cap.pcapng --dev 63
#
# Or read the raw usbmon binary node directly (no dumpcap):
#   sudo cat /sys/kernel/debug/usb/usbmon/3u  ... -> use usbmon_live.py instead.

import argparse
import struct
import sys

OPCODE_NAMES = {
    0x01: "EN_OUT", 0x02: "BLINK", 0x03: "SET_GNSS(1425)", 0x04: "SET_DYNMODEL(1425)",
    0x05: "SET_F1_TEMP", 0x06: "SET_F1", 0x08: "TELEMETRY_POLL(1425)",
    0x09: "SET_F2_TEMP", 0x0A: "SET_F2", 0x0B: "SET_PLL", 0x0C: "SET_PPS",
    0x0D: "SET_PWR1", 0x0E: "SET_PWR2", 0x0F: "SET_NMEA(1425)",
}
KNOWN_OPCODES = set(OPCODE_NAMES)
GNSS_BITS = [(0x01, "GPS"), (0x02, "SBAS"), (0x04, "Gal"),
             (0x08, "BeiDou"), (0x40, "GLONASS")]
DYNMODEL = {0: "Portable", 2: "Stationary", 3: "Pedestrian", 4: "Automotive",
            5: "Sea", 6: "Airborne<1g", 7: "Airborne<2g", 8: "Airborne<4g"}

# The EP 0x83 interrupt stream is UBX, HID-framed as [seq][0x3E=len][62 payload].
UBX_NAMES = {
    (0x01, 0x07): "NAV-PVT", (0x01, 0x22): "NAV-CLOCK", (0x01, 0x35): "NAV-SAT",
    (0x01, 0x21): "NAV-TIMEUTC", (0x01, 0x03): "NAV-STATUS", (0x01, 0x02): "NAV-POSLLH",
    (0x01, 0x26): "NAV-TIMELS", (0x0A, 0x04): "MON-VER", (0x05, 0x01): "ACK-ACK",
    (0x05, 0x00): "ACK-NAK",
}


def ubx_scan(frames):
    """De-frame EP 0x83 (strip the [seq][len] 2-byte HID header per frame),
    reassemble, and yield (class, id, name, payload) for each valid UBX msg."""
    stream = b"".join(f[2:2 + f[1]] for f in frames if len(f) >= 2 and f[1] <= 62)
    i, n = 0, len(stream)
    while i < n - 8:
        if stream[i] == 0xB5 and stream[i + 1] == 0x62:
            cls, mid = stream[i + 2], stream[i + 3]
            ln = stream[i + 4] | (stream[i + 5] << 8)
            if i + 8 + ln <= n:
                body = stream[i + 2:i + 6 + ln]
                a = b = 0
                for x in body:
                    a = (a + x) & 0xFF
                    b = (b + a) & 0xFF
                if a == stream[i + 6 + ln] and b == stream[i + 7 + ln]:
                    yield cls, mid, UBX_NAMES.get((cls, mid), f"{cls:02X}{mid:02X}"), \
                        stream[i + 6:i + 6 + ln]
                    i += 8 + ln
                    continue
        i += 1


def read_pcapng(path):
    """Yield (relative_ts_seconds, usbmon_packet_bytes) for each Enhanced
    Packet Block in a Linux-usbmon (DLT 220) pcapng."""
    data = open(path, "rb").read()
    if data[0:4] != b"\x0a\x0d\x0d\x0a":
        sys.exit("not a pcapng file")
    le = data[8:12] == b"\x4d\x3c\x2b\x1a"
    e = "<" if le else ">"
    u32 = lambda o: struct.unpack_from(e + "I", data, o)[0]
    off = 0
    while off + 12 <= len(data):
        btype = u32(off)
        blen = u32(off + 4)
        if blen < 12 or off + blen > len(data):
            break
        if btype == 0x00000006:                 # Enhanced Packet Block
            body = data[off + 8:off + blen - 4]
            caplen = struct.unpack_from(e + "I", body, 12)[0]
            yield body[20:20 + caplen]
        off += blen


def parse_usbmon(pkt):
    if len(pkt) < 64:
        return None
    lc = struct.unpack_from("<I", pkt, 36)[0]
    return {
        "id": struct.unpack_from("<Q", pkt, 0)[0],   # URB id: pairs submit<->complete
        "type": chr(pkt[8]),                     # S submit / C complete / E error
        "xfer": pkt[9],                          # 0 iso 1 intr 2 ctrl 3 bulk
        "ep": pkt[10],                           # bit7 = IN
        "dev": pkt[11],
        "setup": pkt[40:48],                     # bmRT bReq wVal wIdx wLen
        "ts": struct.unpack_from("<q", pkt, 16)[0] + struct.unpack_from("<i", pkt, 24)[0] / 1e6,
        "data": bytes(pkt[64:64 + lc]),
    }


def hx(b, n=None):
    return " ".join(f"{x:02X}" for x in (b[:n] if n else b))


def annotate(op, data):
    if op in (0x05, 0x06, 0x09, 0x0A) and len(data) >= 9:
        return f"freq={struct.unpack_from('<I', data, 5)[0]} Hz"
    if op == 0x03 and len(data) > 1:
        on = "+".join(n for m, n in GNSS_BITS if data[1] & m) or "none"
        return f"mask=0x{data[1]:02X} [{on}]"
    if op == 0x04 and len(data) > 1:
        return f"dynModel={data[1]} ({DYNMODEL.get(data[1], '?')})"
    if op == 0x08 and len(data) > 6:
        return f"poll sel=0x{data[6]:02X}"
    if len(data) > 1:
        return f"arg=0x{data[1]:02X}"
    return ""


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pcap")
    ap.add_argument("--dev", type=int, default=None,
                    help="usb device address to filter to (the 'Device NNN' from lsusb)")
    ap.add_argument("--ubx", action="store_true",
                    help="instead of the timeline, de-frame EP 0x83 and list the "
                         "decoded UBX diagnostics messages")
    args = ap.parse_args()

    evs = []
    for pkt in read_pcapng(args.pcap):
        h = parse_usbmon(pkt)
        if h and (args.dev is None or h["dev"] == args.dev):
            evs.append(h)
    if not evs:
        sys.exit("no usbmon packets (wrong --dev? not a usbmon capture?)")
    t0 = evs[0]["ts"]

    ep83 = [h["data"] for h in evs
            if h["xfer"] == 1 and h["ep"] == 0x83 and len(h["data"]) == 64]

    if args.ubx:
        from collections import Counter
        print(f"=== EP 0x83 UBX diagnostics ({len(ep83)} HID frames) ===")
        counts = Counter()
        for cls, mid, name, body in ubx_scan(ep83):
            counts[name] += 1
        if not counts:
            print("no valid UBX found (is EP 0x83 in this capture?)")
        for name, c in counts.most_common():
            print(f"  {name:12} x{c}")
        return

    seen_ops, unknown = set(), set()
    pending_get = set()      # URB ids of in-flight GET_REPORT (report-id 0) reads
    print("=== timeline (writes + status reads) ===")
    print(f"{'t(s)':>8}  kind")
    print("-" * 76)
    for h in evs:
        t = h["ts"] - t0
        bm, req = h["setup"][0], h["setup"][1]
        # A GET_REPORT submit carries the setup; its data arrives on the
        # matching Complete (same URB id) -- track the id so we don't confuse
        # the response with an unrelated descriptor read.
        if h["xfer"] == 2 and h["type"] == "S" and bm == 0xA1 and req == 0x01:
            pending_get.add(h["id"])
            continue
        # HID SET_REPORT (write)
        if h["xfer"] == 2 and bm == 0x21 and req == 0x09 and h["data"]:
            op = h["data"][0]
            seen_ops.add(op)
            if op not in KNOWN_OPCODES:
                unknown.add(op)
            name = OPCODE_NAMES.get(op, f"op0x{op:02X} *** UNKNOWN ***")
            print(f"{t:8.2f}  WRITE  0x{op:02X} {name:20} {annotate(op, h['data'])}"
                  f"   [{hx(h['data'], 10)}]")
        # GET_REPORT response: the Complete of a tracked read
        elif h["type"] == "C" and h["id"] in pending_get:
            pending_get.discard(h["id"])
            d = h["data"]
            if len(d) >= 21:
                st, f1, f2 = d[1], struct.unpack_from("<I", d, 6)[0], struct.unpack_from("<I", d, 14)[0]
                print(f"{t:8.2f}  READ   status=0x{st:02X} f1={f1} f2={f2} "
                      f"fll={d[18]} pwr1={d[19]} pwr2={d[20]}  tail[21+]={hx(d[21:28])}")
            else:
                print(f"{t:8.2f}  READ   [{hx(d)}]")
    print("\n=== Verdict ===")
    print("opcodes seen: " + ", ".join(f"0x{o:02X}" for o in sorted(seen_ops)))
    if unknown:
        print("UNKNOWN opcodes (new!): " + ", ".join(f"0x{o:02X}" for o in sorted(unknown)))
    else:
        print("all write opcodes are in the known LBE-142x/1425 map")
    if ep83:
        ubx = list(ubx_scan(ep83))
        print(f"EP 0x83: {len(ep83)} HID frames -> {len(ubx)} valid UBX msgs "
              f"(run with --ubx for the breakdown)")


if __name__ == "__main__":
    main()
