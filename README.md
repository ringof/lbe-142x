# LBE-142x GPS Locked Clock Source Configuration Tool

Cross-platform configuration tool for Leo Bodnar GPS-disciplined clock source devices:

| Model      | PID      | Outputs | Notes                                  |
|------------|----------|---------|----------------------------------------|
| LBE-1420   | 0x2443   | 1       | up to 1600 MHz                         |
| LBE-1421   | 0x2444   | 2       | up to 1400 MHz, dual-output, NMEA over CDC |
| LBE-1423   | 0x226f   | 2       | same protocol as 1421                  |
| LBE-1425   | 0x2269   | 2       | increased stability; OUT1 <=800 MHz +1PPS, OUT2 <=1.4 GHz; 1421 protocol + GNSS/dyn-model/NMEA controls |
| LBE-Mini   | 0x2211   | 1       | up to 810 MHz, UBX stream over HID     |

Configures device settings, sets frequencies, and provides a live GPS monitor.
Runs on Windows (MSVC / MinGW64) and GNU/Linux (tested on Windows 11 x64 and Ubuntu 24.04 LTS x64).

## Features

### Common (all models)
- Cross-platform (Windows and GNU/Linux)
- Set output frequency with Hz precision (persisted to flash)
- Enable/disable outputs, low/normal power level
- Blink output LED for device identification
- `--status` dumps device state (USB serial, PLL lock, GPS lock, antenna, frequencies, ...)
- `--monitor` live GPS display (UTC, lat/lon, altitude, per-SV CNR bars)

### LBE-1421 / LBE-1423 specific
- Dual output with independent frequency / power / temporary-frequency control
- 1PPS on OUT1 enable/disable
- PLL / FLL mode toggle
- `--monitor` parses NMEA from the USB CDC port (`/dev/ttyACM*` or `COMxx`); auto-discovers the port
- Live 1PPS chronometer: sub-second UTC interpolated from DCD edges, rolling jitter stats

### LBE-1425 specific
- Everything in the LBE-1421/1423 set above (dual output, 1PPS, PLL/FLL, NMEA `--monitor`)
- Asymmetric per-output frequency limits: OUT1 ≤ 800 MHz (the 1PPS output), OUT2 ≤ 1.4 GHz
- `--gnss <0xNN>` — GNSS constellation enable bitmask (`bit = 1<<gnssId`: GPS=0x01, SBAS=0x02, Galileo=0x04, BeiDou=0x08, IMES=0x10, QZSS=0x20, GLONASS=0x40). BeiDou is mutually exclusive with GPS/SBAS/Galileo; GLONASS is unrestricted. QZSS works even though the vendor UI doesn't expose it.
- `--dynmodel <model>` — u-blox dynamic platform model (`portable|stationary|pedestrian|automotive|sea|airborne`, or a raw u-blox value)
- `--nmea <0|1>` — enable/disable the NMEA output stream
- `--diag` — live UBX diagnostics: per-SV CNR histogram (NAV-SAT) plus a clock-disciplining line (NAV-CLOCK: bias, drift, time/frequency accuracy), parsed from the EP 0x83 stream
- `--clocklog [seconds]` — CSV time series of the NAV-CLOCK timing telemetry (bias, drift, time/frequency accuracy) for plotting how the GPS timing solution behaves over time; honesty-gated (see [Timing time series](#timing-time-series-clocklog))
- `--gps-info` — u-blox module version (UBX-MON-VER), antenna status, and the constellations the receiver actually has enabled (UBX-CFG-GNSS)
- `--status` additionally reports the **antenna bias current** — so it distinguishes "no antenna" (0 mA) from "OK" from "short", which the single short-circuit bit can't — plus the live GNSS mask, dynamic model and NMEA-output state

### LBE-Mini specific
- `--drive <8|16|24|32>` — set OUT1 Si5351C drive strength in mA (four-level)
- `--monitor` parses UBX NAV-PVT / NAV-SAT / NAV-CLOCK from the HID interrupt-IN endpoint
- `--gps-info` — UBX-MON-VER (u-blox SW/HW version + protocol extensions)

## Prerequisites

### Windows
- Microsoft Visual Studio 2022 **or** MinGW64 (via MSYS2)
- CMake (>= 3.18)
- **libusb**: auto-downloaded at configure time on MSVC (from the official release .7z); picked up from the MinGW64 toolchain (`pacman -S mingw-w64-x86_64-libusb`) on MinGW.

### GNU/Linux
- GCC or Clang
- CMake (>= 3.15)
- libudev:
   ```
   sudo apt install libudev-dev
   ```

## Building the Project

```
git clone https://github.com/bvernoux/lbe-142x.git
cd lbe-142x
```

### Windows (Visual Studio 2022)
```
cmake -B build-msvc -G "Visual Studio 17 2022" -A x64
cmake --build build-msvc --config Release
```
The MSVC build links against the **static CRT** (`/MT`) by default, so the produced `.exe` has no `VCRUNTIME140.dll` / `ucrtbase.dll` dependency.
Pass `-DLBE_MSVC_STATIC_CRT=OFF` to link the dynamic CRT instead.

### Windows (MinGW64, MSYS2)
```
cmake -B build-mingw64 -G "Ninja"
cmake --build build-mingw64
```

### GNU/Linux
```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

All three toolchains treat warnings as errors (`/W4 /WX` on MSVC, `-Wall -Wextra -Werror` elsewhere).

## Usage

`lbe-142x --help` prints a help screen tailored to the connected device.
Run with no device attached (or with `--help --pid 0xDEAD`) to see the generic help covering every supported model:

```
lbe-142x v1.3 26 Jun 2026 Leo Bodnar LBE-142x / LBE-Mini GPS clock source config
Usage: lbe-142x [OPTIONS]
Options:
  --help                 Show this help
  --pid <0xNNNN>         Select a specific LBE device when more than one is attached
                         (0x2443=1420, 0x2444=1421, 0x226f=1423, 0x2269=1425, 0x2211=Mini)
  --f1 <Hz>              Set OUT1 frequency, save to flash (1420 <=1600000000, 1421/1423 <=1400000000, 1425 OUT1 <=800000000, Mini <=810000000)
  --f1t <Hz>             Set OUT1 temporary frequency (not supported on Mini)
  --f2 <Hz>              Set OUT2 frequency, save to flash (LBE-1421/1423/1425)
  --f2t <Hz>             Set OUT2 temporary frequency (LBE-1421/1423/1425)
  --out <0|1>            Enable or disable outputs
  --pll <0|1>            Set PLL(0) or FLL(1) mode (not supported on Mini)
  --pps <0|1>            Enable or disable 1PPS on OUT1 (LBE-1421/1423/1425)
  --pwr1 <0|1>           Set OUT1 power level: normal(0) or low(1)
  --pwr2 <0|1>           Set OUT2 power level: normal(0) or low(1) (LBE-1421/1423/1425)
  --drive <8|16|24|32>   Set OUT1 drive strength in mA (Mini only)
  --gnss <0xNN>          Set GNSS constellation bitmask (GPS=0x01 SBAS=0x02 Gal=0x04 BeiDou=0x08 QZSS=0x20 GLONASS=0x40) (LBE-1425 only)
  --dynmodel <model>     Set u-blox dynamic model (portable|stationary|pedestrian|automotive|sea|airborne) (LBE-1425 only)
  --nmea <0|1>           Enable or disable NMEA output (LBE-1425 only)
  --diag                 Live UBX diagnostics (CNR histogram + clock disciplining) (LBE-1425 only)
  --clocklog [seconds]   CSV NAV-CLOCK time series for plotting (Ctrl-C, or run N s) (LBE-1425 only)
  --blink                Blink output LED(s) for 3 seconds
  --status               Display current device status
  --statlog              Poll status ~1 Hz, log lock state + raw report tail (LBE-142x)
  --probe-op <0xNN> [b..] Send a raw opcode + bytes, show status changes (advanced) (LBE-142x)
  --monitor              Live GPS display (UTC, lat/lon, altitude, CNR bars) (Mini: UBX; 1421/1423/1425: NMEA via CDC)
  --port <name>          CDC port for --monitor (e.g. COM12 or /dev/ttyACM0) (LBE-1421/1423/1425)
  --gps-info             Print u-blox GPS module version + antenna status (Mini / LBE-1425)
```

### Examples

Set OUT1 frequency to 10 MHz and save to flash:
```
./lbe-142x --f1 10000000
```

Set a temporary (non-persisted) frequency on OUT2 (LBE-1421/1423/1425):
```
./lbe-142x --f2t 10500000
```

Switch to FLL mode and enable 1PPS on OUT1 (LBE-1421/1423/1425):
```
./lbe-142x --pll 1 --pps 1
```

Set LBE-Mini drive strength to 24 mA:
```
./lbe-142x --pid 0x2211 --drive 24
```

On the LBE-1425, select GPS+GLONASS, a stationary dynamic model, and enable NMEA:
```
./lbe-142x --pid 0x2269 --gnss 0x41 --dynmodel stationary --nmea 1
```

Live GPS chronometer on LBE-1421 (auto-discovers COM / `/dev/ttyACM*`; `--port` overrides):
```
./lbe-142x --pid 0x2444 --monitor
./lbe-142x --pid 0x2444 --port COM12 --monitor
```

Live UBX monitor on LBE-Mini:
```
./lbe-142x --pid 0x2211 --monitor
./lbe-142x --pid 0x2211 --gps-info
```

UBX diagnostics (CNR histogram + clock disciplining) and module info on LBE-1425:
```
./lbe-142x --pid 0x2269 --diag
./lbe-142x --pid 0x2269 --gps-info
```

Select a specific device when both a Mini and a 1421 are attached:
```
./lbe-142x --pid 0x2444 --status
./lbe-142x --pid 0x2211 --status
```

## Status Display

The `--status` command prints per-model fields.
LBE-1421 / 1423 example:
```
Device Status (0x7F):
  GPS Lock: Yes
  PLL Lock: Yes
  Antenna: OK
  Output(s) Enabled: Yes
  OUT1 Frequency: 10000000 Hz
  OUT1 Power Level: Normal
  OUT2 Frequency: 10000000 Hz
  OUT2 Power Level: Normal
  1PPS on OUT1: Enabled
  Mode: PLL
```
LBE-1425 example (adds the serial number, antenna bias-current readout, and the
GNSS / dynamic-model / NMEA config it echoes in the status report):
```
  Serial: 0C7BB80E70E5
Device Status (0x7F):
  GPS Lock: Yes
  PLL Lock: Yes
  Antenna: OK (5 mA)
  Output(s) Enabled: Yes
  OUT1 Frequency: 10000000 Hz
  OUT1 Power Level: Normal
  OUT2 Frequency: 10000000 Hz
  OUT2 Power Level: Normal
  1PPS on OUT1: Enabled
  Mode: PLL
  GNSS: 0x47 (GPS SBAS Galileo GLONASS)
  Dynamic model: Stationary (2)
  NMEA output: Enabled
```
On the 1425 the antenna line reads `OK (N mA)` / `Not connected (0 mA)` /
`Short Circuit`, using the board's antenna bias-current measurement.

LBE-Mini example (no antenna / OUT2 / PLL-mode lines; `--drive` and signal-loss shown instead):
```
Device Status (0x23):
  GPS Lock: Yes
  PLL Lock: Yes
  Output(s) Enabled: Yes
  OUT1 Frequency: 10000000 Hz
  OUT1 Drive Strength: 24mA
  Signal loss count: 0
```

## Live GPS Monitor

`--monitor` renders a TUI with real-time UTC, position, altitude, fix quality, and a CNR bar chart per satellite.
The display is redrawn continuously; press Ctrl-C to exit.

**LBE-1421 / 1423 / 1425 chronometer mode:** sub-second UTC is interpolated from the 1PPS signal (carried on the CDC DCD line).
The trailing status line reports the rolling PPS jitter over the last 30 edges:
```
UTC:  2026-04-20 13:55:08.094
Lat:      +xx.xxxxxxx deg
Lon:      +xx.xxxxxxx deg
Alt:    +xxx.xxx m (MSL)
Fix:  3D        Sats used: 9
...
PPS:    edges=10  avg=998 ms  min=938  max=1062  (last 9)  NMEA->edge=0 ms
Stream: lines=120 bad_ck=1  port=COM12
```
Measurement precision is bounded by the OS millisecond clock resolution (`GetTickCount` ~15 ms on Windows, `CLOCK_MONOTONIC` ~1 ms on Linux) and the ~20 ms polling cadence.
For metrology-grade PPS timing, an external capture method is required.

## Timing time series (clocklog)

`--clocklog [seconds]` (LBE-1425) streams a CSV time series of the receiver's
UBX **NAV-CLOCK** timing telemetry — one row per solution (~1 Hz) — so you can
plot how the GPS timing solution behaves over time (e.g. `tAcc` settling as a
fix is acquired). It reads the same EP 0x83 diagnostics stream as `--diag` (no
`usbmon`/`sudo`). Output is line-buffered, so it pipes/redirects live:

```
./lbe-142x --pid 0x2269 --clocklog            # until Ctrl-C
./lbe-142x --pid 0x2269 --clocklog 300        # run for 300 s
./lbe-142x --pid 0x2269 --clocklog >> run.csv # log to a file (append)
```

> **Scope:** NAV-CLOCK is the u-blox receiver's *self-reported* clock solution,
> **not** an independent measurement of the disciplined OUT1/OUT2 output (that
> needs an external counter/reference). The tool reports the receiver's honest
> self-report with every detectable artifact flagged.

CSV columns (a leading `#` comment carries the header + the scope caveat):

```
iTOW_s,clkB_ns,clkD_nsps,tAcc_ns,fAcc_pss,fixType,numSV,valid,gap
```

| column | meaning |
|--------|---------|
| `iTOW_s`   | u-blox GPS time-of-week (s) — the authoritative time axis |
| `clkB_ns`  | receiver clock bias (ns) |
| `clkD_nsps`| receiver clock drift (ns/s) |
| `tAcc_ns`  | time accuracy estimate (ns); `-1` = unknown |
| `fAcc_pss` | frequency accuracy estimate (ps/s); `-1` = unknown |
| `fixType`  | 0 none, 2 2D, 3 3D |
| `numSV`    | satellites used |
| `valid`    | 1 only if trustworthy: a ≥2D fix **and** non-sentinel accuracies |
| `gap`      | 1 if a NAV-CLOCK second was missed just before this row |

**Honesty guarantees** (so host/OS/USB/firmware/receiver artifacts can't
masquerade as good data):

- The time axis is the u-blox `iTOW`, never host arrival time (immune to USB /
  scheduler jitter).
- A non-advancing `iTOW` (stale/duplicate frame) is dropped; an `iTOW` step ≠ 1 s
  is recorded as an explicit `gap` and **never** interpolated across.
- Corrupt frames are dropped by the UBX checksum, so they can't fake a reading.
- Unknown accuracies (`0xFFFFFFFF`) are emitted as `-1`, never as `4294967295`.
- Untrusted samples (no fix / unknown accuracy) carry `valid=0` so they render
  distinctly rather than being silently treated as good.

### Plotting with gnuplot

The repo ships two gnuplot scripts (gnuplot only — no matplotlib). They draw
trusted `tAcc` as a connected line, untrusted samples in grey, gaps as red
markers with the line **broken** across them, and drop `-1` sentinels.

**Want the pretty (windowed) plots?** You need a gnuplot built with an
interactive terminal (`qt`, `wxt`, or `x11`). The scripts auto-pick whichever
is present and fall back to in-terminal ASCII otherwise — so a minimal build
(e.g. `gnuplot-nox`, or some conda `base` installs) still works, just in ASCII.
To check, run `gnuplot -e "print GPVAL_TERMINALS"` and look for `qt`/`wxt`/`x11`.
To get a GUI terminal:

- Debian/Ubuntu: `sudo apt install gnuplot-qt` (the `gnuplot` metapackage pulls
  this in; `gnuplot-nox` does not).
- Fedora: `sudo dnf install gnuplot` (includes the qt terminal).
- macOS (Homebrew): `brew install gnuplot --with-qt`, or `brew install gnuplot`
  (recent bottles include qt).
- conda: `conda install -c conda-forge gnuplot` ships qt; if your `base`
  gnuplot is ASCII-only, `conda deactivate` to fall back to the system one.

ASCII (`dumb=1`) needs nothing extra and is the right choice over SSH / headless.

Live strip chart (GUI window if available, or ASCII in-terminal for SSH/no-X):

```
./lbe-142x --pid 0x2269 --clocklog >> run.csv &
gnuplot -e "csv='run.csv'" scripts/clocklog_live.gp                 # GUI window
gnuplot -e "csv='run.csv'; dumb=1" scripts/clocklog_live.gp         # ASCII
gnuplot -e "csv='run.csv'; fromstart=1" scripts/clocklog_live.gp    # whole run, no sliding
gnuplot -e "csv='run.csv'; tight=1" scripts/clocklog_live.gp        # zoom y to the data band
# options: window=<seconds> (default 120), fromstart (show all from first sample),
#          tight (zoom y to the data instead of 0-based), refresh=<seconds> (default 1)
```

`tight=1` is handy once locked: a disciplined 1425 pins `tAcc` near the receiver's
1 ns reporting floor, so the default 0-based axis shows just a flat low line —
`tight` zooms y to the data (with a 1 ns margin) to reveal e.g. `tAcc` dithering
between 3 and 4 ns. It works the same on the static script below.

Static plot of a finished log:

```
gnuplot -e "csv='run.csv'" scripts/clocklog_plot.gp                 # GUI window
gnuplot -e "csv='run.csv'; out='run.png'" scripts/clocklog_plot.gp  # PNG
gnuplot -e "csv='run.csv'; dumb=1" scripts/clocklog_plot.gp         # ASCII
```

No device handy? `scripts/sample_clocklog.csv` is a real capture replay you can
plot immediately to see the format and the `tAcc` settling curve:

```
gnuplot -e "csv='scripts/sample_clocklog.csv'; dumb=1" scripts/clocklog_plot.gp
```

Optional one-liner with [`feedgnuplot`](https://github.com/dkogan/feedgnuplot)
(awk strips the `#` header and emits `iTOW tAcc`; `--domain` makes the first
column the x-axis):

```
./lbe-142x --pid 0x2269 --clocklog \
  | awk -F, '!/^#/{print $1, $4; fflush()}' \
  | feedgnuplot --domain --lines --stream --xlen 120 \
                --xlabel 'iTOW (s)' --ylabel 'tAcc (ns)'
```

## Troubleshooting

### GNU/Linux
If you encounter permission issues accessing the device, add a udev rule granting your user access to LBE hidraw nodes:

```
# /etc/udev/rules.d/99-lbe.rules
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="1dd2", MODE="0660", GROUP="plugdev"
```
Add your user to `plugdev` and reload:
```
sudo usermod -aG plugdev $(whoami)
sudo udevadm control --reload-rules && sudo udevadm trigger
```
(Log out and back in for group membership to take effect.)

**LBE-1421 / 1423 / 1425 `--monitor` also needs read access to the CDC port** (`/dev/ttyACM*`).
Add yourself to `dialout`:
```
sudo usermod -aG dialout $(whoami)
```

## Contributing

Contributions to this project are welcome.
Please fork the repository and submit a pull request with your changes.

Protocol reverse-engineering notes live in [`docs/reverse/`](docs/reverse/)
(per-model USB-capture evidence). Helper scripts in [`python/`](python/) assist
with captures — `parse_pcapng.py` / `parse_usb_capture.py` decode a usbmon /
USBPcap capture, `usbmon_live.py` prints decoded opcodes live, and
`test_lbe1425.py` is an on-device integration test (`--self-test` runs its
parsers with no hardware).

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- Leo Bodnar Electronics for the LBE-142x devices and documentation / protocol
- Simon Unsworth (https://github.com/simontheu/lbe-1420) for the initial implementation reference
