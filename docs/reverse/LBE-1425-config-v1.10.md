<!--
SPDX-License-Identifier: MIT
Copyright (c) 2024-2026 Benjamin Vernoux
-->
# LBE-1425 config protocol (bcdDevice 1.10) — USB capture evidence

Reverse-engineered from a `usbmon` capture of the vendor Windows tool driving
an LBE-1425 (PID `0x2269`, serial `0C7BB80E70E5`), 2026-06-26. Companion to
`LBE-1425-RE-plan.md`.

## Transport

Composite CDC+HID (IAD). All config goes over **HID Feature reports on
interface 2** (`wIndex == 0x0002`), 60-byte payload (`wLength == 60`,
== `LBE_REPORT_SIZE`):

- write: `SET_REPORT` — `bmRequestType 0x21`, `bRequest 0x09`, `wValue 0x0300`
  (Feature, **report-ID 0**)
- read:  `GET_REPORT` — `bmRequestType 0xA1`, `bRequest 0x01`, `wValue 0x0300`

The **command opcode is payload byte 0**; the firmware reads it from the data,
not from the HID report-ID. (This tool's 1421 path sends the opcode in *both*
the report-ID and byte 0, so it interoperates fine.)

GPS NMEA is on the CDC ACM data interface; the vendor sets it to 9600 8N1
(`SET_LINE_CODING 80 25 00 00 00 00 08`).

## Command opcodes (payload byte 0)

Confirmed identical to the LBE-1421 (`include/lbe_common.h`):

| opcode | command   | payload                              | observed            |
|--------|-----------|--------------------------------------|---------------------|
| `0x01` | EN_OUT    | `[1]` = 0x00 off / 0x03 both on      | off, on             |
| `0x06` | SET_F1    | u32 LE freq at **byte 5**            | 10 000 000 Hz       |
| `0x0A` | SET_F2    | u32 LE freq at byte 5                | 27 000 000 Hz       |
| `0x0B` | SET_PLL   | `[1]` = 0 PLL / 1 FLL                | both                |
| `0x0C` | SET_PPS   | `[1]` = 0 off / 1 on                 | both (see note)     |
| `0x0D` | SET_PWR1  | `[1]` = 0 normal / 1 low             | both                |
| `0x0E` | SET_PWR2  | `[1]` = 0 normal / 1 low             | both                |

Not present in the 1421 map — **LBE-1425-specific, function not yet identified**:

| opcode | payload                                   | args observed             | hypothesis (UNCONFIRMED) |
|--------|-------------------------------------------|---------------------------|--------------------------|
| `0x0F` | `[1]` = 0/1                               | toggled on then off       | a boolean feature toggle |
| `0x04` | `[1]` = level                            | `0x00`, `0x02`, `0x08`    | 3+ level selector        |
| `0x03` | `[1]` = value                            | `0x07,0x43,0x45,0x46,0x47`| slider/stepper           |
| `0x08` | `08 06 01 08 00 01 {07,22,35} 0A`        | polled ~continuously      | telemetry/register read  |

`0x08` is issued repeatedly by the GUI (3 distinct sub-selectors cycling) and is
almost certainly how the vendor app reads the extended "increased stability"
telemetry. The `[6]` byte (0x07/0x22/0x35) likely selects what to read.

Note (SET_PPS): enabling PPS in the vendor UI forces OUT1 into PPS-only mode and
shows a 10 MHz default, but the **only** USB command emitted is `0x0C 01` (and
`0x0C 00` to disable) -- the OUT1 reconfiguration is firmware-internal or
UI-side, not a separate command. This tool's `--pps` already sends exactly that,
so it fully replicates the vendor behaviour. (Confirmed live via usbmon_live.py.)

## Status read (GET_REPORT, report-ID 0, 60 bytes)

Same layout as the 1421 status report for the documented fields:

| offset | field           | value seen      |
|--------|-----------------|-----------------|
| 0      | report-id echo  | `0x01`          |
| 1      | status bits     | `0x7F`/`0xF7`   |
| 6      | OUT1 freq u32LE | 10 000 000      |
| 14     | OUT2 freq u32LE | 27 000 000      |
| 18     | FLL mode        | 0               |
| 19     | OUT1 power low  | 0               |
| 20     | OUT2 power low  | 0               |
| **21** | **extra**       | `67 02 05 00`   |
| 25..   | padding         | `FF…`           |

Status bits @1 match `lbe_common.h` (GPS/PLL/ANT/LED/OUT1/OUT2/PPS). Bytes
**21-24 (`67 02 05 00`)** are new vs the 1421 and unexplained — candidate
"increased stability" telemetry (fw version / holdover metric / temperature).
Constant in this capture; needs captures across state changes to decode.

## Status / TODO

- Core 1421 command + status protocol: **confirmed**, already works in this tool.
- `0x03` / `0x04` / `0x0F` writes and the `0x08` poll: opcodes known, **functions
  not yet mapped** — need the vendor-UI action that produced each (see timeline
  in the PR discussion) and/or targeted single-operation captures.
- Status tail bytes 21-24: decode once we can vary the underlying quantity.
