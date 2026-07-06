<!--
SPDX-License-Identifier: MIT
-->
# LBE-1420 reverse-engineering plan

Working notes for bringing the LBE-1420 GPSDO to the same reverse-engineering
depth as the LBE-1425. This is a live document: fill in the "Evidence" sections
as captures come in, then collapse the findings into code + an
`LBE-1420-config-vX.Y.md` evidence file.

Tracks issue #23.

## What the LBE-1420 is

"Precision GPS Reference Clock", single-output, USB. The oldest model in the
LBE-142x family. Product page:
<https://www.leobodnar.com/shop/index.php?main_page=product_info&products_id=234>

| Property            | Value                                    |
|---------------------|------------------------------------------|
| Outputs             | 1                                        |
| OUT1                | 400 Hz – 1.6 GHz                         |
| 1PPS                | not supported                            |
| GNSS                | GPS (unknown if multi-constellation)     |
| Antenna             | 3.3 V bias at SMA                        |
| PID                 | `0x2443`                                 |

### What we know today (code-inferred, not capture-verified)

From `src/model_1420.c` and `include/lbe_common.h`:

- **Status read**: report ID `0x4B`, 60 bytes. Freq1 at bytes 6–9 (LE32),
  FLL flag at byte 18, OUT1 power at byte 10. Status bits at byte 1 use
  `LBE_PLL_LOCK_BIT` (bit 1) and `LBE_ANT_OK_BIT` (bit 2) only.
- **Frequency at byte 1** (not byte 5 like the 1421): opcode `0x03` (temp) /
  `0x04` (save), freq LE32 at payload bytes 1–4.
- **Power**: opcode `0x07`, arg byte 1 = 0 normal / 1 low.
- **Shared opcodes**: `0x01` EN_OUT, `0x02` BLINK, `0x0B` PLL/FLL.
- **Hardcoded assumptions**:
  - `outputs_enabled = 1` always (comment says firmware doesn't echo it back).
  - `pps_enabled = 0` always (1420 has no 1PPS).
  - `antenna_current_ma = 0` — never read, byte 23 not examined.
  - `frequency2 = 0`, `out2_power_low = 0` — single output.

### What we do NOT know

1. **Are the opcodes correct?** 0x03/0x04/0x07 are code-inferred; never
   captured from the vendor Windows tool.
2. **Does the status byte really lack OUT1_EN/OUT2_EN/PPS_EN bits?** The code
   only reads PLL_LOCK and ANT_OK. The shared `lbe_common.h` defines all 8
   bits, but the 1420 ignores bits 5–7. This could be correct (older firmware
   doesn't set them) or an untested assumption.
3. **What's in the rest of the 60-byte status report?** Bytes 11–17 and 19–59
   are never read. On the 1425, bytes 21–24 turned out to carry GNSS/dynmodel/
   antenna-current/NMEA. Could the 1420 have anything there?
4. **Does the 1420 have any interrupt-IN endpoint?** The 1425/Mini have EP 0x83
   streaming UBX. Does the 1420 have a similar diagnostic channel, or is it
   HID-only?
5. **EN_OUT (0x01) arg values**: on the 1425, `0x03` = both outputs on, `0x00`
   = off. On the single-output 1420, the code sends `0x01` for on — is this
   correct, or should it be `0x01` (bit 0 = OUT1)?
6. **Does the firmware read back `outputs_enabled` in the status bits?** The
   hardcoded `= 1` may be covering up a working readback.
7. **1.6 GHz max frequency**: datasheet value. Is this enforced by firmware
   (silently capped / rejected), or only by the tool?
8. **`bcdDevice` version**: unknown. The 1425 is 1.10.

## Test rig

Same as the 1425 RE effort:

- **Linux host**: device plugged in directly. Runs this tool, `lsusb`, and
  `usbmon`-based capture.
- **GNOME Boxes Windows VM**: runs the working vendor Windows tool. The device
  is passed through to the guest (USB redirection). `usbmon` captures on the
  host while Windows drives.

Note: when the device is USB-redirected into the Boxes guest, the host kernel
unbinds the driver and the host-side `/dev/hidraw*` disappears. Un-share the
device before running the Linux tool again.

## Experiment ladder

Run top to bottom. Each rung's output is recorded under "Evidence" below.

### Rung 0 — Identify the device (Linux host, no capture needed)

Plug the 1420 into the Linux host directly (not the VM) and run:

```sh
lsusb -d 1dd2:               # confirm PID 0x2443
lsusb -v -d 1dd2:2443 2>/dev/null   # full descriptors
ls -l /dev/hidraw* /dev/ttyACM*      # which kernel nodes appeared
```

What we learn:
- **`bcdDevice`** → firmware revision.
- **Interface classes**: HID-only (single interface) or HID+CDC (composite, like
  1421/1425) or HID with interrupt-IN (like Mini)?
- **Endpoints**: just EP0 (control-only HID), or also interrupt-IN? If there's
  an interrupt-IN address, note it for Rung 1's `--rawdump`.
- **HID report descriptor**: report ID(s), report sizes. Confirm `0x4B` is
  actually defined, or if it's a different report ID.

### Rung 1 — Probe with this tool (Linux host)

The 1420 is already supported, so we can use it directly:

```sh
# Full status readback — does it decode sanely?
lbe-142x --status

# Dump the raw 60-byte status report for offline analysis
lbe-142x --status 2>&1 | tee /tmp/lbe1420-status.txt

# If Rung 0 reveals an interrupt-IN endpoint, try rawdump:
lbe-142x --rawdump 0x81 5000    # adjust EP address from Rung 0
```

Key checks:
- Does `--status` show a plausible frequency, PLL state, antenna state?
- What does `raw_status` actually read as? (It's printed as hex — check whether
  bits 5–7 are set or always zero.)
- If there's an interrupt-IN EP: does `--rawdump` show UBX (`B5 62`), NMEA
  (`$G`), or nothing?

Also try a non-destructive probe of the status byte's upper bits:

```sh
# Enable outputs, read status, check if bit 5 is set
lbe-142x --enable
lbe-142x --status     # note raw_status hex

# Disable outputs, read status, check if bit 5 clears
lbe-142x --disable
lbe-142x --status     # note raw_status hex
```

This directly tests whether the `outputs_enabled = 1` hardcode is hiding a
working readback.

### Rung 2 — Capture the vendor tool (host usbmon while VM drives)

On the Linux host:

```sh
sudo modprobe usbmon
lsusb -d 1dd2:2443          # note "Bus 0NN Device 0MM"
sudo tshark -i usbmon<NN> -w /tmp/lbe1420-<operation>.pcapng
```

Then pass the device to the Windows VM and perform **one operation at a time**
in the vendor tool, stopping the capture after each. One pcap per operation:

1. **connect / open** — baseline status poll
2. **set OUT1 = 10 MHz** (save)
3. **set OUT1 = 25 MHz** (different value, confirm byte layout)
4. **outputs off, then on** — capture EN_OUT opcode + arg
5. **power low, then normal**
6. **PLL ↔ FLL toggle**
7. **blink / identify**
8. **GPS view running** (if the vendor UI has one — capture the stream)

Extract the control transfers:

```sh
# SET_REPORT payloads (host→device):
tshark -r /tmp/lbe1420-<op>.pcapng \
  -Y 'usb.bmRequestType == 0x21 && usb.setup.bRequest == 0x09' \
  -T fields -e usb.setup.wValue -e usb.capdata

# GET_REPORT responses (device→host):
tshark -r /tmp/lbe1420-<op>.pcapng \
  -Y 'usb.bmRequestType == 0xa1 && usb.setup.bRequest == 0x01' \
  -T fields -e usb.setup.wValue -e usb.capdata
```

Or use the capture parser:

```sh
python3 python/parse_usb_capture.py /tmp/lbe1420-<op>.pcapng
```

Compare against the assumed opcode map:
- `0x03` should carry freq LE32 at payload byte 1 (not byte 5).
- `0x04` likewise.
- `0x07` should carry power arg at byte 1.
- `0x01` should carry enable arg — but is it `0x01` (single output) or `0x03`
  (both outputs, like 1421)?
- `0x0B` PLL/FLL arg.

### Rung 3 — Full status report analysis

After Rung 2, we have ground-truth pcaps. Examine the full 60-byte
GET_REPORT(0x4B) responses:

```sh
# Extract all status reads from the connect capture:
tshark -r /tmp/lbe1420-connect.pcapng \
  -Y 'usb.bmRequestType == 0xa1 && usb.setup.bRequest == 0x01' \
  -T fields -e usb.capdata | head -5
```

Annotate every byte:
- Bytes 0–10: already decoded (report-id, status, freq1, power).
- Bytes 11–17: unknown. On the 1421, bytes 14–17 are freq2 — does the 1420
  have anything here, or is it padding?
- Byte 18: FLL mode (already decoded). Confirm.
- Bytes 19–59: on the 1425, bytes 19–24 carry power2/GNSS/dynmodel/antenna-mA/
  NMEA. Does the 1420 report anything? If it's all `0x00` or `0xFF`, it's padding.

Also diff status before/after each operation from Rung 2 to see which bytes
change. This is how bytes 21–24 were identified on the 1425.

### Rung 4 — Document and correct

Based on Rungs 0–3:

1. Write `docs/reverse/LBE-1420-config-vX.Y.md` — same format as the 1425
   evidence doc: transport, opcode table, status layout, any diagnostic channel.
2. Fix any incorrect assumptions in `src/model_1420.c`:
   - If `outputs_enabled` is readable from status bits, read it.
   - If byte 23 has antenna current, decode it.
   - If there's an interrupt-IN diagnostic stream, implement it.
3. Add test fixtures from captured data to `tests/fixtures/`.
4. Add replay tests to `tests/test_replay.c`.
5. Extend `python/test_lbe1425.py` (or create `python/test_lbe1420.py`) with
   1420 scenarios.

## Specific hypotheses to test

Each is stated as falsifiable, per the project policy:

### H1: "The 1420 firmware does not echo `outputs_enabled` in the status byte"
- **Falsifier**: toggle outputs with `--enable`/`--disable`, read status, check
  bit 5 of `raw_status`. If it changes, the hardcode is wrong.
- **Test**: Rung 1 (no capture needed).

### H2: "Opcodes 0x03/0x04 carry freq at payload byte 1, not byte 5"
- **Falsifier**: vendor tool capture (Rung 2). If the freq LE32 appears at byte
  5 instead, the 1420 shares the 1421 payload layout.
- **Test**: Rung 2.

### H3: "The 1420 is HID-only with no interrupt-IN endpoint"
- **Falsifier**: `lsusb -v` shows an interrupt-IN endpoint, or `--rawdump`
  returns data.
- **Test**: Rung 0.

### H4: "Status byte bits 5–7 (OUT1_EN, OUT2_EN, PPS_EN) are unused on the 1420"
- **Falsifier**: Rung 1's enable/disable test shows bit 5 toggling. Or Rung 2's
  vendor tool capture shows the vendor reading/acting on these bits.
- **Test**: Rung 1 + Rung 2.

### H5: "EN_OUT arg is 0x01 (on) / 0x00 (off) on the 1420"
- **Falsifier**: vendor tool capture shows a different arg value (e.g. `0x03`
  like the 1421/1425).
- **Test**: Rung 2.

### H6: "The 1420 has no GNSS-related firmware features"
- **Falsifier**: status report bytes 21–24 contain non-padding data, or the
  vendor UI exposes constellation/dynmodel controls.
- **Test**: Rung 2 + Rung 3.

### H7: "1.6 GHz max is a hard firmware limit"
- **Falsifier**: setting a frequency above 1.6 GHz via `--f1t` succeeds and
  reads back. (Caution: only test with `--f1t` temporary, not `--f1` saved.)
- **Test**: Rung 1 (with care).

## Evidence (fill in as captures arrive)

### Rung 0 — descriptors
> _PID:_ (confirm `0x2443`)
> _bcdDevice:_
> _Interfaces / classes:_
> _Endpoints:_
> _Kernel nodes:_

### Rung 1 — `--status` + enable/disable probe
> _`--status` output:_
> _`raw_status` hex with outputs enabled:_
> _`raw_status` hex with outputs disabled:_
> _Interrupt-IN rawdump (if applicable):_

### Rung 2 — vendor-tool opcode map
| Operation      | wValue (report id) | payload bytes (hex)         | matches assumed? |
|----------------|--------------------|-----------------------------|------------------|
| connect/status |                    |                             |                  |
| set freq 10MHz |                    |                             |                  |
| set freq 25MHz |                    |                             |                  |
| outputs off    |                    |                             |                  |
| outputs on     |                    |                             |                  |
| power low      |                    |                             |                  |
| power normal   |                    |                             |                  |
| PLL→FLL        |                    |                             |                  |
| FLL→PLL        |                    |                             |                  |
| blink          |                    |                             |                  |

### Rung 3 — full status report layout
| offset | field (assumed)  | value seen | confirmed? |
|--------|------------------|------------|------------|
| 0      | report-id echo   |            |            |
| 1      | status bits      |            |            |
| 2–5    | (unknown)        |            |            |
| 6–9    | OUT1 freq LE32   |            |            |
| 10     | OUT1 power low   |            |            |
| 11–17  | (unknown)        |            |            |
| 18     | FLL mode         |            |            |
| 19–59  | (unknown)        |            |            |

### Rung 4 — code corrections
> _(to be filled after evidence review)_

## Open questions

- Does the vendor Windows tool for the 1420 expose any GPS/constellation UI?
  The 1420 is older — it may predate the GNSS control features.
- What GNSS module does the 1420 use? If it's an older u-blox (M6/M7 vs the
  1425's M8), the protocol capabilities differ.
- Is there a firmware update mechanism? (The 1425 is ROM-based, no updates.)
