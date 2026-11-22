# Original Sound, Stage 1 (replay tool) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The replay tool can run the game's Z80 sound program beside the game, write the sound to a WAV and log every FM chip write, and that log is compared with the original's from MAME.

**Architecture:** A new unit `port/hw/rr_sound.[ch]` owns a Z80 core, a YM2612 model, the Z80's memory map and a sample buffer. It is driven by hardware time (master clocks) and catches up whenever the Genesis model tells it the 68000 touched it. `RrGenesis` holds an optional pointer to it; NULL (the default) is today's behaviour. The two cores are vendored unchanged under `port/third_party/`.

**Tech Stack:** C11 (port), Python via uv (comparison tool, tests), MAME 0.289 with Lua (reference log), sh.

**Spec:** `docs/superpowers/specs/2026-11-13-original-sound-design.md` (stage 1: requirements 1, 2, 3, 7).

## Global Constraints

- Nothing derived from the ROM is committed: WAVs, FM logs, Z80 memory images go under `generated/`. Test data is synthetic.
- With no sound unit attached every existing test and measurement is unchanged; with one attached `rr-race-replay` on `full-throttle` still ends `RESULT match`.
- Vendored cores are byte-for-byte as downloaded (superzazu/z80 at d64fe10, MIT; nukeykt/Nuked-OPN2 at 335747d, LGPL-2.1-or-later), each with its licence file; they are compiled with their own flags (`-std=c11 -O2 -w`), not the port's `-Werror`.
- Claims in `analysis/` are marked **verified** or **hypothesis**.
- `godot/bin/librr2.dylib` is not rebuilt in this stage.
- Commit at the end of each task on branch `sound`.

## Review Focus

- The 68000 holds the Z80's bus or reset for a long time (thousands of frames): the Z80 must stand still, time must still pass for the FM chip, and no arithmetic may overflow or loop per cycle.
- The game speed is changed (cpu_percent 200): the Z80 and chip follow hardware time, so the sample count per frame does not change.
- The sample buffer is never taken (replay without `--sound-wav`, but with `--fm-log`): it must not grow without bound.
- The Z80 reads the ROM window with a bank beyond the ROM's end: reads give 0xFF, no out-of-bounds access.
- FM logs of different lengths, or empty, given to the comparison tool: a report, not a crash.

---

### Task 1: Vendor the cores and build them

**Files:**
- Create: `port/third_party/z80/{z80.c,z80.h,LICENSE}`, `port/third_party/nuked-opn2/{ym3438.c,ym3438.h,LICENSE}`, `port/third_party/README.md` (source URL, commit, licence, "unchanged")
- Modify: `port/Makefile` (objects `$(BUILD)/tp_z80.o`, `$(BUILD)/tp_ym3438.o` built with `-std=c11 -O2 -w`; variable `SOUND_OBJ`; `test_sound` in `test`)
- Test: `port/tests/test_sound.c`

- [ ] Write `test_cores_link`: a Z80 with a 4-byte program `3E 2A 76` (LD A,0x2A; HALT) stepped 2 times has `a == 0x2A` and `halted`; a YM chip after `OPN2_Reset` clocked 24 times gives 0 output sum. Run `make -C port test`: fails (no such files).
- [ ] Copy the cores from the download, add the Makefile rules. Run: passes.
- [ ] Commit.

### Task 2: The sound unit

**Files:** Create `port/hw/rr_sound.h`, `port/hw/rr_sound.c`; Test `port/tests/test_sound.c`; Modify `port/Makefile` (HW_SRC/HW_HDR gain rr_sound; every target that links HW_SRC also links `$(SOUND_OBJ)`).

**Interfaces (Produces):**

```c
#define RR_SOUND_HZ 53267            /* master / 7 / 144 */
#define RR_SOUND_MASTER_PER_Z80 15
#define RR_SOUND_MASTER_PER_FM_CLOCK 42   /* 24 of these make one sample */
typedef struct RrSound RrSound;
typedef void (*RrSoundFmLog)(void *ctx, uint64_t master, unsigned port, uint8_t value);

RrSound *rr_sound_new(const uint8_t *rom, uint32_t rom_size, uint8_t *z80_ram /* 0x2000 bytes, shared */);
void rr_sound_free(RrSound *s);
void rr_sound_run_to(RrSound *s, uint64_t master);          /* catch up; never goes back */
void rr_sound_set_bus_held(RrSound *s, uint64_t master, int held);   /* 68000 has the Z80's bus */
void rr_sound_set_reset_held(RrSound *s, uint64_t master, int held); /* also resets the FM chip when taken */
void rr_sound_fm_write(RrSound *s, uint64_t master, unsigned port, uint8_t value); /* a 68000 write to A04000..3 */
void rr_sound_set_fm_log(RrSound *s, RrSoundFmLog log, void *ctx);   /* Z80 and 68000 writes alike */
size_t rr_sound_available(const RrSound *s);                 /* stereo frames waiting */
size_t rr_sound_take(RrSound *s, int16_t *out, size_t frames); /* interleaved L R; returns frames copied */
uint64_t rr_sound_z80_steps(const RrSound *s);               /* instructions run, for tests and reports */
```

Decisions the tests pin:
- Z80 map: 0000-3FFF the shared RAM (mirrored every 0x2000); 4000-5FFF FM (port = addr & 3; reads give `OPN2_Read`); 6000-60FF bank register (each write shifts bit 0 in at the top of a 9-bit value); 7F11 PSG (ignored in stage 1, counted); 8000-FFFF ROM at `(bank << 15) | (addr & 0x7FFF)`, 0xFF past the end.
- At creation the reset line is held (the Z80 does not run until the game releases it). Releasing reset starts the Z80 at address 0 with interrupts as after `z80_init`.
- While bus or reset is held the Z80 runs no instruction; the FM chip's time still passes.
- The Z80's interrupt is raised once per frame of `3420 * 262` master clocks, at master time `224 * 3420 + 205` into the frame, and withdrawn `3420` master clocks later if not taken.
- One sample is the sum of 24 FM clocks' output times 8, clipped to 16 bits.
- The buffer holds at most 4 seconds; when full the oldest samples are dropped.

- [ ] Tests first (each written, seen failing, then made to pass), all in `test_sound.c` with hand-assembled programs placed in a local 0x2000-byte RAM:
  - `test_held_at_start`: run_to one frame with reset never released → `rr_sound_z80_steps == 0`, `rr_sound_available == 888 ± 1` (896040 / 1008).
  - `test_runs_after_reset_release`: program `3E 2A 32 00 10 76` (LD A,0x2A; LD (0x1000),A; HALT); release reset at 0, run_to 1000 master → `ram[0x1000] == 0x2A`.
  - `test_bus_held_stops_z80`: same program, bus held from 0 → after run_to 100000, `ram[0x1000] == 0`; released → next run_to writes it.
  - `test_fm_write_logged`: program `3E 2B 32 00 40 3E 80 32 01 40 76` → log receives exactly `(port 0, 0x2B)`, `(port 1, 0x80)` in order with increasing master times.
  - `test_bank_and_rom`: 9 writes to 0x6000 of bits forming bank 1 (first write bit 1, then eight 0), then `3A 34 92` (LD A,(0x9234)) and store → RAM holds `rom[0x8000 + 0x1234]`; with a bank past a 0x10000-byte ROM the value read is 0xFF.
  - `test_interrupt_each_frame`: program `ED 56 FB 76` at 0 (IM 1; EI; HALT) with `34 C9`... at 0x38 a handler that increments `(0x1000)` then `FB C9`; run 3 frames → `ram[0x1000] == 3`.
  - `test_long_hold`: bus held, run_to `896040ull * 60 * 3600` returns promptly (< 1 s is not asserted; the test asserts `rr_sound_available <= 4 * RR_SOUND_HZ` and steps == 0).
  - `test_take`: take fewer than available returns that many and leaves the rest in order.
  - `test_tone`: 68000-side `rr_sound_fm_write` sequence that keys on a sine on channel 1 (algorithm 7, operator 4, total level 0, attack 31, frequency block 4 / fnum 0x269, key on) → over the next 0.1 s the samples are not all zero and cross zero between 35 and 55 times (440 Hz ± 25%).
- [ ] Implement `rr_sound.c` to those tests.
- [ ] `make -C port test` passes. Commit.

### Task 3: The Genesis model drives it

**Files:** Modify `port/hw/rr_genesis.h` (field `RrSound *sound;` documented "NULL: no sound, the default"), `port/hw/rr_genesis.c`; Test `port/tests/test_sound.c` (or the existing Genesis test file if one fits).

**Interfaces:** Consumes Task 2. Produces: with `g->sound` set, (a) every 68000 write or read in `0xA00000-0xA0FFFF` first calls `rr_sound_run_to(rr_genesis_master_time(g))`; writes to `A04000-3` call `rr_sound_fm_write`; (b) writes to `A11100`/`A11200` call `rr_sound_set_bus_held` / `rr_sound_set_reset_held` with the new state; (c) the end of every `rr_genesis_run_frame` calls `rr_sound_run_to`; (d) a reset of the model (`rr_genesis_reset` or its equivalent) holds the Z80's reset.

- [ ] Test `test_genesis_drives_sound`: a Genesis with a sound unit; through the model's write function, write a Z80 program into `0xA00000..`, hold then release reset via `0xA11200`, release the bus via `0xA11100`; run one frame → the program's marker byte is in `g->z80_ram`. Without a sound unit the same writes leave the marker 0 and nothing crashes. Fails first.
- [ ] Test `test_speed_does_not_change_sample_count`: cpu_percent 200, run 10 frames → `rr_sound_available` within 2 of `10 * 896040 / 1008`.
- [ ] Implement. `make -C port test` passes. Commit.

### Task 4: Replay tool options

**Files:** Modify `port/tools/rr_race_replay.c`; Test `tests/port/test_race_replay.py`.

**Interfaces:** `--sound-wav FILE` writes 16-bit stereo WAV at 53,267 Hz of the whole run; `--fm-log FILE` writes one line per FM write: `<master clocks since power-on> <port 0-3> <value, two hex digits>`. Either option attaches a sound unit; neither means none. A final line `SOUND z80_steps=N fm_writes=N samples=N` is printed when attached.

- [ ] Test (synthetic ROM as the file's other tests): run with both options → exit status and RESULT line as without; WAV header says 2 channels, 53267 Hz, 16 bits, and its data length is `4 * samples`; the log file exists; the `SOUND` line is printed. Without the options: no `SOUND` line. Fails first.
- [ ] Implement (samples are taken and written after every frame). Passes. Commit.

### Task 5: The original's log, and the comparison tool

**Files:** Create `mame/rr206-fm-log.lua`, `scripts/run-mame-fm-log-rr206.sh` (modelled on `run-mame-handoff-capture-rr206.sh`; plays back `generated/mame/<name>.inp`, writes `generated/sound/<name>.mame-fm.log` and, with `-wavwrite`, `generated/sound/<name>.mame.wav`); `src/rr2port/fm_compare.py`; Modify `src/rr2port/cli.py`, `pyproject.toml` (`rr2-fm-compare`); Test `tests/port/test_fm_compare.py`, `tests/port/test_build_script.py`-style check that the script refuses a wrong ROM.

**Interfaces:** MAME log lines: `<seconds since power-on, 9 decimals> <port> <value hex>`, written from a write tap on the sound Z80's program space at 0x4000-0x4003 (and on the 68000's at 0xA04000-3). `rr2-fm-compare PORT_LOG MAME_LOG` pairs address and data writes into `(part, register, value)` events, and prints: `EVENTS port=N mame=N`, `SAME_IN_ORDER n` (length of the common prefix), `FIRST_DIFFERENCE index … | none`, `REGISTERS_DIFFERING …` (per-register counts when the multisets differ), `TIMING samples: median, p90, p99, max` of `port_time − mame_time` over the common prefix (port master clocks / 53693175 → seconds; × 53267 → samples), and `RESULT same | different`. Exit 0 same, 1 different, 2 bad input.

- [ ] Tests on hand-written logs: identical → `RESULT same`; one value changed → `FIRST_DIFFERENCE` names its index and both events; one log shorter → reported, exit 1; empty file → exit 1 with `EVENTS port=0`; a data write with no address write before it → counted as register "none", no crash; timing: port events 1008 master clocks later than MAME → median 1.0 sample. Fail first, then implement.
- [ ] Write the Lua tap and the script. Run it on `full-throttle` (MAME must be allowed to open a window briefly). Expected: a log of several hundred thousand lines.
- [ ] Commit (script, Lua, tool, tests; not the logs).

### Task 6: Measure, and write it down

**Files:** Create `analysis/sound.md`; Modify `README.md` (one paragraph), `docs/building.md` (third-party note).

- [ ] Run the replay with `--sound-wav generated/sound/full-throttle.port.wav --fm-log generated/sound/full-throttle.port-fm.log`: expect `RESULT match` (requirement 1).
- [ ] Run `rr2-fm-compare`. If `RESULT different`: find the cause (superpowers:systematic-debugging), correct the model with a test that fails first (likely places: when the Z80's interrupt arrives and how long it is held; the Z80's state after reset; bus-hold timing), re-measure. What cannot be explained is reported as unexplained with its counts.
- [ ] Time the replay with and without sound (cost per frame).
- [ ] Write `analysis/sound.md`: how the game makes sound (verified/hypothesis as in the spec), the model, the comparison's numbers, what is not covered (PSG, no crash/police recording, output filter), where the two WAVs are for listening.
- [ ] Full suites: `make -C port test`, `uv run --group dev pytest`, `uv run --group dev ruff check .`. Commit.
