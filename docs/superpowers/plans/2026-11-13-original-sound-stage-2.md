# Original Sound, Stage 2 (the Godot window) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The game's sound plays in the Godot window, in step with the picture, with a mute key.

**Architecture:** The console (`rr_console`) owns a sound unit (stage 1) and a band-limited resampler that turns the chip's 53,267 samples a second into the sound card's rate as 32-bit floats. The window pulls what each frame made and pushes it into a Godot `AudioStreamGenerator`, nudging the resampling rate by at most half a percent to hold a steady amount buffered.

**Tech Stack:** C11, GDExtension C interface, GDScript (Godot 4.7).

**Spec:** `docs/superpowers/specs/2026-11-13-original-sound-design.md` (stage 2: requirements 4, 5, 6; 1 and 7 still hold).

## Global Constraints

- Nothing ROM-derived is committed. Tests are synthetic.
- Sound off (the default for `rr_console_open`) leaves every existing test and measurement unchanged. The window turns it on.
- `godot/bin/librr2.dylib` is rebuilt only when no Godot window is running (check with `ps aux | grep -i "[G]odot.app"`), and this is said before the rebuild.
- Pitch is never changed by more than 0.5% by the rate nudge.
- Claims in `analysis/` are marked verified or hypothesis.
- Commit at the end of each task on branch `sound`.

## Review Focus

- The window is halted, or loses focus for minutes, then resumes: no burst of stale sound, no growing delay.
- Reset (F5) in the middle of a note: silence, then sound from the start; nothing stuck.
- A sound card rate other than 48,000 (44,100, 96,000): correct pitch, no crash.
- `take_sound` asked for with rate 0 or a negative or absurd rate: nothing returned, no crash.
- Game speed keys 2 to 4 with sound on: the same amount of sound per frame.

---

### Task 1: The resampler

**Files:** Create `port/host/rr_resample.h`, `port/host/rr_resample.c`; Test `port/tests/test_resample.c`; Modify `port/Makefile` (HOST_SRC/HOST_HDR, `test_resample` in `test`).

**Interfaces (Produces):**

```c
typedef struct RrResample RrResample;     /* stereo, int16 in, float out (-1..1) */
RrResample *rr_resample_new(double in_hz);
void rr_resample_free(RrResample *r);
void rr_resample_reset(RrResample *r);    /* forget history */
/* Feed `in_frames` stereo frames; write at most `max_out` frames at `out_hz`.
 * Returns frames written; *used is how many input frames were consumed. */
size_t rr_resample_run(RrResample *r, double out_hz, const int16_t *in, size_t in_frames,
                       size_t *used, float *out, size_t max_out);
```

A windowed-sinc filter, 16 taps a side at most, cut off just below half the lower of the two rates. `out_hz` outside 8,000..384,000 returns 0 and uses nothing.

- [ ] Tests first: a constant input comes out constant at value/32768 (±1%); a 1 kHz sine keeps its amplitude within 3% and its zero-crossing count; a 25 kHz sine into 48,000 comes out at least 30 dB down; over 10 s of input the output count is `in * out_hz / in_hz` within 2 frames; feeding the same input in chunks of 1, 7 and 889 frames gives the same output as one call (to 1e-6); a bad rate returns 0; left and right stay separate.
- [ ] Implement. `make -C port test` passes. Commit.

### Task 2: The console hands out sound

**Files:** Modify `port/host/rr_console.h`, `port/host/rr_console.c`; Test `port/tests/test_console.c`.

**Interfaces:** Consumes Task 1 and stage 1's `rr_sound`. Produces:

```c
void rr_console_set_sound(RrConsole *c, int on);  /* off as opened; survives reset */
int rr_console_sound(const RrConsole *c);
/* Sound made since the last call, at out_hz, interleaved left/right floats.
 * Returns frames written (0 when off, halted with nothing left, or a bad rate). */
size_t rr_console_take_sound(RrConsole *c, double out_hz, float *out, size_t max_frames);
```

Turning sound on mid-run attaches a fresh unit (the Z80 stays in reset until the game next releases it; the window turns sound on before the first frame). Reset recreates the unit. Samples older than half a second that were never taken are dropped at each take, so a window that stopped asking does not replay stale sound.

- [ ] Tests first: off → take returns 0; on, 10 frames run → one take at 48,000 returns `10 * 896040 / 1008 * 48000 / 53267` within 20 frames, a second take 0; with cpu speed 200 the same count; after `rr_console_reset` sound is still on and a take after 1 frame returns about one frame's worth; 120 frames run with no take, then a take returns no more than 0.5 s of sound; rate 0 → 0; turning it off then taking → 0; the existing race tests still pass with sound on for one of them (RAM unchanged against the sound-off run).
- [ ] Implement. Passes. Commit.

### Task 3: The Godot class and the window

**Files:** Modify `port/host/rr_godot.c` (methods `set_sound(on: bool)`, `get_sound() -> bool`, `take_sound(rate: int) -> PackedByteArray` of little-endian float32 pairs), `godot/scripts/console.gd`, `godot/tests/test_console.gd`, `godot/tests/test_console_scene.gd`, `godot/README.md`.

**Window behaviour:**
- On start, if the console has `set_sound`, turn it on and create an `AudioStreamPlayer` with an `AudioStreamGenerator` (mix rate = `AudioServer.get_mix_rate()`, buffer 0.2 s) and play it.
- After each frame: `asked_rate = mix_rate * (1 + nudge)`, where `nudge = clamp((TARGET - buffered) / TARGET * 0.005, -0.005, 0.005)`, `buffered` is the seconds waiting in the generator and `TARGET = 0.05`; push what `take_sound(asked_rate)` returns (as many frames as fit).
- **M** mutes and unmutes (the player's volume; sound keeps flowing so unmuting is instant), with a message "Sound off" / "Sound on", and a line in the Tab list: `M  Sound: on|off|not available`.
- F5 and a halt clear what is waiting in the generator.
- Test helpers: `sound_on() -> bool`, `sound_muted() -> bool`, `sound_buffered_seconds() -> float`, `sound_asked_rate() -> float`.

- [ ] Scene tests first (stub console with `set_sound`, `get_sound`, `take_sound` returning a known number of frames and recording the rate asked): sound is turned on at start; each tick calls `take_sound` once with a rate within 0.5% of the mix rate; M toggles `sound_muted()` and the message; the Tab list has the M line; a stub without `set_sound` shows "not available" and M says so; a halt stops the calls.
- [ ] Real-class test in `test_console.gd`: `take_sound(48000)` after frames returns a byte array whose size is a multiple of 8.
- [ ] Say that the library is about to be rebuilt; check no Godot window is running; `scripts/build-godot-extension.sh`; run the Godot tests through pytest.
- [ ] Commit.

### Task 4: Measure and write down

**Files:** Modify `analysis/sound.md`, `README.md`, `godot/README.md`, `docs/building.md` if needed.

- [ ] A short windowed run with a probe script (not committed) that plays 30 s from boot and prints, once a second: seconds buffered in the generator, `AudioServer.get_output_latency()`, the asked rate. Report median and worst buffered, total delay (buffered + output latency), and whether the generator ever ran empty after the first second.
- [ ] Frame cost with sound on in the window build (console `run_frame` timing over 600 frames, crisp picture on) against sound off.
- [ ] Tempo at 200%: count FM music register writes per second of hardware time at 100% and 200% over the same 60 s from boot in the replay tool (`--cpu-speed`), report.
- [ ] Write section 7 of `analysis/sound.md` (verified/hypothesis), update READMEs. Full suites. Commit.
