# CLAUDE.md — Working Agreement for Claude Code

This is the configuration tool for Leo Bodnar LBE-142x / LBE-Mini GPS-locked
clock sources. Large parts of it are reverse-engineered from hardware; these
rules keep that work honest and the tree releasable.

## Hypothesis Validation Policy

- Before proposing a patch or claiming a root cause, state the hypothesis as
  falsifiable: "X is the cause; if so, Y would change."
- If the falsifier (Y) can be tested without committing or building — a USB
  capture, a live `--status`/`--diag` readback, an opcode sweep — test it
  FIRST and report the result. Do not propose code changes whose justification
  rests on un-tested theory about what the firmware "should" do.
- When an existing empirical observation contradicts a new theory (e.g.,
  "the vendor Windows tool sets it, our tool doesn't" vs. "the opcode is X"),
  the existing observation wins until the new theory directly explains it.
- Do not generalize a negative across surfaces. "Opcodes 0x10–0x2F caused no
  status change" does NOT rule out a physical/GPS-side effect that the status
  report wouldn't echo — different mechanism, different evidence.
- Do not present an inferred unit or scale as fact. Status byte 23 reads as a
  bias current "in mA" by inference, not proof — keep the hedge.
- Do not call fixes "decisive" or "this'll do it" before validation.
  Predictions are noise; results are signal.
- See `docs/reverse/` for the captured opcode/status/UBX evidence this policy
  was forged from. Read the relevant file before proposing changes to the wire
  protocol, status decode, or diagnostics parsing — and if you contradict a
  recorded finding, update the doc in the same change.

## Bench / Hardware Debugging

- **The agent's environment has no device attached.** Hardware tests are run by
  the maintainer on a Linux host plus a Windows VM (GNOME Boxes), with `usbmon`.
- **Never fabricate device output.** If a claim needs the hardware, say so,
  hand the maintainer the exact command or capture steps, and wait for the
  result. Do not present invented `--status`/`--diag`/capture output as real.
- When the maintainer is driving the device, prefer plain persistent-shell
  commands (`./build/bin/lbe-142x --status`, `lsusb`, a single `usbmon`
  capture) over batches of one-off wrappers.
- Do not hand over one-off scripts for manual investigation unless asked.
- Capture gotchas to relay, not re-discover: passing the device through to the
  Boxes VM detaches it from the host (un-share it to capture host-side); keep
  `usbmon`/tshark captures under `/tmp` (AppArmor blocks `$HOME`); the tool
  needs no `sudo` (a hidraw udev rule grants access) — do not add a sudo step.

## Commit and Push Policy

- **Always ask before committing and pushing.** Never commit or push without
  explicit user approval. **Never push to `main` unless explicitly told to.**

## Git Branch Discipline

**Work is issue-driven: one issue, one branch.** The standing branch name is
`claude/<issue-number>-<short-description>` (e.g. `claude/14-rmc-empty-status`).
That naming pattern is the only pre-authorized branch creation — everything
else needs my explicit instruction naming the branch.

- Do not start work without an issue number. If a task has no issue yet, file
  the issue first (see Issue Filing Policy), then branch from its number.
- The fix for an issue lands through that issue's branch and nothing else.
- Conceptual "scope cleanliness" is not a reason for an extra branch. Within a
  branch, commits manage scope; branches do not.
- If you genuinely think a change should land somewhere other than the current
  branch, ASK. Do not preemptively create or switch branches and present the
  result as a fait accompli.
- This applies recursively: if you've already created an unauthorized branch,
  do not propose moving the work to *another* new branch. Move it to a branch
  that already exists, or ask.

## Planning Policy

- For tasks that generate multiple needs or planned changes, **write a plan
  first** and add it to a document (e.g., `PLAN.md` or a specifically named
  Markdown file) before beginning implementation.
- Get user approval on the plan before proceeding with changes.

## Change Documentation Requirements

Before any approved commit, provide the user — in the chat — with a
**copy-pastable block** containing all of the following:

1. **Detailed description of the change** — what was changed and why.
2. **Build/update instructions** — how to build after applying the change. If
   identical to the documented build (`cmake -B build && cmake --build build`),
   state that explicitly; if it differs, give the exact steps.
3. **Validation test** — a concrete procedure showing the change works. For
   parser/decode changes, cite `python/test_lbe1425.py --self-test`. For
   device-dependent behavior, give the exact hardware steps for the maintainer
   to run. If runtime validation isn't feasible, justify why code inspection
   suffices.
4. **Regression test** — a procedure or checks showing other functions remain
   unaffected (at minimum: clean `-Werror` build on all toolchains + self-test;
   for protocol changes, the specific commands that exercise nearby opcodes).

## Issue Filing Policy

- **Always audit the codebase before filing issues.** Issue descriptions must
  be derived from actual findings, not speculation about what might be wrong.
- Never assume a problem exists — verify it with concrete evidence (grep, file
  reads, build output) before writing it up. Every issue cites concrete
  `file:line` locations and a concrete failure scenario (inputs/state → wrong
  result).
- Mirror the review format: type (bug / latent / refactor), location(s),
  problem, impact, proposed fix.
- GitHub Issues must be enabled on the repo (Settings → Features → Issues).

## Build, Test & CI

- Build: `cmake -B build && cmake --build build` → binary at
  `build/bin/lbe-142x`. Linux needs `libudev-dev`.
- **Warnings are errors on every toolchain:** GCC/Clang `-Wall -Wextra
  -Werror`, MSVC `/W4 /WX`.
- **GCC-clean is NOT MSVC-clean.** `/W4 /WX` rejects things `-Wall -Wextra`
  silently allow — variable shadowing (C4456), unreachable code (C4702) — and
  these have broken the Windows build before. Before pushing C changes,
  pre-check with:
  `clang -fsyntax-only -Wshadow -Wunreachable-code-aggressive -Iinclude src/*.c`
  (skip the `*_windows.c` files locally — they need Windows headers). The
  Windows CI job (pinned `windows-2022`) is the real gate.
- Hardware-free parser test: `python3 python/test_lbe1425.py --self-test`
  (also runs in CI on Linux). Extend it when you add a command or change a
  decode.

## Architecture & Wire Conventions (orientation)

- Device dispatch is by USB PID → a model vtable (`struct lbe_model_ops`,
  `include/lbe_model.h`) selected in `src/lbe_device.c`. Per-model logic lives
  in `src/model_*.c`; UBX parsing is shared in `src/ubx.c`, GNSS rendering in
  `src/gnss_view.c`, NMEA in `src/nmea.c`. Transport is split
  `lbe_transport_{linux,windows}.c`. Prefer adding a vtable entry over a new
  PID `switch`/`if` in the device or CLI layer.
- Config goes over HID Feature reports on interface 2; **command opcode is
  payload byte 0**; report size is `LBE_REPORT_SIZE` (60). The 1425 reuses the
  1421 wire format plus its own opcodes.
- Every source/header carries the SPDX MIT + copyright header. C99, tabs for
  indentation.
