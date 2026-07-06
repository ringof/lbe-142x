<!--
SPDX-License-Identifier: MIT
-->
# LBE-1420 config protocol (bcdDevice 1.08) — USB capture evidence

Reverse-engineered from `usbmon` capture of the vendor Windows tool driving an
LBE-1420 (PID `0x2443`, serial `04565E12611B`, bcdDevice 1.08), 2026-07-05.
Companion to `LBE-1420-RE-plan.md`.

## Transport

Composite CDC+HID (IAD), same three-interface layout as the LBE-1425:

- IF0 + IF1 = CDC ACM (serial port, `/dev/ttyACM*`, streams NMEA)
- IF2 = HID with interrupt-IN endpoint **EP 0x83** (64-byte frames)

All config goes over **HID Feature reports on interface 2**, 60-byte payload
(`LBE_REPORT_SIZE`):

- write: `SET_REPORT` — `bmRequestType 0x21`, `bRequest 0x09`
- read:  `GET_REPORT` — `bmRequestType 0xA1`, `bRequest 0x01`, report-ID `0x4B`

The **command opcode is payload byte 0**, same as the 1421/1425.

## Command opcodes (payload byte 0)

The 1420 uses the **1421 wire format**, not the previously assumed "legacy"
format. Frequency LE32 at byte offset 5 (not 1). Confirmed via vendor-tool
capture.

| opcode | command       | payload                              | notes |
|--------|---------------|--------------------------------------|-------|
| `0x01` | EN_OUT        | `[1]` = 0x00 off / 0xFF on           | vendor sends 0xFF (any nonzero = on) |
| `0x02` | BLINK         | (no args)                            | |
| `0x05` | SET_F1_TEMP   | u32 LE freq at byte 5                | not seen in vendor captures |
| `0x06` | SET_F1        | u32 LE freq at byte 5                | confirmed: 10 MHz, 25 MHz |
| `0x07` | **SET_GNSS**  | `[1]` = constellation bitmask        | **not** SET_PWR1 (critical bug) |
| `0x09` | **SET_DYNMODEL** | `[1]` = u-blox CFG-NAV5 value     | new: Portable/Stationary/Airborne |
| `0x0B` | SET_PLL       | `[1]` = 0 PLL / 1 FLL               | confirmed |
| `0x0D` | SET_PWR1      | `[1]` = 0 normal / 1 low             | confirmed |

### Differences from the LBE-1425

| feature | 1420 (v1.08) | 1425 (v1.10) |
|---------|--------------|--------------|
| SET_GNSS | opcode `0x07` | opcode `0x03` |
| SET_DYNMODEL | opcode `0x09` | opcode `0x04` |
| SET_F2 / SET_F2_TEMP | N/A (single output) | `0x0A` / `0x09` |
| SET_PPS | N/A | `0x0C` |
| SET_PWR2 | N/A | `0x0E` |
| SET_NMEA | not available | `0x0F` |
| EN_OUT arg for "on" | `0xFF` | `0x03` (both outputs) |

### GNSS bitmask (opcode 0x07, arg byte 1)

Same bit encoding as the 1425 (`bit = 1 << u-blox gnssId`):

| bit | mask | constellation |
|-----|------|---------------|
| 0   | 0x01 | GPS           |
| 1   | 0x02 | SBAS          |
| 2   | 0x04 | Galileo       |
| 3   | 0x08 | BeiDou        |
| 4   | 0x10 | IMES          |
| 5   | 0x20 | QZSS          |
| 6   | 0x40 | GLONASS       |

Full constellation sweep captured: GPS-only (0x01), GPS+SBAS (0x03),
GPS+SBAS+Galileo (0x07), GPS+SBAS+Galileo+BeiDou (0x0F),
GPS+SBAS+Galileo+GLONASS (0x47).

**Critical bug found:** the old `LBE_1420_SET_PWR1 = 0x07` meant `--pwr1 1`
sent `0x07 0x01` = SET_GNSS GPS-only, corrupting the constellation config.

### Factory reset (vendor tool)

1. `0x06` SET_F1 = 10 MHz
2. `0x0B` SET_PLL = 0 (PLL mode)
3. `0x0D` SET_PWR1 = 0 (normal)
4. `0x07` SET_GNSS = 0x47 (GPS+SBAS+Galileo+GLONASS)
5. `0x09` SET_DYNMODEL = 0x02 (Stationary)

## Status read (GET_REPORT, report-ID 0x4B, 60 bytes)

| offset | field            | value seen | notes |
|--------|------------------|------------|-------|
| 0      | report-id echo   | `0x01`     | |
| 1      | status bits      | `0x1F`     | see bit layout below |
| 2–5    | padding          | `0x00`     | |
| 6–9    | OUT1 freq LE32   | 10 MHz     | |
| **10** | **GNSS mask**    | `0x47`     | |
| **11** | **dynModel**     | `0x02`     | Stationary |
| **12** | **antenna mA**   | `0x04`     | 0 with no antenna, 4 with |
| 13–17  | padding          | `0xFF`     | |
| 18     | FLL mode         | `0x00`     | PLL |
| 19     | unknown          | `0x00`     | |
| 20–59  | padding          | `0xFF`     | |

**Power level is NOT echoed** in the status report (write-only on the 1420).

### Status bit layout (byte 1) — differs from the 1421/1425

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

## Diagnostics channel: UBX over EP 0x83

Same 2-byte framing as the 1425: `[tag][len][payload]`, 64-byte transfers.
`len = 0` is a keepalive. The concatenated payload is a valid UBX stream.

**Key difference from the 1425:** on bcdDevice 1.08, the `0x08` CFG-MSG wrap
is **one-shot** — it produces one burst of NAV-PVT/SAT/CLOCK, then the stream
stops. The 1425 (v1.10) free-runs continuously. For `--diag`/`--clocklog`, the
implementation re-polls at ~1 Hz (the existing code already does this with the
`m1425_diag_poll()` keepalive).

Confirmed messages: NAV-PVT (`01 07`), NAV-SAT (`01 35`), NAV-CLOCK (`01 22`),
ACK-ACK (`05 01`). Timestamp validated: 2026-07-06 04:26:22 UTC.

## GNSS module (UBX-MON-VER, via --gps-info)

```
SW version : ROM SPG 5.10 (7b202e)
HW version : 000A0000
Extension 1: FWVER=SPG 5.10
Extension 2: PROTVER=34.10
Extension 3: GPS;GLO;GAL;BDS
Extension 4: SBAS;QZSS
GNSS enabled: GPS SBAS Galileo GLONASS
Antenna    : ? (power ?)  [MON-HW]
```

��� a **u-blox M10** — identified by protocol version 34.10 (M10 uses protocol
33+; M9 is 27–32; M8 is 15–23) and firmware generation SPG 5.x (M8 = 3.x).
ROM-based, Standard Precision GNSS. HW field `000A0000` (the 1425 M8 has
`00080000`; the encoding of this field is not documented — the generation
identification rests on PROTVER, not the HW byte). Likely an M10M or similar
small variant (speculative — not confirmed by teardown).

| | LBE-1420 | LBE-1425 |
|--|----------|----------|
| u-blox gen | M10 | M8 |
| HW version | `000A0000` | `00080000` |
| FW | SPG 5.10 | SPG 3.01 |
| Protocol | 34.10 | 18.00 |

The M10 supports all the same constellations (GPS/GLONASS/Galileo/BeiDou +
SBAS/QZSS). The M8's BeiDou-vs-GPS/Galileo exclusion (3-concurrent-GNSS limit)
likely does NOT apply on the M10 — untested (see H8 in LBE-1420-RE-plan.md).

**Antenna status:** MON-HW reports `antStatus = ?`, `antPower = ?` — the
antenna supervisor is not wired to the u-blox (same situation as the 1425).
The LBE MCU's own bias current measurement (status byte 12, mA) provides real
antenna detection instead.

## CDC serial (NMEA)

NMEA confirmed streaming on `/dev/ttyACM*`. The 1420 supports `--monitor`
using the same CDC NMEA + DCD/1PPS infrastructure as the 1421/1425.
