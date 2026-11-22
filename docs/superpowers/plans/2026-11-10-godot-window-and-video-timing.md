# Godot Window and Video Timing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show the natively running game live in a Godot window with keyboard and gamepad input, and bring the hardware model's video timing (line counter, status flags, frame length) close enough to MAME for the race milestone to start.

**Architecture:** A small host-facing C API (`rr_console`) wraps the machine and hardware model and turns fatal errors into a "halted" state instead of ending the process. It is built with the generated code into one shared library. A Godot native extension written in plain C against Godot's own `gdextension_interface.h` exposes it as a `RoadRashConsole` class; a Godot scene runs one frame per 60 Hz physics tick, uploads the 320×224 picture to a texture and feeds controller state in. Separately, MAME's beam position is logged at line-counter reads and the model's counters are fitted to it.

**Tech Stack:** C11 with make; Godot 4.7 (installed at `/Applications/Godot.app`), GDScript for the scene and tests; Python tools as before; MAME 0.289 debugger and Lua scripts.

**Spec:** `docs/superpowers/specs/2026-11-10-native-port-design.md` (Stage 1 milestone 3, "Hosts", "Complete state"). Background: `analysis/first-picture-findings.md` sections 2, 6 and 7.

## Global Constraints

- RR206 only (SHA-256 `c7a609019b1f052bcee6204a3a09ea77a5150a748de90b51382fedf4df207848`, CRC-32 `0876E992`); every tool and the extension verify the ROM before running it.
- Nothing ROM-derived is committed: no ROM bytes, generated C, built libraries, traces, pictures or dumps. Built libraries go under ignored paths (`generated/` and `godot/bin/`, which this plan adds to `.gitignore`).
- The ROM is loaded at run time from a path the user supplies (`RR2_ROM` environment variable, else the repository's default ROM file name one directory above the Godot project). The Godot project never copies it.
- No new build tools: make and the system C compiler only. No scons, no godot-cpp, unless Task 3's fallback is taken and recorded.
- Third-party code in `port/` or the extension must be BSD/MIT/zlib-class. Godot's `gdextension_interface.h` (MIT) is produced from the installed Godot at build time into `generated/` and is not committed.
- A runtime fatal error (untranslated address, unmodelled hardware, unimplemented instruction) must never terminate Godot. It halts the console and is shown on screen with its message.
- No static mutable state in `port/core`, `port/hw` or generated code, except the single fatal-handler hook, which is documented as process-wide.
- C11 `-std=c11 -Wall -Wextra -Werror -O2`; Python ruff clean (format only `src/rr2port`, `tests/port`); MAME debugger numbers `0x`-prefixed, `-debugger osx`; MAME is stopped by the script after any `snap`.
- Tests needing the ROM, MAME output, Godot or a compiler skip cleanly when absent.
- Claims about the original game in `analysis/` are labelled verified or hypothesis.

## Review Focus

1. **Fatal error while Godot is running** (the front end reaches untranslated code or an unmodelled hardware address): the window stays up, shows the message, and `run_frame` becomes a no-op; nothing calls `exit`. Tests in Task 1 and Task 4.
2. **Missing or wrong ROM** (no file, RR205, truncated): `load_rom` returns a message naming the path and the reason; the scene shows it; no crash. Tests in Task 1 and Task 3.
3. **Extension library missing or built for another ROM** (fresh checkout, stale build): Godot reports the extension as unavailable and the scene shows how to build it, instead of a script error. Test in Task 4.
4. **Input edge cases**: opposite directions held together, a gamepad unplugged mid-game, focus lost while a key is down: no stuck buttons, and left+right or up+down are never sent together. Test in Task 4.
5. **Frame pacing when the display is not 60 Hz or the window is dragged**: the game runs exactly one console frame per physics tick and never runs several to catch up after a stall longer than a few frames. Test in Task 4.

## File Structure

```
port/core/rr_run.c, rr_run.h        + fatal handler hook
port/host/rr_console.h, rr_console.c  host API: load, reset, input, run a frame, picture, halt state
port/host/rr_godot.c                 GDExtension entry point and RoadRashConsole class (plain C)
port/tests/test_console.c
port/Makefile                        + targets: console-lib, godot-extension
godot/rr2.gdextension                points Godot at godot/bin/librr2.dylib (ignored)
godot/scenes/console.tscn, godot/scripts/console.gd      the live window
godot/tests/test_console.gd          headless Godot test
scripts/build-godot-extension.sh, scripts/run-godot-console.sh
src/rr2port/hv.py                    beam-position log reader and counter fit
mame/rr206-vdp-state.lua             dumps MAME's video memory at a VBlank entry
analysis/video-timing.md             written in Task 8
```

---

## Part 1: the window

### Task 1: Host API and fatal errors that do not exit

**Files:**
- Create: `port/host/rr_console.h`, `port/host/rr_console.c`, `port/tests/test_console.c`
- Modify: `port/core/rr_run.h`, `port/core/rr_run.c`, `port/Makefile`

**Interfaces (produces):**

```c
/* rr_run.h: the handler receives the formatted message. If it returns, the
 * process exits with status 2 as before. Process-wide; NULL restores the default. */
typedef void (*RrFatalHandler)(const char *message, void *ctx);
void rr_set_fatal_handler(RrFatalHandler handler, void *ctx);

/* rr_console.h */
typedef struct RrConsole RrConsole;
enum { RR_CONSOLE_W = 320, RR_CONSOLE_H = 224 };
/* Returns NULL and fills `error` (never truncated mid-word, always terminated)
 * when the ROM cannot be read or is not the ROM the code was translated from. */
RrConsole *rr_console_open(const char *rom_path, char *error, size_t error_size);
void rr_console_close(RrConsole *c);
void rr_console_reset(RrConsole *c);                       /* 68000 reset; clears a halt */
void rr_console_set_buttons(RrConsole *c, int port, unsigned held);   /* RR_PAD_* bits */
/* Run one frame. Returns 1 if it ran, 0 if the console is halted. */
int rr_console_run_frame(RrConsole *c);
const uint32_t *rr_console_frame(const RrConsole *c);      /* 320*224 pixels, 0x00RRGGBB */
const char *rr_console_halt_message(const RrConsole *c);   /* NULL while running */
const uint8_t *rr_console_ram(const RrConsole *c);         /* 65,536 bytes */
uint64_t rr_console_frames(const RrConsole *c);
```

`rr_console_run_frame` installs a fatal handler that stores the message and `longjmp`s back out of the frame; the console is then halted until `rr_console_reset`. `rr_console_set_buttons` clears left when right is also held and up when down is also held (Review Focus 4, enforced here so every host gets it).

- [ ] **Step 1: Write failing tests** in `port/tests/test_console.c` (with a tiny hand-made code table like `test_core.c`, ROM written to a temp file): open succeeds on the matching ROM and fails with a message containing the path for a missing file and the two CRC values for a different file; `run_frame` returns 1 and `frames` counts; a ROM whose code jumps to an untranslated address halts: `run_frame` returns 0 from then on, `halt_message` contains `RR2FATAL` text with the address, and the test process is still alive; `reset` clears the halt; `set_buttons(0, LEFT|RIGHT|A)` leaves the model's `pad_buttons[0] == RIGHT|A`; with no handler installed `rr_fatal` still exits 2 (existing death tests keep passing).
- [ ] **Step 2: Run and see them fail** — `make -C port test`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `make -C port test && uv run --group dev pytest -q` → all pass.
- [ ] **Step 5: Commit** — `git commit -m "Add host console API; fatal errors can halt instead of exiting"`

---

### Task 2: Shared library

**Files:**
- Modify: `port/Makefile`, `.gitignore` (add `godot/bin/`)
- Create: `scripts/build-godot-extension.sh`

`make -C port console-lib` builds `generated/port/build/librr2core.a` from `port/core`, `port/hw`, `port/host/rr_console.c` and the generated code. `scripts/build-godot-extension.sh [ROM]` verifies the ROM hash, runs `rr2-translate`, then the make targets of this task and Task 3, and prints the path of the library it produced. It refuses to run without the ROM.

- [ ] **Step 1: Write the failing test** `tests/port/test_build_script.py`: running the script with a path to a 1 MiB zero file exits non-zero with "SHA-256 mismatch" and creates nothing under `godot/bin/`.
- [ ] **Step 2: Run and see it fail; Step 3: implement; Step 4: run tests**, then run the script on the real ROM. Expected: `librr2core.a` exists; `git status --short` is empty.
- [ ] **Step 5: Commit** — `git commit -m "Build the console core as a library"`

---

### Task 3: Godot extension (proof first)

**Files:**
- Create: `port/host/rr_godot.c`, `godot/rr2.gdextension`, `godot/tests/test_console.gd`, `tests/port/test_godot_extension.py`
- Modify: `port/Makefile` (target `godot-extension` → `godot/bin/librr2.dylib`), `godot/README.md`

**Approach.** Godot can load a native library that speaks its C interface directly. `Godot --headless --dump-gdextension-interface` writes `gdextension_interface.h`; the build runs that into `generated/port/godot/` and compiles `rr_godot.c` against it. The extension registers one class.

**Interfaces (produces), as seen from GDScript:**

```gdscript
class RoadRashConsole extends RefCounted
func load_rom(path: String) -> String      # "" on success, else the error message
func reset() -> void
func set_buttons(port: int, held: int) -> void
func run_frame() -> bool                   # false when halted or no ROM is loaded
func get_frame() -> PackedByteArray        # 320*224*4 bytes, RGBA8, alpha 255
func get_halt_message() -> String          # "" while running
func get_frame_count() -> int
const WIDTH = 320, HEIGHT = 224
const PAD_UP = 1, PAD_DOWN = 2, PAD_LEFT = 4, PAD_RIGHT = 8, PAD_B = 16, PAD_C = 32, PAD_A = 64, PAD_START = 128
```

This task is a proof, because the plain-C route is unverified here. **Fallback, to be taken only if the proof fails and recorded as a ruling with the reason:** build the same class with godot-cpp (which needs scons). Stop and report before taking it; it changes the project's build tools.

- [ ] **Step 1: Write the failing test** `godot/tests/test_console.gd` (run with `Godot --headless --path godot --script res://tests/test_console.gd`), which prints one `PASS`/`FAIL` line per check and exits non-zero on any failure:
  - `ClassDB.class_exists("RoadRashConsole")`;
  - `load_rom("/nonexistent.gen")` returns a message containing `/nonexistent.gen`; `run_frame()` is then `false`;
  - with the real ROM (path from `RR2_ROM`; skip these checks with a `SKIP` line if unset): `load_rom` returns `""`; after 600 `run_frame()` calls `get_frame().size() == 320 * 224 * 4`, `get_halt_message() == ""`, and the frame saved as PNG to `generated/port/godot/frame-0600.png`.
  `tests/port/test_godot_extension.py` runs that script when Godot, the built library and the ROM are present (skips otherwise), asserts exit 0, and compares the saved frame with a `rr-boot-run` picture taken at the same frame count (`--frames` semantics: add `--snap-frame N:FILE` to `rr-boot-run`, snapshotting after N whole frames, with its own unit test).
- [ ] **Step 2: Run and see it fail** (class does not exist).
- [ ] **Step 3: Implement `rr_godot.c`** and the make target. Keep it to the minimum the interface needs: library init, class registration, method binds with typed arguments, instance create/free.
- [ ] **Step 4: Run** the GDScript test and pytest. Expected: all `PASS`; the 600-frame picture is identical to `rr-boot-run`'s.
- [ ] **Step 5: Commit** — `git commit -m "Add Godot native extension exposing the console"`

---

### Task 4: The live window

**Files:**
- Create: `godot/scenes/console.tscn`, `godot/scripts/console.gd`, `godot/scripts/pad_input.gd`, `godot/tests/test_pad_input.gd`, `godot/tests/test_console_scene.gd`, `scripts/run-godot-console.sh`
- Modify: `godot/project.godot` (main scene, physics at 60 ticks, `max_physics_steps_per_frame = 1`, input actions for both pads if missing), `godot/README.md`

Behaviour:
- One `run_frame()` per `_physics_process`. With `max_physics_steps_per_frame = 1` a stall is not caught up (Review Focus 5).
- The picture is a 320×224 `ImageTexture` on a `TextureRect`, nearest-neighbour, scaled by the largest whole number that fits the window, centred, black bars.
- `pad_input.gd` exposes `static func buttons(player: int) -> int` built from input actions (keyboard and gamepad): arrows / d-pad / left stick, and `Z`,`X`,`C`,`Enter` (gamepad face buttons and Start) for A, B, C, Start. On `NOTIFICATION_APPLICATION_FOCUS_OUT` the scene sends 0 for both pads.
- If the class is missing, the scene shows: "Native library not built. Run scripts/build-godot-extension.sh". If `load_rom` fails or the console halts, the message is shown over the last picture, with "Press F5 to reset".
- `F5` resets; `F12` saves the current frame to `user://` and prints the path.

- [ ] **Step 1: Write failing tests** (headless GDScript, same PASS/FAIL convention):
  - `test_pad_input.gd`: with simulated action presses, left+right gives neither direction... the console layer already drops left; here assert the mask sent contains at most one of each pair, A/B/C/Start map to the right bits, releasing all gives 0, and a focus-out notification gives 0 while actions are still pressed.
  - `test_console_scene.gd`: instancing the scene with `RR2_ROM` unset shows the ROM-missing message and does not error; with the class hidden (scene property `console_class_name` set to a name that does not exist) it shows the build hint; with the real ROM, after 5 physics ticks the frame count is exactly 5, and after simulating a 500 ms stall followed by one tick it is 6, not more.
  - A halt test: the scene is given a console stub (a GDScript object with the same methods whose `run_frame` returns false and `get_halt_message` returns text) and must display the text and stop calling `run_frame`.
- [ ] **Step 2: Run and see them fail; Step 3: implement; Step 4: run** all Godot tests and pytest.
- [ ] **Step 5: Run it for real** — `./scripts/run-godot-console.sh`. Expected: logo, then title screen, at steady speed. Press Start and note in the commit message how far the front end gets before anything halts or looks wrong; each halt message names an address or a hardware register and becomes an item for Task 8's findings (do not fix them in this task).
- [ ] **Step 6: Commit** — `git commit -m "Add the live Godot console scene with keyboard and gamepad input"`

---

## Part 2: video timing

### Task 5: Known model corrections

**Files:**
- Modify: `port/hw/rr_vdp.c`, `port/hw/rr_vdp.h`, `port/hw/rr_vdp_render.c`, `port/hw/rr_genesis.c`, `port/tests/test_vdp.c`, `tests/port/test_boot_run.py`

The independent review of the last plan listed these; each gets a failing test first:

- [ ] Frame-interrupt flag: cleared when the level 6 interrupt is acknowledged (`rr_vdp_ack_frame_interrupt`, called on delivery), not by a status read. Test: flag still set on a second status read; clear after delivery.
- [ ] Sprite vertical position is 9 bits outside interlace. Test: a sprite with Y `0x280` appears at line 0.
- [ ] A register write also sets the address register's low 14 bits and the low code bits, as on hardware. Test: `0x8F02` then a data write lands at the address the register word implies.
- [ ] Frame length does not drift: each frame starts exactly 262 × 488 cycles after the previous one, whatever instruction straddled the boundary. Test: after 100 frames of a busy loop, `frame_start_cycles` is exactly `100 * 127856` past the first.
- [ ] No line interrupt is pending before the game has programmed one. Test: with line interrupts enabled from reset and register 10 = 50, the first is taken on line 50 or 51 of the first frame, not line 0.
- [ ] Re-run the 20-picture comparison and both trace windows. Expected: unchanged results.
- [ ] Commit — `git commit -m "Correct video status flag, sprite Y, register side effects and frame length"`

---

### Task 6: Fit the line counter to MAME

**Files:**
- Create: `src/rr2port/hv.py`, `tests/port/test_hv.py`, `scripts/run-mame-hv-log-rr206.sh`
- Modify: `src/rr2port/mame_cmd.py` (`build_hv_cmd`), `port/hw/rr_genesis.c`, `port/tests/test_vdp.c`, `mame/README.md`

- `build_hv_cmd(first_handoff, handoffs)` → debugger script for the race recording that, inside the same handoff range as the race trace window, logs every read of `0xC00008`–`0xC00009` as `RR2HV <addr> <size> <value> <beamx> <beamy> <totalcycles>`, and every level 4 and level 6 handler entry as `RR2INT <level> <beamx> <beamy> <totalcycles>`. Confirm the `beamx`/`beamy` symbols exist in MAME 0.289 before relying on them.
- `hv.py`: `read_hv_log(path)`; `fit_counters(samples) -> CounterFit` giving, as plain tables, the V counter value for each `beamy` and the H counter value for each `beamx`, plus the residual (samples the tables do not reproduce); `cycles_per_line` and the beam position at which each interrupt is entered.
- The model's `h_counter`, `v_counter`, blanking flags and the points in the line where interrupts are raised are rewritten from the fit. Constants taken from the fit are commented with the log they came from.

- [ ] **Step 1: Write failing tests**: parser accepts and rejects rows; `fit_counters` on a synthetic set recovers a known step table and reports an injected outlier as residual; `build_hv_cmd` has the two watchpoints and `0x` counts.
- [ ] **Step 2–3: Fail, implement.**
- [ ] **Step 4: Record and fit.** Run the script; print the fit. Expected: V is a function of `beamy` alone and H of `beamx` alone with zero residual (hypothesis; if not, the residual pattern is the finding).
- [ ] **Step 5: Write failing C tests** from the fit: for a handful of (cycle in frame → expected counter word) pairs spread over a frame, including the jump in the V counter and the jump in the H counter, the model must return the fitted value. Implement.
- [ ] **Step 6: Measure** with `rr-trace-check --shadow-hw` on the race window. Record `SHADOW addr=C00008` before and after. The shadow model's frame phase comes from when the recording took the frame interrupt, so agreement will not be total; the target is that the V counter byte agrees for at least 95% of reads, and the remaining error is characterised (how many lines off, and why).
- [ ] **Step 7: Commit** — `git commit -m "Fit the video line counter and interrupt positions to MAME"`

---

### Task 7: Settle the one-frame picture offset

**Files:**
- Create: `mame/rr206-vdp-state.lua`, `port/tools/rr_vdp_render_tool.c` (`rr-vdp-render`), `tests/port/test_vdp_render_tool.py`
- Modify: `port/Makefile`, `scripts/run-mame-boot-pictures-rr206.sh` (optional `--state N`), `mame/README.md`

- The Lua script, at the Nth VBlank handler entry, writes MAME's video memory (VRAM 64 KiB, CRAM, VSRAM, registers) to one file with a small header. First find how MAME 0.289 exposes the 315-5313's internal memory to Lua (save-state items of `:gen_vdp`); if it cannot be read, record that and stop this task with the question still open.
- `rr-vdp-render --state FILE -o PICTURE.ppm` loads that file into an `RrVdp` and renders it.

- [ ] **Step 1: Write failing tests**: `rr-vdp-render` on a hand-made state file (backdrop colour only) writes the expected PPM; a truncated state file is an error naming the missing part.
- [ ] **Step 2–3: Fail, implement.**
- [ ] **Step 4: Run** for N = 120 and N = 660 (both are frames where the picture is changing). Compare the rendering of MAME's memory at N with MAME's snapshot at N, the port's picture at N, and the port's at N−1.
  - Rendering equals the port's picture at N and MAME's snapshot equals N−1: the snapshot lags; the offset is confirmed as a property of the reference. Verified.
  - Rendering equals the port's picture at N−1: the port is a frame ahead of MAME's video memory. That is a port bug to investigate before anything else.
  - Neither: the renderer and MAME disagree on the same memory; the first differing pixel is the lead.
- [ ] **Step 5: Commit** — `git commit -m "Check the picture offset against MAME's own video memory"`

---

### Task 8: Findings

**Files:**
- Create: `analysis/video-timing.md`
- Modify: `analysis/first-picture-findings.md` (section 2 gets the Task 7 result), `README.md`

- [ ] Write `analysis/video-timing.md` (verified/hypothesis): the counter fit and its residual; interrupt positions; shadow agreement before and after; what Task 5 changed; the Task 7 result; what happened when the front end was driven in the Godot window (how far it got, every halt with its address or register, anything that looked wrong); and what the first race frame now needs.
- [ ] Commit and stop for the owner's review — `git commit -m "Add video timing findings"`
