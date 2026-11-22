# Race Conformance and Smoothness Experiment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure whether a natively played race is the same race as the original, pass by pass, and then run the experiment the project started with: let the game loop run every frame and see whether the race keeps its speed.

**Architecture:** MAME replays the owner's `full-throttle` recording once more, stopping at every game-loop handoff (`0xDF48`) so a Lua script can save work RAM there. A native replay tool runs the same race from power-on with no hardware replay: at each handoff it sets the controller to what the original's game code read during that pass (taken from the next handoff's RAM), optionally forces the recorded timestep, and compares all of work RAM with MAME's. That gives two numbers: how often the native build's own timing produces the original timestep, and whether the game logic is bit-identical when it does. The smoothness experiment then scales the 68000's speed in the hardware model and repeats the race, comparing distance, speed and finishing time.

**Tech Stack:** C11 with make; Python tools; MAME 0.289 debugger plus Lua; Godot only for a final look.

**Spec:** `docs/superpowers/specs/2026-11-10-native-port-design.md` (Stage 2 whole-capture conformance; Stage 3b). Background: `analysis/video-timing.md`, `analysis/bike-physics.md` ("Timestep scheduler").

## Global Constraints

- RR206 only (SHA-256 `c7a609019b1f052bcee6204a3a09ea77a5150a748de90b51382fedf4df207848`); every tool verifies the ROM.
- Nothing ROM-derived or recording-derived is committed: handoff RAM, input logs and pictures stay under the ignored `generated/`.
- The comparison never passes by default: a missing or short reference, a handoff count that does not match, or an excluded RAM range that is not named in the report is a failure.
- Every RAM range excluded from comparison is listed in the tool's output with a reason, and in the findings. Unexplained differences are failures (spec, Stage 2).
- No static mutable state in `port/core`, `port/hw` or generated code beyond the existing fatal handler.
- C11 `-std=c11 -Wall -Wextra -Werror -O2`; Python ruff clean; MAME debugger numbers `0x`-prefixed, `-debugger osx`, MAME stopped by the script.
- Tests needing the ROM, MAME output, Godot or a compiler skip cleanly when absent.
- Do not rebuild `godot/bin/librr2.dylib` while a Godot window may be using it; say so before the final rebuild.
- Claims in `analysis/` are labelled verified or hypothesis. The owner's standard applies: "it plays the same" is only written where a measurement supports it.

## Review Focus

1. **Reference and replay out of step** (the native race reaches a handoff the reference does not have, or ends early): reported with both counts and a non-zero exit, never a silent truncated comparison. Test in Task 3.
2. **Forced timestep hides a real difference**: when the tool forces the recorded timestep, it must still count and report every pass where the native build's own value differed. Test in Task 3.
3. **Excluded ranges swallow a bug**: exclusions are byte ranges given on the command line; the report prints how many differing bytes fell inside each. A range that hides thousands of bytes is visible. Test in Task 3.
4. **CPU speed scale at its edges** (scale 1 must be bit-identical to today; a scale so high that a frame runs millions of instructions must not hang): Test in Task 5.
5. **Controller input applied a pass late or early**: the input for pass p is what the original read during pass p. Pinned by a test with a synthetic two-handoff reference. Test in Task 3.

## File Structure

```
mame/rr206-handoff-ram.lua            saves work RAM each time the debugger stops at 0xDF48
scripts/run-mame-handoff-capture-rr206.sh
src/rr2port/handoffs.py               reader/writer for the handoff RAM file; summary helpers
port/tools/rr_race_replay.c           native replay and comparison (rr-race-replay)
port/hw/rr_genesis.c, rr_genesis.h    + CPU speed scale
port/host/rr_console.c, rr_console.h  + rr_console_set_cpu_speed
port/host/rr_godot.c                  + set_cpu_speed
godot/scripts/console.gd              + keys to change speed, on-screen indicator
analysis/race-conformance.md          written in Task 4
analysis/smoothness-experiment.md     written in Task 6
```

---

### Task 1: Handoff RAM file format

**Files:**
- Create: `src/rr2port/handoffs.py`, `tests/port/test_handoffs.py`

**Interfaces (produces):**
- File `handoffs.bin`: ASCII `RR2HAND1`, then for each handoff in order: big-endian `u32 index`, `u32 changed_count`, then `changed_count × (u16 offset, u8 value)` relative to the previous handoff. Handoff 0 is relative to an all-zero image, so the file is self-contained.
- `write_handoffs(path, images: Iterable[bytes]) -> int`
- `iter_handoffs(path) -> Iterator[tuple[int, bytes]]` yielding (index, full 64 KiB image); raises `HandoffFormatError(index, reason)` for a bad magic, an index out of sequence, a truncated record or an offset outside RAM.
- `handoff_count(path) -> int`

- [ ] **Step 1: Write failing tests**: round trip of three images; `handoff_count`; bad magic; truncated final record names the index; index out of sequence; an image that is not 65,536 bytes is rejected on write.
- [ ] **Step 2: Run and see them fail. Step 3: Implement. Step 4: Run** `uv run --group dev pytest tests/port -q`.
- [ ] **Step 5: Commit** — `git commit -m "Add handoff RAM file format"`

---

### Task 2: Capture handoff RAM from MAME

**Files:**
- Create: `mame/rr206-handoff-ram.lua`, `scripts/run-mame-handoff-capture-rr206.sh`
- Modify: `src/rr2port/mame_cmd.py` (`build_handoff_stop_cmd`), `tests/port/test_mame_cmd.py`, `mame/README.md`

**Interfaces:**
- `build_handoff_stop_cmd() -> str`: debugger script with one breakpoint at `0xdf48` that stops every time (no action), then `g`.
- The Lua script, as in `rr206-vdp-state.lua`: a periodic callback that, when the debugger is stopped with the PC at `0xDF48`, appends a record to `RR2_HANDOFF_OUT` in the Task 1 format, also appends `D1` (the timestep about to be stored) as a text line `<index> <d1 low word>` to `RR2_HANDOFF_OUT.timesteps`, and resumes with the debugger's `go` command. It exits the machine when playback ends.
- `scripts/run-mame-handoff-capture-rr206.sh <capture>` → `generated/port/handoffs/<capture>.bin` and `.timesteps`. MAME runs in the background; the script stops it when playback has ended and the output has stopped growing, with a time limit, and fails if the handoff count differs from the number of `RR2TICK` rows in `generated/mame/<capture>.timestep.log` when that log exists (2,176 for `full-throttle`).

- [ ] **Step 1: Write the failing test** for `build_handoff_stop_cmd` (one `bp 0xdf48` line without `g` in its action; last line `g`).
- [ ] **Step 2–3: Fail, implement.**
- [ ] **Step 4: Capture** `full-throttle`. Expected: 2,176 handoffs; `.timesteps` equals the timestep column of the existing log; `uv run python -c` check that handoff 500's image equals `generated/port/traces/race/handoff-00.bin` byte for byte (the race trace window starts at handoff 500).
- [ ] **Step 5: Commit** — `git commit -m "Capture work RAM at every game-loop handoff from MAME"`

---

### Task 3: Native race replay and comparison

**Files:**
- Create: `port/tools/rr_race_replay.c`, `tests/port/test_race_replay.py`
- Modify: `port/Makefile` (target `race-replay`), `port/hw/rr_genesis.h`/`.c` (a per-instruction hook used only by tools: `void (*step_hook)(RrGenesis *, void *)`, called before each instruction when set)

**Interfaces (produces):** `rr-race-replay --rom ROM --handoffs FILE --timesteps FILE [--force-timestep] [--exclude START:END:REASON]... [--max-frames N] [--report FILE]`

Behaviour:
1. Run from reset on the native hardware model, self-timed.
2. Each time the PC is `0xDF48` (handoff k, counted from 0):
   - note the native timestep (low word of `D1`) and compare with the recorded one; count mismatches; with `--force-timestep`, replace `D1`'s low word with the recorded value;
   - compare work RAM with reference handoff k, outside the excluded ranges; record the number of differing bytes and, for the first handoff that differs, the first ten offsets;
   - set controller 1 and 2 for the coming pass from reference handoff k+1's bytes at `0xFF062A` and `0xFF062B` (the raw active-high pad bytes the original read during pass k); for the last handoff, release everything.
3. Stop at the last reference handoff. Reaching more native handoffs than the reference has, or `--max-frames` before the last one, is exit 1 with both counts.
4. Print a summary and, with `--report`, one line per handoff: `k frame native_timestep recorded_timestep differing_bytes`.

Summary lines (exact prefixes, for tests): `HANDOFFS compared=<n> of <total>`, `TIMESTEP same=<n> different=<n> forced=<0|1>`, `RAM identical=<n> differing=<n> first_differing=<k|none>`, one `EXCLUDED start=<hex> end=<hex> bytes_hidden=<n> reason=<text>` per range, then `RESULT match` (every compared handoff identical and, without `--force-timestep`, every timestep the same), `RESULT logic-match` (identical with forced timesteps but some native timesteps differed) or `RESULT differ`. Exit 0 for `match` and `logic-match`, 1 for `differ` or out-of-step, 2 for usage or input errors.

The raw pad byte layout comes from the routine at `0x10F4` (`initial-findings.md`); confirm the bit order against the first handoffs where a button is down in the reference and record it in a comment. The conversion to `RR_PAD_*` bits lives in one function with a unit test.

- [ ] **Step 1: Write failing tests** in `tests/port/test_race_replay.py`, with a synthetic ROM built through the harness whose code loops through a fake "handoff" at a test address (the tool takes the handoff address from a `-DHANDOFF_PC=` build define, default `0xDF48`), increments a RAM counter each pass and stores the pad byte it reads:
  - identical reference → `RESULT match`, exit 0;
  - reference with one byte changed at handoff 2 → `RESULT differ`, `first_differing=2`, exit 1;
  - the same with `--exclude` covering that byte → exit 0 and `bytes_hidden=1` (Review Focus 3);
  - reference timesteps differing from native: without `--force-timestep` → `RESULT differ`; with it → `TIMESTEP same=… different=1 forced=1` and `RESULT logic-match` (Review Focus 2);
  - reference with fewer handoffs than the code produces before `--max-frames` → still exit 0 at the last reference handoff; reference with more than the native run reaches → exit 1 with both counts (Review Focus 1);
  - the pad byte stored during pass k equals reference handoff k+1's pad byte (Review Focus 5);
  - truncated handoff file → exit 2 naming the handoff index.
- [ ] **Step 2–3: Fail, implement.**
- [ ] **Step 4: Run tests**, `make -C port race-replay`.
- [ ] **Step 5: Commit** — `git commit -m "Add native race replay and handoff comparison"`

---

### Task 4: Measure the race, explain every difference

**Files:**
- Create: `analysis/race-conformance.md`
- Modify: `README.md`

- [ ] **Step 1: Free run.** `rr-race-replay` on `full-throttle` without `--force-timestep` or exclusions. Record: how many handoffs before the first difference, the timestep agreement, the first differing offsets.
- [ ] **Step 2: Forced timesteps.** The same with `--force-timestep`. Record the result.
- [ ] **Step 3: Explain.** For each RAM range that differs, find the cause (use `analysis/memory-map.md`, the trace tools and `rr-trace-check`), and either fix the port (with a failing test first, in the owning module) or justify an exclusion (for example, state written by the per-frame interrupt handler when the native pass spanned a different number of frames). Rerun until the forced run is `logic-match` or `match`, or the remaining differences are understood and listed.
- [ ] **Step 4: Cross-check the method.** The run must agree with what is already known: handoffs 500–520 of the forced run are identical to the race trace window's `handoff-NN.bin` files.
- [ ] **Step 5: Write `analysis/race-conformance.md`** (verified/hypothesis): the two results as numbers; the native timestep histogram beside the original's (3:7 4:294 5:911 6:721 7:200 8:41 9:2); every exclusion with its reason and hidden-byte count; every port bug found and fixed; what the result does and does not show (one race, one track, one bike, no combat, no cops); and a plain statement of what can now be claimed about "plays the same".
- [ ] **Step 6: Commit** — `git commit -m "Measure a native race against the original, pass by pass"`

---

### Task 5: CPU speed scale

**Files:**
- Modify: `port/hw/rr_genesis.h`, `port/hw/rr_genesis.c`, `port/host/rr_console.h`, `port/host/rr_console.c`, `port/host/rr_godot.c`, `port/tools/rr_race_replay.c` (`--cpu-speed N`), `godot/scripts/console.gd`, `port/tests/test_console.c`, `godot/tests/test_console.gd`, `tests/port/test_race_replay.py`

**Interfaces (produces):**
- `void rr_genesis_set_cpu_speed(RrGenesis *g, unsigned percent)`: 100 is the original. At 800 the 68000 executes eight times as many cycles per video line; the video chip's timing (lines per frame, interrupt positions, counters) is unchanged as seen by the game. Implemented by charging the hardware model's clock `cycles × 100 / percent`, keeping the remainder so no time is lost. Range 100–6400; anything else is clamped.
- `rr_console_set_cpu_speed(c, percent)`; Godot `set_cpu_speed(percent: int)` and `get_cpu_speed() -> int`.
- In the Godot scene: `1`–`4` select 100%, 200%, 400%, 800%; the current speed is shown for two seconds after a change.
- `rr-race-replay --cpu-speed N`.

- [ ] **Step 1: Write failing tests**: at 100 the frame-timing tests are unchanged and a 100-frame run executes the same number of instructions as before the change (pin the number first); at 800 a frame of the 10-cycle loop executes eight times as many instructions (±1 per line); a frame still raises exactly one frame interrupt on line 224; out-of-range values clamp; at 6400 a frame returns (Review Focus 4); Godot `get_cpu_speed()` round-trips.
- [ ] **Step 2–3: Fail, implement.**
- [ ] **Step 4: Run** all suites; rerun the 20 boot pictures and both trace windows at 100. Expected: unchanged.
- [ ] **Step 5: Commit** — `git commit -m "Add a CPU speed scale to the hardware model"`

---

### Task 6: The smoothness experiment

**Files:**
- Create: `analysis/smoothness-experiment.md`
- Modify: `port/tools/rr_race_replay.c` (`--track ADDR:SIZE:NAME` to print a RAM value at every handoff into the report), tests for it

The question (from `analysis/bike-physics.md`): the race loop takes about 5.4 frames a pass and scales motion by the frames elapsed. With a faster CPU, does it update more often at the same race speed?

- [ ] **Step 1: Add `--track`** with a failing test first (a tracked word appears in each report line).
- [ ] **Step 2: Baseline at 100%.** Replay `full-throttle` (inputs from the reference, no forced timestep), tracking player speed (`0xFF0A74`), the per-pass motion long at object `+0x26` (`0xFF0A88`), and the timestep. Record passes, frames, the sum of motion over the race, top speed, and the frame at which the race ends (the last reference handoff's frame).
- [ ] **Step 3: Repeat at 200%, 400%, 800%.** The reference's handoffs no longer line up, so RAM is not compared; inputs are applied by frame instead: for each native frame, the pad state the reference held at the handoff whose frame number is nearest (add `--inputs-by-frame`, with a test). Record the same quantities plus passes per frame and the timestep histogram.
- [ ] **Step 4: Decide what the numbers say.** Same distance and finishing frame within 1% at higher speeds, with timesteps of 1–2: the engine is frame-rate independent in this race. Otherwise: identify what changed (a second pass inside one frame reusing a stale timestep, logic that counts passes not frames) and name the code involved.
- [ ] **Step 5: Look at it.** Tell the owner the library will be rebuilt, rebuild (`scripts/build-godot-extension.sh`), and give the key to press. The owner's impression goes in the findings as theirs.
- [ ] **Step 6: Write `analysis/smoothness-experiment.md`**: the table of runs, the answer to the question with its evidence, what feels different and why (controls answer faster), what breaks if anything, and the recommendation for Stage 3 (ship the faster loop, or interpolate, or both as options).
- [ ] **Step 7: Commit and stop for review** — `git commit -m "Run the smoothness experiment: the race loop at higher CPU speeds"`
