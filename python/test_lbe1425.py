#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2024-2026 Benjamin Vernoux
#
# Integration test for the LBE-1425 CLI. Exercises every command we'd
# reasonably send and checks for bugs several ways:
#
#   1. round-trip  -- set a value, read it back via --status, assert it matches:
#                     frequency, outputs, PLL/FLL, PPS, power, and (since the
#                     1425 echoes them) the GNSS mask, dynamic model and NMEA.
#   2. status content -- serial present, antenna line well-formed.
#   3. read + live -- --blink, --gps-info (u-blox version), --probe-op, and a
#                     smoke test of the never-returning live modes (--statlog,
#                     --diag, --monitor) that they start rendering.
#   4. accept/reject -- valid inputs accepted, invalid rejected (frequency caps,
#                     bad enums, the BeiDou-vs-GPS/SBAS/Gal constraint).
#   5. opcode capture (optional, --usbmon) -- snoop usbmon and assert the exact
#                     opcode/argument each command puts on the wire.
#
# The device is left at the vendor factory defaults when the run finishes.
#
# Usage:
#   sudo python3 python/test_lbe1425.py                       # round-trip + accept/reject
#   sudo python3 python/test_lbe1425.py --usbmon              # + wire-level opcode checks
#   python3 python/test_lbe1425.py --self-test               # parser self-test, no device
#
# Needs sudo (hidraw + usbmon are root-only) and the built binary at
# ./build/bin/lbe-142x (override with --bin).

import argparse
import glob
import re
import subprocess
import sys
import threading
import time
from collections import deque

PID = "0x2269"
VID = 0x1DD2
PID_INT = 0x2269

# Vendor factory defaults (from the "factory reset" capture).
DEFAULTS = [
    ("--gnss", "0x47"), ("--dynmodel", "stationary"), ("--nmea", "1"),
    ("--pll", "0"), ("--pps", "0"), ("--pwr1", "0"), ("--pwr2", "0"),
    ("--out", "1"),
]


# ---------------------------------------------------------------- result tally
class Tally:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.fails = []

    def check(self, name, ok, detail=""):
        if ok:
            self.passed += 1
            print(f"  PASS  {name}")
        else:
            self.failed += 1
            self.fails.append(name)
            print(f"  FAIL  {name}   {detail}")

    def summary(self):
        print(f"\n{'='*60}\n{self.passed} passed, {self.failed} failed")
        for f in self.fails:
            print(f"  - {f}")
        return self.failed == 0


# ---------------------------------------------------------------- CLI driver
def run_cli(binp, *args, timeout=20):
    cmd = [binp, "--pid", PID, *args]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout + p.stderr


STATUS_FIELDS = {
    "out1_freq":  (r"OUT1 Frequency:\s*(\d+)", int),
    "out2_freq":  (r"OUT2 Frequency:\s*(\d+)", int),
    "out1_power": (r"OUT1 Power Level:\s*(\w+)", str),
    "out2_power": (r"OUT2 Power Level:\s*(\w+)", str),
    "outputs":    (r"Output\(s\) Enabled:\s*(\w+)", str),
    "pps":        (r"1PPS on OUT1:\s*(\w+)", str),
    "mode":       (r"Mode:\s*(\w+)", str),
    # 1425-specific status lines (config echoed back in the report tail)
    "gnss_mask":  (r"GNSS:\s*(0x[0-9A-Fa-f]+)", lambda x: int(x, 16)),
    "dynmodel":   (r"Dynamic model:.*\((\d+)\)", int),
    "nmea":       (r"NMEA output:\s*(\w+)", str),
    "serial":     (r"Serial:\s*(\S+)", str),
    "antenna":    (r"Antenna:\s*(.+)", str),
}


def parse_status(text):
    st = {}
    m = re.search(r"Device Status \((0x[0-9A-Fa-f]+)\)", text)
    if m:
        st["raw"] = int(m.group(1), 16)
    for key, (pat, conv) in STATUS_FIELDS.items():
        m = re.search(pat, text)
        if m:
            st[key] = conv(m.group(1))
    return st


def get_status(binp):
    _, out = run_cli(binp, "--status")
    return parse_status(out)


# ---------------------------------------------------------------- usbmon snoop
def find_bus_dev():
    out = subprocess.run(["lsusb", "-d", "1dd2:2269"], capture_output=True, text=True).stdout
    m = re.search(r"Bus (\d+) Device (\d+):", out)
    if not m:
        return None, None
    return int(m.group(1)), int(m.group(2))


class UsbmonWatcher:
    """Background reader of /sys/kernel/debug/usb/usbmon/<bus>u that records the
    SET_REPORT (opcode, arg, payload) writes to our device."""
    def __init__(self, bus, dev):
        self.dev = dev
        self.node = f"/sys/kernel/debug/usb/usbmon/{bus}u"
        self.events = deque(maxlen=2000)
        self._stop = False
        self._t = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self._t.start()

    def stop(self):
        self._stop = True

    def _run(self):
        try:
            f = open(self.node)
        except OSError as e:
            print(f"  (usbmon open failed: {e}; opcode checks skipped)")
            return
        while not self._stop:
            line = f.readline()
            if not line:
                break
            ev = self._parse(line)
            if ev:
                self.events.append((time.time(), *ev))

    def _parse(self, line):
        tok = line.split()
        if len(tok) < 5:
            return None
        addr = tok[3].split(":")
        if len(addr) < 4 or not addr[0].startswith("C"):
            return None
        try:
            dev = int(addr[2])
        except ValueError:
            return None
        if dev != self.dev or tok[4] != "s":
            return None
        try:
            bm, req = int(tok[5], 16), int(tok[6], 16)
        except (ValueError, IndexError):
            return None
        if bm != 0x21 or req != 0x09:           # SET_REPORT only
            return None
        # setup occupies tok[4..9] ('s' + bmRT bReq wVal wIdx wLen); then the
        # data length (tok[10]), the '=' tag (tok[11]) and the data words.
        if len(tok) < 12 or tok[11] != "=":
            return None
        data = bytes.fromhex("".join(tok[12:]))
        if not data:
            return None
        return data[0], (data[1] if len(data) > 1 else 0), data

    def opcodes_since(self, t0):
        return [(op, arg, d) for (ts, op, arg, d) in list(self.events) if ts >= t0]


# ---------------------------------------------------------------- test phases
def test_roundtrip(binp, t):
    print("\n-- round-trip via --status --")
    # PPS off first: enabling PPS reconfigures OUT1, which would skew freq tests.
    run_cli(binp, "--pps", "0")

    # Temporary frequencies (not flashed) round-trip through status.
    for out, opt, freqs in [(1, "--f1t", [10_000_000, 100_000_000, 800_000_000]),
                            (2, "--f2t", [10_000_000, 250_000_000, 1_400_000_000])]:
        for f in freqs:
            run_cli(binp, opt, str(f))
            st = get_status(binp)
            key = f"out{out}_freq"
            t.check(f"{opt} {f} -> status {key}={st.get(key)}",
                    st.get(key) == f, f"got {st.get(key)}")

    for val, want in [("0", "No"), ("1", "Yes")]:
        run_cli(binp, "--out", val)
        t.check(f"--out {val} -> outputs {want}", get_status(binp).get("outputs") == want)

    for val, want in [("1", "FLL"), ("0", "PLL")]:
        run_cli(binp, "--pll", val)
        t.check(f"--pll {val} -> mode {want}", get_status(binp).get("mode") == want)

    for opt, field in [("--pwr1", "out1_power"), ("--pwr2", "out2_power")]:
        for val, want in [("1", "Low"), ("0", "Normal")]:
            run_cli(binp, opt, val)
            t.check(f"{opt} {val} -> {field} {want}", get_status(binp).get(field) == want)

    for val, want in [("1", "Enabled"), ("0", "Disabled")]:
        run_cli(binp, "--pps", val)
        t.check(f"--pps {val} -> pps {want}", get_status(binp).get("pps") == want)

    # 1425 config the status report echoes back (GNSS mask / dynModel / NMEA).
    for mask in [0x47, 0x01, 0x40, 0x21, 0x48]:
        run_cli(binp, "--gnss", f"0x{mask:02X}")
        got = get_status(binp).get("gnss_mask")
        t.check(f"--gnss 0x{mask:02X} -> status GNSS 0x{(got or 0):02X}", got == mask)

    for name, val in [("portable", 0), ("stationary", 2), ("pedestrian", 3),
                      ("automotive", 4), ("airborne", 8)]:
        run_cli(binp, "--dynmodel", name)
        got = get_status(binp).get("dynmodel")
        t.check(f"--dynmodel {name} -> status dynModel {got}", got == val)

    for val, want in [("0", "Disabled"), ("1", "Enabled")]:
        run_cli(binp, "--nmea", val)
        t.check(f"--nmea {val} -> NMEA {want}", get_status(binp).get("nmea") == want)


def test_status_content(binp, t):
    """The 1425 --status should carry a serial and a parseable antenna line."""
    print("\n-- status content --")
    st = get_status(binp)
    t.check("serial present", bool(st.get("serial")), f"got {st.get('serial')!r}")
    ant = st.get("antenna", "")
    ok = bool(re.match(r"(OK \(\d+ mA\)|Not connected \(0 mA\)|Short Circuit)", ant))
    t.check(f"antenna line well-formed: {ant!r}", ok)


def run_timed(binp, args, seconds):
    """Launch the binary directly (script already runs as root), let it run for
    `seconds`, then stop it and return whatever it printed. For the
    never-returning live modes (--diag/--monitor/--statlog)."""
    p = subprocess.Popen([binp, "--pid", PID, *args],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(seconds)
    p.terminate()
    try:
        out, _ = p.communicate(timeout=3)
    except subprocess.TimeoutExpired:
        p.kill()
        out, _ = p.communicate()
    return out


def test_read_and_live(binp, t):
    print("\n-- read + live commands --")
    # one-shot reads
    _, out = run_cli(binp, "--blink")
    t.check("--blink runs", "Blink" in out, out.strip()[-80:])
    _, out = run_cli(binp, "--gps-info", timeout=15)
    t.check("--gps-info returns u-blox version", "SW version" in out, out.strip()[-80:])
    _, out = run_cli(binp, "--probe-op", "0x07", "01")
    t.check("--probe-op runs (0x07 -> no status change)",
            "no status change" in out or "byte" in out, out.strip()[-80:])

    # never-returning live modes: smoke-test that each starts producing output
    out = run_timed(binp, ["--statlog"], 3)
    t.check("--statlog produces rows", "bytes[18..40]" in out or "0x" in out, out.strip()[-80:])
    out = run_timed(binp, ["--diag"], 5)
    t.check("--diag renders", "GPS Diagnostics" in out, out.strip()[-80:])
    out = run_timed(binp, ["--monitor"], 5)
    t.check("--monitor renders (needs /dev/ttyACM*)",
            "GPS Monitor" in out, out.strip()[-120:])


def is_rejected(out):
    return bool(re.search(r"Invalid|not supported|cannot be combined|does not support", out))


def test_accept_reject(binp, t):
    print("\n-- accept / reject --")
    # frequency caps: OUT1 <= 800 MHz, OUT2 <= 1.4 GHz. Use the temporary
    # variants (--f1t/--f2t): identical validation path, no flash wear.
    for opt, val, ok in [("--f1t", "800000000", True), ("--f1t", "900000000", False),
                         ("--f2t", "1400000000", True), ("--f2t", "1500000000", False),
                         ("--f1t", "0", False)]:
        _, out = run_cli(binp, opt, val)
        rej = is_rejected(out)
        t.check(f"{opt} {val} {'accepted' if ok else 'rejected'}", rej != ok, out.strip()[-80:])

    # invalid enums
    for opt, val in [("--out", "2"), ("--pll", "2"), ("--pps", "2"),
                     ("--pwr1", "5"), ("--nmea", "2")]:
        _, out = run_cli(binp, opt, val)
        t.check(f"{opt} {val} rejected", is_rejected(out), out.strip()[-80:])

    # GNSS: valid masks accepted, constraint + range enforced
    for val, ok in [("0x47", True), ("0x41", True), ("0x08", True), ("0x48", True),
                    ("0x00", True), ("0x0F", False), ("0x09", False), ("0x1FF", False)]:
        _, out = run_cli(binp, "--gnss", val)
        rej = is_rejected(out)
        t.check(f"--gnss {val} {'accepted' if ok else 'rejected'}", rej != ok, out.strip()[-80:])

    # dynamic model: names + numeric accepted, garbage rejected
    for val, ok in [("portable", True), ("stationary", True), ("airborne", True),
                    ("pedestrian", True), ("8", True), ("foo", False), ("999", False)]:
        _, out = run_cli(binp, "--dynmodel", val)
        rej = is_rejected(out)
        t.check(f"--dynmodel {val} {'accepted' if ok else 'rejected'}", rej != ok, out.strip()[-80:])

    for val in ["0", "1"]:
        _, out = run_cli(binp, "--nmea", val)
        t.check(f"--nmea {val} accepted", not is_rejected(out), out.strip()[-80:])


def test_opcodes(binp, t, w):
    print("\n-- wire-level opcode capture (usbmon) --")
    # (args, expected opcode, expected arg byte or None, expected freq@5 or None)
    cases = [
        (["--out", "1"],         0x01, 0x03, None),
        (["--out", "0"],         0x01, 0x00, None),
        (["--blink"],            0x02, None, None),
        (["--f1t", "12000000"],  0x05, None, 12000000),
        (["--f2t", "13000000"],  0x09, None, 13000000),
        (["--pll", "1"],         0x0B, 0x01, None),
        (["--pps", "0"],         0x0C, 0x00, None),
        (["--pwr1", "1"],        0x0D, 0x01, None),
        (["--pwr2", "0"],        0x0E, 0x00, None),
        (["--gnss", "0x41"],     0x03, 0x41, None),
        (["--dynmodel", "airborne"], 0x04, 0x08, None),
        (["--nmea", "0"],        0x0F, 0x00, None),
    ]
    for args, op, arg, freq in cases:
        t0 = time.time()
        run_cli(binp, *args)
        time.sleep(0.3)
        evs = [e for e in w.opcodes_since(t0) if e[0] == op]
        if not evs:
            t.check(f"{' '.join(args)} -> opcode 0x{op:02X}", False, "no matching SET_REPORT seen")
            continue
        _, a, data = evs[-1]
        ok = True
        detail = f"got op=0x{op:02X} arg=0x{a:02X}"
        if arg is not None and a != arg:
            ok = False
        if freq is not None:
            got_f = int.from_bytes(data[5:9], "little") if len(data) >= 9 else -1
            if got_f != freq:
                ok = False
                detail += f" freq={got_f}"
        t.check(f"{' '.join(args)} -> opcode 0x{op:02X}"
                + (f" arg 0x{arg:02X}" if arg is not None else "")
                + (f" freq {freq}" if freq is not None else ""), ok, detail)


def restore_defaults(binp):
    print("\n-- restoring factory defaults --")
    for opt, val in DEFAULTS:
        run_cli(binp, opt, val)
    run_cli(binp, "--f1", "10000000")
    run_cli(binp, "--f2", "10000000")


# ---------------------------------------------------------------- self-test
def self_test():
    """Validate the parsers without a device."""
    t = Tally()
    sample = """lbe-142x v1.3 ...
Connected to LBE-1425 dual output
  Serial: 0C7BB80E70E5
Device Status (0xEE):
  GPS Lock: No
  PLL Lock: Yes
  Antenna: OK (5 mA)
  Output(s) Enabled: Yes
  OUT1 Frequency: 10000000 Hz
  OUT1 Power Level: Normal
  OUT2 Frequency: 27000000 Hz
  OUT2 Power Level: Low
  1PPS on OUT1: Enabled
  Mode: FLL
  GNSS: 0x47 (GPS SBAS Galileo GLONASS)
  Dynamic model: Stationary (2)
  NMEA output: Enabled
"""
    st = parse_status(sample)
    t.check("status raw 0xEE", st.get("raw") == 0xEE)
    t.check("out1_freq", st.get("out1_freq") == 10000000)
    t.check("out2_freq", st.get("out2_freq") == 27000000)
    t.check("out2_power Low", st.get("out2_power") == "Low")
    t.check("outputs Yes", st.get("outputs") == "Yes")
    t.check("pps Enabled", st.get("pps") == "Enabled")
    t.check("mode FLL", st.get("mode") == "FLL")
    t.check("serial parsed", st.get("serial") == "0C7BB80E70E5")
    t.check("antenna parsed", st.get("antenna") == "OK (5 mA)")
    t.check("gnss_mask 0x47", st.get("gnss_mask") == 0x47)
    t.check("dynmodel 2", st.get("dynmodel") == 2)
    t.check("nmea Enabled", st.get("nmea") == "Enabled")
    t.check("reject detects Invalid", is_rejected("Invalid frequency: 900000000"))
    t.check("reject detects constraint", is_rejected("BeiDou (0x08) cannot be combined"))
    t.check("accept passes clean", not is_rejected("  Set GNSS mask to 0x41"))

    # usbmon line parser
    w = UsbmonWatcher.__new__(UsbmonWatcher)
    w.dev = 63
    ev = w._parse("ffff 100 S Co:3:063:0 s 21 09 0300 0002 003c 60 = 0c010000 00000000")
    t.check("usbmon parse opcode 0x0C", ev and ev[0] == 0x0C and ev[1] == 0x01)
    ev2 = w._parse("ffff 100 C Ci:3:063:0 0 60 = ...")  # not a SET_REPORT submit
    t.check("usbmon ignores non-SET_REPORT", ev2 is None)
    ev3 = w._parse("ffff 100 S Co:3:099:0 s 21 09 0300 0000 0007 7 = 80250000 000008")
    t.check("usbmon filters other devices", ev3 is None)
    return t.summary()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", default="./build/bin/lbe-142x")
    ap.add_argument("--usbmon", action="store_true",
                    help="also verify the exact opcode each command emits (needs root)")
    ap.add_argument("--self-test", action="store_true",
                    help="validate the parsers with built-in samples; no device needed")
    ap.add_argument("--no-restore", action="store_true",
                    help="leave device as-is instead of restoring factory defaults")
    args = ap.parse_args()

    if args.self_test:
        sys.exit(0 if self_test() else 1)

    # sanity: device reachable?
    rc, out = run_cli(args.bin, "--status")
    if "Connected to LBE-1425" not in out:
        sys.exit(f"LBE-1425 not reachable via {args.bin} (run with sudo?):\n{out}")

    t = Tally()
    watcher = None
    if args.usbmon:
        subprocess.run(["modprobe", "usbmon"], capture_output=True)
        bus, dev = find_bus_dev()
        if bus is None:
            print("  (could not find device on a bus; opcode checks skipped)")
        else:
            watcher = UsbmonWatcher(bus, dev)
            watcher.start()
            time.sleep(0.3)

    try:
        test_roundtrip(args.bin, t)
        test_status_content(args.bin, t)
        test_accept_reject(args.bin, t)
        test_read_and_live(args.bin, t)
        if watcher:
            test_opcodes(args.bin, t, watcher)
    finally:
        if watcher:
            watcher.stop()
        if not args.no_restore:
            restore_defaults(args.bin)

    sys.exit(0 if t.summary() else 1)


if __name__ == "__main__":
    main()
