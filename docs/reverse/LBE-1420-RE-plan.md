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

### Rung 3 — full status report layout (CONFIRMED 2026-07-05)

Raw dump obtained by adding a full hex print to `--probe-op`. Antenna
current confirmed by disconnect/reconnect test (byte 12: 0x04 → 0x00 → 0x04).

| offset | field            | value seen | confirmed? |
|--------|------------------|------------|------------|
| 0      | report-id echo   | `0x01`     | yes |
| 1      | status bits      | `0x1F`     | yes (see bit map below) |
| 2–5    | padding          | `0x00`     | — |
| 6–9    | OUT1 freq LE32   | 10 MHz     | yes |
| **10** | **GNSS mask**    | `0x47`     | **yes** (code assumes power — BUG) |
| **11** | **dynModel**     | `0x02`     | **yes** (Stationary) |
| **12** | **antenna mA**   | `0x04`     | **yes** (0 with no antenna, 4 with) |
| 13–17  | padding          | `0xFF`     | — |
| 18     | FLL mode         | `0x00`     | yes (PLL) |
| 19     | unknown          | `0x00`     | — |
| 20–59  | padding          | `0xFF`     | — |

**Power level is NOT echoed in the status report.** Toggling `0x0D` (SET_PWR1)
produced no status change in any byte. Power is write-only on the 1420.

**Status bit layout (byte 1)** — differs from the 1421/1425:

| bit | 1420 meaning  | 1421/1425 meaning |
|-----|---------------|-------------------|
| 0   | GPS_LOCK      | GPS_LOCK          |
| 1   | PLL_LOCK      | PLL_LOCK          |
| 2   | ANT_OK        | ANT_OK            |
| 3   | LED1          | LED1              |
| 4   | **OUT_EN**    | LED2              |
| 5   | unused (0)    | OUT1_EN           |
| 6   | unused (0)    | OUT2_EN           |
| 7   | unused (0)    | PPS_EN            |

Confirmed: `0x1F` (enabled) → `0x0F` (disabled via `--out 0`), bit 4 toggles.
Blink (`0x02`) toggles bit 3 (LED1): `0x1F` → `0x17`.

**Key difference from the 1425:** GNSS/dynModel/antenna-mA live at bytes
10–12 on the 1420 (vs 21–23 on the 1425). This places them right after the
frequency field, not in a "tail" region. The 1420 has no freq2, no OUT2 power,
no NMEA enable, and no PPS in the status report.

### Rung 4 — code corrections (CONFIRMED 2026-07-05)

All code changes implemented and validated (clean `-Werror` build, C test suite,
Python self-test). Evidence document: `LBE-1420-config-v1.08.md`.

**Bugs fixed:**
- **Critical:** `--pwr1` sent opcode `0x07` (= SET_GNSS), corrupting the
  constellation config. Now sends `0x0D` (SET_PWR1) — the correct opcode
  confirmed by vendor capture.
- **Status decode:** byte 10 was read as "OUT1 power" — it's actually the GNSS
  mask. Byte 11 (dynModel) and byte 12 (antenna current mA) were ignored.
- **Output-enable:** hardcoded `outputs_enabled = 1` masked the real state;
  now reads bit 4 of the status byte (confirmed via Rung 1).
- **Frequency opcodes:** used 0x03/0x04 with freq at byte 1; now uses 0x06/0x05
  with freq at byte 5 (1421 wire format, confirmed via Rung 2).
- **EN_OUT arg:** sent `0x01` for on; vendor sends `0xFF`. Changed for parity.

**New capabilities enabled:**
- `--gnss` (opcode 0x07): set GNSS constellation bitmask.
- `--dynmodel` (opcode 0x09): set u-blox dynamic model.
- `--diag` / `--clocklog` / `--gps-info`: UBX diagnostics via EP 0x83.
- `--monitor`: CDC NMEA live display (shared with 1421/1425).
- `--status` now shows GNSS mask, dynamic model, and antenna current (mA).
- Antenna current enables "Not connected (0 mA)" vs "OK (N mA)" display.

**Architecture:**
- Shared functions (`lbe_shared_monitor`, `lbe_shared_diag`,
  `lbe_shared_clocklog`, `lbe_shared_gps_info`) extracted from model_1421.c
  and declared in lbe_model.h. Titles use `LBE_MODEL_NAME` env var.
- New struct fields `gnss_mask` and `dynmodel` in `lbe_status` normalize
  model-specific byte offsets (1420: 10/11, 1425: 21/22).
- `status_byte_name()` in main.c is now model-aware for `--probe-op`.
- `cli_view.c` gates NMEA display on `ops->set_nmea` (NULL on 1420).
- Removed `m1420_set_1pps` stub — set to NULL (lbe_device.c handles it).

## Open questions

- ~~Does the vendor Windows tool for the 1420 expose any GPS/constellation UI?~~
  **Answered: yes.** The vendor tool sends `0x07` SET_GNSS and `0x09`
  SET_DYNMODEL. Full constellation sweep and dynmodel sweep captured.
- ~~What GNSS module does the 1420 use?~~ **Answered: u-blox M10** (ROM SPG
  5.10, PROTVER 34.10 — protocol 33+ = M10). HW field `000A0000`. Likely an
  M10M or similar small variant. The M10 lifts the M8's 3-concurrent-GNSS
  limit — BeiDou exclusion may not apply here (see H8).
- ~~Is there a firmware update mechanism?~~ **Yes.** All Leo Bodnar GPSDOs
  support firmware updates. (The "ROM" in UBX-MON-VER refers to the u-blox GPS
  module's mask-ROM firmware, not the LBE MCU.)
- ~~Is the `0x08` CFG-MSG wrap a one-shot poll or a persistent stream-enable on
  bcdDevice 1.08?~~ **Answered: one-shot.** The 15-second single-session test
  confirmed one burst of PVT+SAT+CLOCK, then idle. Continuous diagnostics
  require periodic re-polling.
- ~~Does the 1420 support the same `0x03`/`0x04` GNSS/dynmodel opcodes as the
  1425?~~ **Answered: no.** The 1420 uses different opcodes: `0x07` for GNSS
  (not `0x03`) and `0x09` for dynModel (not `0x04`). The `0x03`/`0x04` opcodes
  were never sent by the vendor tool.
- Does the 1420 firmware accept the "legacy" opcodes `0x03`/`0x04` for freq, or
  does it ONLY accept `0x06`? The current code uses `0x03`/`0x04` and freq
  changes have worked — but this may be because `0x03` is now SET_GNSS (and
  the freq bytes at offset 1–4 happen to be a valid mask), not because the
  firmware treats them as freq commands. **Do not probe `0x03`/`0x04` further
  without a hypothesis about what they might do.**

## Rung 5 — M10-specific capabilities (hypotheses)

The 1420 has a u-blox M10 (PROTVER 34.10), two generations newer than the
1425's M8 (PROTVER 18.00). The M10 lifts several M8 constraints, and the 0x08
UBX-wrap opcode potentially exposes features the vendor UI doesn't surface.

### H8: "The M10 lifts the BeiDou exclusion rule"

The M8 has a 3-concurrent-GNSS limit: GPS/SBAS/Galileo are mutually exclusive
with BeiDou. The M10 removes this limit — all constellations can track
simultaneously.

- **Falsifier**: Set `--gnss 0x4F` (GPS+SBAS+Galileo+BeiDou+GLONASS). If
  `--status` reads back `0x4F` AND `--gps-info` shows all five enabled in
  CFG-GNSS, the exclusion rule doesn't apply on the 1420.
- **Test**: Set and read back. Non-destructive — worst case, the firmware
  silently drops BeiDou and the readback shows `0x47`.
- **Note**: The Rung 2 vendor sweep captured `0x0F` (GPS+SBAS+Galileo+BeiDou)
  being sent — which would violate the M8 rule but succeed on M10.
- **Code impact**: If confirmed, the BeiDou-exclusion reject in
  `lbe_device.c:lbe_set_gnss()` needs a model gate (only enforce on 1425/M8).
- **Result**: **CONFIRMED.** `--probe-op 0x07 0x4F` → byte 10 reads back
  `0x4F`. `--status` shows `GNSS: 0x4F (GPS SBAS Galileo BeiDou GLONASS)`.
  GPS lock dropped momentarily during reconfiguration (0x1F→0x1E) then
  recovered. The M10 runs all five constellations concurrently.
  BeiDou exclusion gated to 1425-only in `lbe_device.c`.

### H9: "NavIC/IRNSS is accessible via the GNSS mask"

The M10 supports NavIC (u-blox gnssId 7). Following the `bit = 1 << gnssId`
pattern, NavIC would be bit 7 = `0x80`.

- **Falsifier**: Set `--gnss 0xC7` (GPS+SBAS+Galileo+GLONASS+NavIC). If
  `--status` reads back `0xC7` AND `--gps-info` CFG-GNSS shows NavIC enabled,
  the firmware passes bit 7 through.
- **Test**: Set and read back. Safe — if bit 7 is ignored, readback is `0x47`.
- **Note**: NavIC coverage is regional (India + surrounding). Might not acquire
  any SVs from the test location, but CFG-GNSS readback confirms the config.
- **Result**: **CONFIRMED.** `--probe-op 0x07 0xCF` → byte 10 reads back
  `0xCF`. GPS lock recovered. The firmware passes bit 7 through to the M10.
  NavIC display added to `--status` GNSS table.

### H10: "IMES (bit 4, 0x10) is passable"

IMES (Indoor Messaging System, gnssId 4) is a Japanese indoor positioning
system. Confirmed working on the 1425 by inference from the `1 << gnssId`
pattern, but never explicitly tested on either model.

- **Falsifier**: Set `--gnss 0x57` (GPS+SBAS+Galileo+IMES+GLONASS). Readback
  shows `0x57` and CFG-GNSS lists IMES.
- **Test**: Safe — if unsupported, bit is dropped.
- **Result**: **CONFIRMED.** `--probe-op 0x07 0x5F` → byte 10 reads back
  `0x5F`. Also tested `0xFF` (all 8 bits set) → reads back `0xFF`. The
  firmware passes the entire byte through verbatim; the M10 accepts all
  constellation bits.

### H11: "The 0x08 UBX wrap accepts arbitrary UBX commands"

The vendor tool only sends CFG-MSG via 0x08 (to enable NAV-PVT/SAT/CLOCK). But
the wrap format `{0x08, class, id, len_lo, len_hi, payload...}` suggests it
forwards any UBX command to the GPS module. If so, the following become
accessible without firmware changes:

- **CFG-TP5** (timing pulse): configure the 1PPS output parameters (pulse
  width, polarity, lock/unlock frequencies). The 1420 has no SET_PPS opcode,
  but if CFG-TP5 reaches the M10, it might be configurable via UBX directly.
- **CFG-VALSET** (M10 key-value config): the M10's native configuration
  interface. Could set anything the module supports.
- **MON-HW3** / **MON-RF** (M10 variants): might give better antenna status
  than the M8-era MON-HW we currently poll.

- **Falsifier (safe)**: Send a **poll** (zero-length payload) of a known M10
  message via `--probe-op 0x08 <class> <id> 0x00 0x00` and check EP 0x83 for
  the response. A safe first test: poll CFG-TP5 (`0x06 0x31 0x00 0x00`) — if
  the M10 responds, we can read the current timing pulse config without
  changing it.
- **Falsifier (observable)**: Send CFG-TP5 to set a known pulse width, then
  measure the 1PPS output with an oscilloscope.
- **Note**: The wrap format we use is
  `{0x08, class, id, len_lo, len_hi, payload...}` at args_offset 1. The
  firmware presumably adds the B5 62 sync header and UBX checksum before
  forwarding to the M10.
- **Result**: **CONFIRMED.** CFG-TP5 poll (`0x06 0x31 0x00 0x00`) via
  `--gps-info` returned a full 32-byte CFG-TP5 response from the M10.
  The 0x08 wrap forwards arbitrary UBX commands, not just CFG-MSG.

  TIMEPULSE0 readback:
  - Unlocked: 0 Hz, 0% duty (no output)
  - Locked: **141,697 Hz**, 50% duty square wave
  - Flags: active, lockGnssFreq, alignToTow, pol=falling, grid=GPS
  - Cable delay: 0 ns

  141,697 Hz is the GPS-disciplined reference feeding the LBE's PLL.
  Not a standard frequency — likely chosen by the LBE firmware to minimize
  fractional-N jitter at common output frequencies.

  CFG-TP5 is not in the M10 interface description (u-blox M10 SPG 5.00,
  UBX-20053845) — it's a legacy M8-era message. The M10 supports it via
  backwards compatibility. The documented M10 equivalent is CFG-VALGET/
  VALSET with CFG-TP-* keys (section 4.9.23 of the interface description).

### H12: "M10 protocol 34 NAV messages are available"

The M10 supports additional NAV messages not present on M8:
- NAV-SIG (signal-level per-signal, not just per-SV)
- NAV-TIMELS (leap second info)

These could be enabled via CFG-MSG wrap (same mechanism as PVT/SAT/CLOCK).

- **Falsifier**: Send CFG-MSG enable for NAV-SIG (`0x08 0x06 0x01 0x08 0x00
  0x01 0x43 0x0A`) and check EP 0x83 for class 0x01 id 0x43 responses.
- **Test**: Safe — worst case, the M10 NAK's the unknown message.
- **Value**: NAV-SIG gives per-signal (L1C/A, L1C, etc.) CNR rather than the
  per-SV aggregate NAV-SAT provides. Better visibility into multipath/jamming.
- **Result**: **CONFIRMED.** CFG-MSG enable for NAV-SIG via `--probe-op`,
  followed by `--rawdump 0x83 3000`, produced `B5 62 01 43 48 02` — a 584-byte
  NAV-SIG response with `numSigs = 36` individual signal records. The M10
  forwards protocol-34 NAV messages through the 0x08 wrap.
