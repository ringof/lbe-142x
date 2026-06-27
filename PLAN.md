# PLAN — Issue #17 `--clocklog`, stage (a)

Branch: `claude/17-clocklog`. This plan covers **stage (a)** only:
`iTOW` + CSV `--clocklog` + replay test. Stage (b) (gnuplot scripts + docs)
is a separate follow-up commit on the same branch.

## Key finding that changes the design (honesty-relevant)

The issue's spec lists, as a gap trigger, *"an EP 0x83 `[seq]` byte
discontinuity → dropped frames → gap."* The captured fixtures contradict the
premise that `r[0]` is a per-frame sequence counter:

- Across `ubx_1425_noant.bin` (397 frame transitions) `r[0]` takes **only two
  values, 0x6E and 0x76**, and stays constant across whole multi-frame UBX
  message groups (runs of 25, 12, 5, 11, … frames).
- By the "+1 mod 256" rule, **397 of 397** transitions look like "jumps" —
  i.e. treating non-consecutive `r[0]` as a dropped-frame gap would flag
  essentially *all* healthy data as gapped. That is the exact failure the
  feature exists to prevent.

Per CLAUDE.md hypothesis-validation ("when an existing empirical observation
contradicts a new theory, the existing observation wins"), I will **not** use
`r[0]` for continuity. The authoritative continuity signal is **`iTOW`** (from
the UBX NAV-CLOCK payload, well-understood, immune to USB/scheduler jitter):

- `iTOW` not advancing → stale/duplicate NAV-CLOCK → **drop** (no row).
- `iTOW` advances by ≠ 1000 ms → missed second(s) → emit the row with
  **`gap=1`** (never interpolate).

`docs/reverse/LBE-1425-config-v1.10.md` currently labels `r[0]` a "seq byte";
I will update it to record the observed two-value behavior and that it is not a
per-frame counter (CLAUDE.md: update the doc in the same change).

## Changes

1. **`include/ubx.h` / `src/ubx.c`** — add `uint32_t itow_ms;` to
   `struct ubx_clock`; set it from NAV-CLOCK payload offset 0 in
   `ubx_parse_clock`.

2. **`src/ubx.c` / `include/ubx.h`** — new hardware-free, testable units:
   - `int clocklog_row(char *out, size_t cap, const struct ubx_clock *clk,
     int fix_type, int num_sv, int gap)` — formats one CSV row
     `iTOW_s,clkB_ns,clkD_nsps,tAcc_ns,fAcc_pss,fixType,numSV,valid,gap`.
     Sentinel `tAcc`/`fAcc == 0xFFFFFFFF` → emitted as `-1` (never
     4294967295). `valid` = `clk.valid && fix_type>=2 && both accuracies not
     sentinel`.
   - `struct clocklog_state` + `clocklog_init()` + `clocklog_feed()` — wraps
     `ubx_consume` and the iTOW continuity/dup/gap logic so the whole honesty
     pipeline is replayable in tests, decoupled from the live HID loop.
     `clocklog_feed()` returns 1 and fills a row when a *fresh* NAV-CLOCK
     completes, else 0.

3. **`src/model_1421.c`** — `m1425_clocklog(struct lbe_transport *t, int
   seconds)`: mirrors `m1425_diag`'s claim/poll/read loop, prints a `#` header
   (column names + the scope caveat: NAV-CLOCK is the receiver's self-report,
   not an independent OUT1/OUT2 measurement), and for each frame calls
   `clocklog_feed()`, printing returned rows line-buffered (`fflush`).
   `seconds>0` bounds the run via the monotonic `lbe_millis()`; else runs until
   `read_input` errors / Ctrl-C.

4. **Wiring** — `clocklog` fn-ptr in `struct lbe_model_ops`; `.clocklog =
   m1425_clocklog` in `lbe_ops_1425`; `lbe_clocklog(dev, seconds)` wrapper in
   `src/lbe_device.c` (NULL-guarded like `lbe_diag`); decl in
   `include/lbe_device.h`; `--clocklog [seconds]` handler in `src/main.c` +
   `--help` line.

5. **Tests**
   - `tests/test_ubx.c`: `clocklog_row` unit cases — normal row, sentinel
     `tAcc`/`fAcc` → `-1`, untrusted (`fix_type<2` → `valid=0`), `gap=1`
     passthrough; plus `ubx_parse_clock` now sets `itow_ms`.
   - `tests/test_replay.c`: feed both fixtures through `clocklog_feed`, collect
     rows; assert noant rows are all `valid=0` (no fix) and 3dfix yields
     `valid=1` rows with `fixType=3`, monotonic non-decreasing `iTOW`, and no
     spurious `gap=1` in a clean capture.

## Out of scope for stage (a)
gnuplot scripts, README, `feedgnuplot` one-liner (stage (b)).

## Validation / regression (full change-doc given at commit time)
- Build: `cmake -B build && cmake --build build`.
- `clang -fsyntax-only -Wshadow -Wunreachable-code-aggressive -Iinclude
  src/*.c` (skip `*_windows.c`) before push; Windows CI (`windows-2022`) is the
  gate.
- `python3 python/test_lbe1425.py --self-test` + `ctest` (incl. ASan/UBSan,
  Valgrind jobs).
- Hardware (maintainer): `--clocklog 30` on a 1425 emits a CSV; an induced
  antenna disconnect shows a flagged `gap`/untrusted region, not a smooth lie.
