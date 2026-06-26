<!--
SPDX-License-Identifier: MIT
Copyright (c) 2024-2026 Benjamin Vernoux
-->
# LBE-1425 reverse-engineering plan

Working notes for adding LBE-1425 support. This is a live document: fill in the
"Evidence" sections as captures come in, then collapse the findings into code +
a `LBE-1425-config-vX.Y.md` evidence file (same style as the Mini's).

## What the LBE-1425 is (from the datasheet, no USB facts yet)

"Increased Stability Dual Output GPS Locked Clock Source", USB-C, ~Sept 2025.

| Property            | Value                                              |
|---------------------|----------------------------------------------------|
| Outputs             | 2 (independent)                                    |
| OUT1                | 1 Hz – **800 MHz**, switchable 1-PPS               |
| OUT2                | 1 Hz – **1.4 GHz**                                 |
| Stability           | ±5 ppb (-20…40 °C), 0.2 ppb/day holdover           |
| GNSS                | multi-constellation, ~30 s cold acquisition        |
| Antenna             | 3.3 V bias at SMA, max 30 mA                        |
| Power               | USB-C, 5 V max 250 mA                              |

Datasheet: <https://leobodnar.com/files/datasheets/LBE-1425-Datasheet-V1.0-Initial-26-09-2025.pdf>
Product page: <https://www.leobodnar.com/shop/index.php?main_page=product_info&products_id=639>

### Working hypothesis

The dual-output + 1PPS-on-OUT1 + (likely) NMEA-over-CDC profile matches the
**LBE-1421 / LBE-1423 family**, which this tool already supports
(`src/model_1421.c`). Most likely the 1425 reuses that HID-feature-report wire
format and we mainly need: a new PID, a model entry, and corrected per-output
frequency caps (OUT1 800 MHz vs OUT2 1.4 GHz — the 1421 path currently caps both
at a single `max_freq`).

Things that could break the hypothesis and must be checked, not assumed:
- **PID** is unknown. Existing: 1420=`0x2443`, 1421=`0x2444`, 1423=`0x226f`,
  Mini=`0x2211`. No obvious numeric pattern — must be read from the device.
- **Status report layout** (the 1421 reads report ID `0x4B`, freqs at byte
  offsets 6 and 14, flags at 18/19/20). A firmware revision can move these.
- **GNSS transport**: 1421 streams NMEA over a USB CDC ACM port; the Mini
  streams UBX over HID interrupt-IN. The 1425 could do either.
- **Per-output frequency limits** differ between OUT1 and OUT2 on this model.

## Test rig

- **Linux host**: device plugged in directly. Runs this tool, `lsusb`, and
  `usbmon`-based capture.
- **GNOME Boxes Windows VM**: runs the working vendor Windows tool. The device
  is passed through to the guest (USB redirection).

The trick that makes this fast: when the device is redirected into the Boxes
guest, the URBs still traverse the **host** kernel's USB stack, so the Linux
host can capture the vendor tool's traffic with `usbmon` while Windows drives
the device. That gives ground-truth opcodes for every operation without
touching Windows-side capture tooling.

## Experiment ladder

Run top to bottom. Each rung's output is recorded under "Evidence" below.

### Rung 0 — Identify the device (Linux host, no capture)

Plug the 1425 into the Linux host directly (not the VM) and run:

```sh
lsusb -d 1dd2:                       # get the PID
lsusb -v -d 1dd2:<pid> 2>/dev/null   # full descriptors
ls -l /dev/hidraw* /dev/ttyACM*      # which kernel nodes appeared
```

What we learn:
- **PID** → the `PID_LBE_1425` constant.
- **Interface classes**: HID-only (→ 1420/1421-style, status via feature
  reports) vs HID+CDC (→ NMEA monitor like the 1421) vs HID with a fat
  interrupt-IN endpoint (→ UBX like the Mini).
- **Endpoints**: an interrupt-IN endpoint address (e.g. `0x81`) is the `ep`
  argument for `--rawdump`.

> If `lsusb -v` needs root for the full config descriptor, run it with `sudo`.

### Rung 1 — Probe with this tool (Linux host)

Once the PID is known, the existing build can talk to it if we point it at the
PID. The tool currently only opens *known* PIDs, so until `PID_LBE_1425` is
compiled in, use `--rawdump` against the Mini/1421 code paths is not possible
for an unknown PID — see "Tooling gaps" below for the `--probe` change that
removes this limitation.

After `PID_LBE_1425` is added (even mapped provisionally to the 1421 ops):

```sh
lbe-142x --pid 0x<pid> --status      # does the 0x4B status read decode sanely?
lbe-142x --pid 0x<pid> --rawdump 0x81 3000   # any HID interrupt-IN stream?
```

Sane `--status` output (plausible GPS/PLL/antenna bits, a real frequency)
confirms the 1421 wire format. Garbage means the status layout moved — capture
it (Rung 2) and re-map the offsets.

### Rung 2 — Capture the vendor tool (host usbmon while VM drives)

This is the authoritative source. On the Linux host:

```sh
sudo modprobe usbmon
# find the bus the device is on:
lsusb -d 1dd2:<pid>          # note "Bus 0NN Device 0MM"
# capture that bus (Wireshark or tshark); usbmonNN where NN = bus number:
sudo tshark -i usbmon<NN> -w lbe1425-<operation>.pcapng
```

Then, in the Boxes Windows VM with the device passed through, perform **one
operation at a time** in the vendor tool and stop the capture. One pcap per
operation keeps the diff clean:

1. open / connect (baseline poll, status read)
2. set OUT1 = 10 MHz (save)
3. set OUT1 = 10 MHz temporary (no save), if the vendor UI exposes it
4. set OUT2 = 10 MHz
5. set OUT2 = some other freq
6. outputs off, then on
7. 1PPS on, then off
8. OUT1 power low, then normal (and OUT2)
9. PLL ↔ FLL toggle
10. blink / identify
11. live GPS view running (to capture the NMEA/UBX stream + endpoint)

For each capture, the interesting URBs are **GET_REPORT / SET_REPORT** control
transfers (`bmRequestType` 0x21/0xA1, `bRequest` 0x09/0x01, `wValue` high byte
= report type, low byte = report ID) and any interrupt-IN data. Extract them:

```sh
# control SET_REPORT payloads (host→device):
tshark -r lbe1425-<op>.pcapng -Y 'usb.bmRequestType == 0x21 && usb.setup.bRequest == 0x09' \
       -T fields -e usb.setup.wValue -e usb.capdata
# control GET_REPORT responses (device→host):
tshark -r lbe1425-<op>.pcapng -Y 'usb.bmRequestType == 0xa1 && usb.setup.bRequest == 0x01' \
       -T fields -e usb.setup.wValue -e usb.capdata
```

Or just run the helper, which does all of the above and annotates each report
against the known opcode map (`python/parse_usb_capture.py`):

```sh
python3 python/parse_usb_capture.py lbe1425-<op>.pcapng
```

It prints every Feature report with its decoded opcode, both candidate
frequency-field offsets (@1 = 1420 layout, @5 = 1421 layout), any interrupt-IN
GPS frames (flagging UBX `b5 62` vs NMEA `$`), and a verdict on whether all
opcodes match the existing 1421 map.

Compare the `wValue` low byte (report ID / opcode) and payload bytes against
`include/lbe_common.h`. If they match the `LBE_1421_*` opcodes and offsets, the
1425 is a straight 1421 clone (+ freq caps). If not, this is where we learn the
new map.

### Rung 3 — Decode the GPS stream

From Rung 0 we know HID-interrupt vs CDC. Then:
- **CDC / NMEA** (like 1421): find the port (`/dev/ttyACM*`), confirm `$G…`
  sentences, confirm 1PPS shows up on the DCD modem line
  (`python/read_nmea.py` and `--monitor` already do this).
- **HID / UBX** (like Mini): `--rawdump <ep>` should show `B5 62` UBX frames;
  reuse the Mini's UBX parser.

## Tooling gaps to close (this tool, device-independent — safe to write now)

1. **`PID_LBE_1425` + provisional model mapping.** Add the constant and map it
   to `lbe_ops_1421` in `lbe_device.c` (mirrors how 1423 was handled) so the
   tool can open and probe it. Cheap, reversible.
2. **`--probe` mode that ignores the known-PID allowlist.** Today
   `lbe_transport_open` only matches the hardcoded PID list, so an unknown 1425
   can't be opened by our tool at all. A `--probe` path that opens *any*
   `0x1dd2` HID device and dumps descriptors + tries a `0x4B` status read +
   rawdumps the first interrupt-IN endpoint would let us classify the device
   before we commit a PID. Compile-checkable here; only validatable on the
   user's hardware.
3. **Per-output frequency caps.** OUT1 ≤ 800 MHz, OUT2 ≤ 1.4 GHz. The current
   `main.c` uses one `max_freq` for both outputs; the 1425 needs per-output
   limits (either in `lbe_model_ops` or a small validation hook).

## Evidence (fill in as captures arrive)

### Rung 0 — descriptors
> _PID:_
> _Interfaces / classes:_
> _Endpoints:_
> _Kernel nodes:_

### Rung 1 — `--status` / `--rawdump`
> _Status decode plausible?_
> _Interrupt-IN stream?_

### Rung 2 — vendor-tool opcode map
| Operation | wValue (report id) | payload bytes | matches LBE_1421_*? |
|-----------|--------------------|---------------|---------------------|
|           |                    |               |                     |

### Rung 3 — GPS stream
> _Transport (CDC NMEA / HID UBX):_
> _Port or endpoint:_
> _1PPS carrier:_

## Open questions

- Is the status report ID still `0x4B`, and are the freq/flag offsets unchanged?
- Does OUT1's 800 MHz ceiling need enforcement in firmware-rejected ranges, or
  just client-side validation?
- "Increased stability" — does the holdover behaviour expose any new
  status/telemetry field the vendor UI shows (cf. the Mini's "signal loss
  count")?
