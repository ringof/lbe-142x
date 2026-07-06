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
# Disable outputs, read status, check if any bit clears
lbe-142x --out 0 && lbe-142x --status     # note raw_status hex

# Enable outputs, read status, check if it comes back
lbe-142x --out 1 && lbe-142x --status     # note raw_status hex
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
- **Falsifier**: toggle outputs with `--out 0`/`--out 1`, read status, check
  bit 5 of `raw_status`. If it changes, the hardcode is wrong.
- **Test**: Rung 1 (no capture needed).
- **Result**: REFUTED. Bit 5 (`OUT1_EN_BIT`) stays 0 in both states, but
  **bit 4 (`LED2_BIT`) toggles**: `0x1F` with outputs on → `0x0F` with outputs
  off. The 1420 firmware echoes output-enable state on bit 4, not bit 5. The
  hardcoded `outputs_enabled = 1` in `model_1420.c` is a **bug** — it reports
  "Output(s) Enabled: Yes" even after `--out 0`.

### H2: "Opcodes 0x03/0x04 carry freq at payload byte 1, not byte 5"
- **Falsifier**: vendor tool capture (Rung 2). If the freq LE32 appears at byte
  5 instead, the 1420 shares the 1421 payload layout.
- **Test**: Rung 2.
- **Result**: REFUTED. The vendor tool uses opcode `0x06` with freq at byte 5 —
  the 1421 wire format. Opcodes 0x03/0x04 were never sent. The "legacy" 1420
  opcodes in `lbe_common.h` may be from an older firmware or simply wrong.

### H3: "The 1420 is HID-only with no interrupt-IN endpoint"
- **Falsifier**: `lsusb -v` shows an interrupt-IN endpoint, or `--rawdump`
  returns data.
- **Test**: Rung 0.
- **Result**: REFUTED. `lsusb -v` shows the 1420 is a CDC+HID composite with
  the same three-interface layout as the 1425: CDC ACM on IF0+IF1 (serial port,
  likely NMEA), HID on IF2 with **EP 0x83 interrupt-IN (64B)** — the same
  endpoint the 1425 uses for its UBX diagnostic stream.

### H4: "Status byte bits 5–7 (OUT1_EN, OUT2_EN, PPS_EN) are unused on the 1420"
- **Falsifier**: Rung 1's enable/disable test shows bit 5 toggling. Or Rung 2's
  vendor tool capture shows the vendor reading/acting on these bits.
- **Test**: Rung 1 + Rung 2.
- **Result**: CONFIRMED for bits 5–7 — they stay 0 in both enabled and disabled
  states. But the 1420 firmware uses a **different bit layout**: bit 4
  (`LED2_BIT` in the 1421/1425 mapping) serves as the output-enable indicator.
  The 1420 status byte is `{GPS, PLL, ANT, LED1, OUT_EN, -, -, -}`, not the
  1421/1425 layout `{GPS, PLL, ANT, LED1, LED2, OUT1, OUT2, PPS}`.

### H5: "EN_OUT arg is 0x01 (on) / 0x00 (off) on the 1420"
- **Falsifier**: vendor tool capture shows a different arg value (e.g. `0x03`
  like the 1421/1425).
- **Test**: Rung 2.
- **Result**: PARTIALLY REFUTED. Off = `0x00` (correct), but on = `0xFF`
  (not `0x01`). The current code sends `0x01` and it works, so likely any
  nonzero value = on. Minor — but for vendor-parity, should send `0xFF`.

### H6: "The 1420 has no GNSS-related firmware features"
- **Falsifier**: status report bytes 21–24 contain non-padding data, or the
  vendor UI exposes constellation/dynmodel controls.
- **Test**: Rung 2 + Rung 3.
- **Result**: REFUTED. The vendor tool sends `0x07` SET_GNSS (constellation
  mask) and `0x09` SET_DYNMODEL (u-blox dynamic platform model). Full
  constellation sweep captured (GPS, SBAS, Galileo, BeiDou, GLONASS). Factory
  reset sets GNSS=0x47, dynModel=Stationary — same as the 1425.

### H7: "1.6 GHz max is a hard firmware limit"
- **Falsifier**: setting a frequency above 1.6 GHz via `--f1t` succeeds and
  reads back. (Caution: only test with `--f1t` temporary, not `--f1` saved.)
- **Test**: Rung 1 (with care).

## Evidence (fill in as captures arrive)

### Rung 0 — descriptors (CONFIRMED 2026-07-05)
> _PID:_ **`0x2443`** confirmed. `iProduct` = "LBE-1420 GPS Locked Clock Source".
> _bcdDevice:_ **1.08**
> _Serial:_ `04565E12611B`
> _Interfaces / classes:_ **Composite IAD** (`bDeviceClass 0xEF`), same shape
> as the 1425. Three interfaces:
> - IF0 + IF1 = **CDC ACM** (Communications + CDC Data). This means the 1420
>   has a serial port (`/dev/ttyACM*`) — likely streaming NMEA, just like the
>   1421/1425.
> - IF2 = **HID** with interrupt-IN.
>
> _Endpoints:_
> - CDC notify: EP `0x81` (interrupt, 10B)
> - CDC data: EP `0x02` OUT (bulk, 8B) / EP `0x82` IN (bulk, 16B)
> - HID interrupt-IN: **EP `0x83`** (64B) — same as the 1425's UBX diagnostic
>   channel. The 1420 code never reads this endpoint.
>
> _Kernel nodes:_ not checked yet (device was opened without root; HID report
> descriptor shows "UNAVAILABLE" — needs `sudo lsusb -v` or `/dev/ttyACM*` check).
>
> **Key finding:** the LBE-1420 is NOT HID-only. It has the same CDC+HID
> composite layout as the 1421/1425, with a CDC serial port and a 64-byte
> interrupt-IN endpoint on EP 0x83. Neither is used by the current code.
> This means the 1420 potentially supports NMEA monitoring (`--monitor`),
> UBX diagnostics (`--diag`/`--clocklog`/`--gps-info`), and 1PPS-on-DCD — all
> capabilities that were never investigated.

### Rung 1 — `--status` + enable/disable probe (CONFIRMED 2026-07-05)
> _Serial:_ `04565E12611B`
> _`--status` output (baseline):_ GPS Lock Yes, PLL Lock Yes, Antenna OK,
> OUT1 27 MHz, Power Normal, PLL mode.
> _`raw_status` hex with outputs enabled:_ **`0x1F`** = `0001 1111`
> (bits 0–4: GPS, PLL, ANT, LED1, **bit 4 set**)
> _`raw_status` hex with outputs disabled (`--out 0`):_ **`0x0F`** = `0000 1111`
> (bits 0–3: GPS, PLL, ANT, LED1; **bit 4 clear**)
>
> **Finding:** bit 4 tracks output enable state. The 1420 uses a different
> status-bit layout from the 1421/1425 — bit 4 is the output-enable flag, not
> `LED2_BIT`. Bits 5–7 are always 0. The hardcoded `outputs_enabled = 1` in
> `model_1420.c:48` is a bug: it masks the disabled state.
> Despite reporting "Output(s) Enabled: Yes" after `--out 0`, the output was
> actually disabled (confirmed by the frequency output stopping).
>
> _Interrupt-IN rawdump (EP 0x83):_ **UBX stream confirmed.**
>
> Idle state: `[1F][00][FF×62]` — tag `0x1F`, length 0, 62 bytes padding.
> Same 2-byte `[tag][len]` framing as the 1425.
>
> After sending `0x08` CFG-MSG wrap to enable NAV-PVT (same command the 1425
> vendor tool uses: `08 06 01 08 00 01 07 0A`), EP 0x83 produced a **valid
> NAV-PVT** (`B5 62 01 07 5C 00`, 92 bytes) spanning frames 3–5, timestamped
> **2026-07-06 04:26:22 UTC**. Framing: `[1F][3E][62 bytes payload]` — identical
> to the 1425.
>
> **Single-session test (15 s, all three NAV messages enabled):**
> All three `0x08` CFG-MSG wraps (PVT `0x07`, CLOCK `0x22`, SAT `0x35`) sent
> in one session, followed by `--rawdump 0x83 15000`. Result:
>
> - Frames 1–3: three `ACK-ACK` (`05 01`, payload `06 01` = CFG-MSG ack) —
>   firmware accepted all three commands.
> - Frames 4–8: **one burst** containing NAV-PVT (`01 07`, timestamp
>   2026-07-06 04:42:46 UTC), NAV-SAT (`01 35`, per-SV records), and
>   NAV-CLOCK (`01 22`, 20-byte payload with iTOW/bias/drift/accuracy).
> - Frames 9–42: all idle (`1F 00 FF...`), ~10 seconds of silence.
>
> **`0x08` is one-shot on bcdDevice 1.08.** One burst of all three NAV messages,
> then the stream stops. The 1425 (bcdDevice 1.10) free-runs continuously after
> activation. For `--diag`/`--clocklog` on the 1420, the implementation will
> need to re-poll `0x08` at ~1 Hz (the vendor GUI likely does exactly this).
>
> The diagnostic channel exists and works. The 1420 can support `--diag`,
> `--clocklog`, and `--gps-info` using the same UBX infrastructure as the
> 1425, with the addition of periodic re-polling.
>
> _CDC serial (NMEA):_ confirmed streaming on `/dev/ttyACM*`.

### Rung 2 — vendor-tool opcode map (CONFIRMED 2026-07-05, usbmon_live.py)

Captured via `usbmon_live.py --dev 13 --reads` on bus 3, device redirected
to GNOME Boxes VM running the vendor Windows tool.

**The LBE-1420 (bcdDevice 1.08) uses the 1421 wire format, NOT the assumed
"legacy" format.** The `LBE_1420_SET_F1_TEMP`/`SET_F1`/`SET_PWR1` opcodes
in `lbe_common.h` (0x03/0x04/0x07) are all wrong for this firmware.

| Operation | opcode | payload (hex) | code assumed | match? |
|-----------|--------|---------------|--------------|--------|
| set freq 10MHz | `0x06` | `06 00 00 00 00 80 96 98 00` (freq@byte5) | 0x04 freq@byte1 | **NO** |
| set freq 25MHz | `0x06` | `06 00 00 00 00 40 78 7D 01` (freq@byte5) | 0x04 freq@byte1 | **NO** |
| outputs off | `0x01` | `01 00` | 0x01 arg=0x00 | yes |
| outputs on | `0x01` | `01 FF` | 0x01 arg=0x01 | **minor** (0xFF vs 0x01; nonzero=on) |
| power low | `0x0D` | `0D 01` | 0x07 arg=0x01 | **NO** (0x07 is GNSS!) |
| power normal | `0x0D` | `0D 00` | 0x07 arg=0x00 | **NO** |
| PLL→FLL | `0x0B` | `0B 01` | 0x0B arg=0x01 | yes |
| FLL→PLL | `0x0B` | `0B 00` | 0x0B arg=0x00 | yes |
| blink | `0x02` | `02 00` | 0x02 | yes |
| GNSS mask (new!) | `0x07` | `07 <mask>` (see below) | not implemented | — |
| dynmodel (new!) | `0x09` | `09 <val>` (see below) | not implemented | — |
| UBX poll | `0x08` | `08 06 01 08 00 01 <sel> 01` | not implemented | — |

**Opcode 0x07 = SET_GNSS (constellation bitmask)**. Same bitmask encoding as
the 1425's `0x03` — `bit = 1 << u-blox gnssId`:

| capture | arg | constellations |
|---------|-----|----------------|
| sweep | `0x01` | GPS |
| sweep | `0x03` | GPS+SBAS |
| sweep | `0x07` | GPS+SBAS+Galileo |
| sweep | `0x0F` | GPS+SBAS+Galileo+BeiDou |
| sweep | `0x47` | GPS+SBAS+Galileo+GLONASS |

**Critical bug:** `model_1420.c` defines `LBE_1420_SET_PWR1 = 0x07`. Using
`--pwr1 1` on the 1420 sends `0x07 0x01`, which the firmware interprets as
SET_GNSS with mask=0x01 (GPS-only). `--pwr1 0` sends `0x07 0x00`, which
would **disable all GNSS constellations**.

**Opcode 0x09 = SET_DYNMODEL (u-blox CFG-NAV5)**. Same value encoding as the
1425's `0x04`:

| capture | arg | model |
|---------|-----|-------|
| sweep | `0x00` | Portable |
| sweep | `0x08` | Airborne<4g |
| sweep | `0x02` | Stationary |

**Factory reset sequence** (vendor "reset to factory" button):
1. `0x06` SET_F1 = 10 MHz
2. `0x0B` SET_PLL = 0 (PLL mode)
3. `0x0D` SET_PWR1 = 0 (normal)
4. `0x07` SET_GNSS = `0x47` (GPS+SBAS+Galileo+GLONASS)
5. `0x09` SET_DYNMODEL = `0x02` (Stationary)

Factory defaults match the 1425 exactly. No EN_OUT, SET_PPS, or SET_NMEA in
the reset macro — those settings are not touched by factory reset.

**Opcodes NOT seen:** `0x03`/`0x04` (the assumed "legacy" 1420 freq opcodes),
`0x05` (SET_F1_TEMP), `0x0C` (SET_PPS), `0x0E` (SET_PWR2), `0x0F` (SET_NMEA).
The vendor UI does not expose temporary-freq, 1PPS, or NMEA toggle on the 1420.

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
  1425's M8), the protocol capabilities differ. A `--gps-info` equivalent
  (UBX-MON-VER poll) would answer this directly once implemented.
- Is there a firmware update mechanism? (The 1425 is ROM-based, no updates.)
- ~~Is the `0x08` CFG-MSG wrap a one-shot poll or a persistent stream-enable on
  bcdDevice 1.08?~~ **Answered: one-shot.** The 15-second single-session test
  confirmed one burst of PVT+SAT+CLOCK, then idle. Continuous diagnostics
  require periodic re-polling.
- Does the 1420 support the same `0x03`/`0x04` GNSS/dynmodel opcodes as the
  1425, or are those opcodes exclusively freq commands on this model? The Rung 2
  vendor capture will answer this; DO NOT probe `0x03`/`0x04` blindly as they
  would be interpreted as SET_F1_TEMP/SET_F1 (frequency change).
