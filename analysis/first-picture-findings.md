# First native picture: findings

Date: 2026-11-10. Plan: `docs/superpowers/plans/2026-11-10-discovery-and-first-picture.md`.
Statements are **verified** or a **hypothesis**. "MAME" is MAME 0.289's
`genesis` driver.

## 1. Result (verified)

RR206, translated to C, boots from the reset vector on the port's own hardware
code, with its own timing and no emulator involved, and draws the same
pictures as MAME.

- `rr-boot-run` runs 1,200 VBlank handler entries (1,317 frames, 12.1 million
  instructions) without reaching an untranslated address or an unmodelled
  hardware address.
- MAME snapshots were taken at every 60th VBlank handler entry from 60 to
  1,200. **All 20 are pixel-identical** to the port's picture taken one
  handler entry earlier. They cover the Electronic Arts logo while it
  animates (entries 60–180), the finished logo (240–360), a black frame
  (420), and the title screen with four different credit lines (480–1,200).
- Taken at the same entry number, 16 of the 20 are identical. The four that
  differ are the only ones where the picture is changing from one frame to
  the next: the logo animating (60, 120, 180) and a credit line changing
  (660). Each matches exactly at an offset of one entry, and three of them
  match at no other offset from −3 to +3. The other 16 are static across
  neighbouring frames, so they say nothing about the offset.
- Speed: 1,317 frames in 1.15 s, about 19 times real time, including
  rendering every line.

Reproduce:

```sh
./scripts/run-mame-boot-pictures-rr206.sh
uv run rr2-translate "Road Rash II (USA, Europe) (RR206).gen" \
  --annotations port/annotations/rr206.toml -o generated/port/rr206_code.c
make -C port boot-run
generated/port/build/rr-boot-run --rom "Road Rash II (USA, Europe) (RR206).gen" \
  --snap 599:generated/port/native/title.ppm
uv run python -m rr2port.picture generated/port/pictures/boot-v0600.png \
  generated/port/native/title.ppm
```

## 2. The one-entry offset

> **Resolved 2026-11-10:** MAME's snapshot lags its own video memory by one
> frame; the port is not a frame early. See `analysis/video-timing.md`
> section 2. The text below is kept as written at the time.

MAME's snapshot at the Nth VBlank handler entry equals the port's picture at
entry N−1 (verified for all 20; the evidence that it is exactly one comes
from the four changing pictures). Hypothesis: MAME's snapshot at that moment
is the bitmap of the frame before the one whose blanking has just begun, so
the offset is in how the reference is taken, not in the game.

Evidence for (verified): work RAM at entry 434 is closest to MAME's RAM at
its 434th entry, not at 433 or 435 (section 4), so the game itself is not
running a frame ahead. That RAM comes from the trace-window run, not the
picture run.

Not ruled out: that the port displays what the game draws one frame sooner
than hardware would. The picture check has the offset built in, so it could
hide a one-frame display-latency error. The test that would settle it is to
render MAME's own video memory at entry N with the port's renderer; MAME's
debugger does not expose that memory through `save`, so it needs a Lua
script reading the device's state.

## 3. What is modelled, and how it was checked

| Part | State | Check |
| --- | --- | --- |
| Video chip registers, control and data ports, VRAM, CRAM, VSRAM | modelled | unit tests; 20 pictures |
| DMA: 68000 to VDP, VRAM fill, VRAM copy | modelled | 68000 copies: unit tests and the pictures (boot does 732). Fill and copy: unit tests only, written to the behaviour documented for the hardware; boot does one fill and no copies, and the race window none of either, so nothing checks them against MAME |
| Renderer: planes A and B, scrolling, window, sprites, priority, 40-cell mode | modelled | unit tests; 20 pictures |
| Shadow/highlight, interlace, 32-cell mode, 30-row mode | not modelled | lines render magenta so they cannot pass unnoticed (unit test) |
| Colour levels | MAME's eight levels: 0, 52, 87, 116, 144, 172, 206, 255 | read from the snapshots |
| Frame and line interrupts | from the port's own cycle count: 262 lines of 488 cycles, frame interrupt at line 224 | pictures; RAM (section 4) |
| Status port, line counter | approximate | shadow check (section 5) |
| Three-button controllers, version register, Z80 bus request | modelled | shadow check: every recorded boot read agrees |
| Z80, FM chip, PSG | stubs by default: Z80 RAM accepts writes, sound chip writes are ignored. Since 2026-11-13 a sound side can be attached (`analysis/sound.md`); the PSG is still not modelled | see `analysis/sound.md` |

No model bug had to be fixed to get the pictures to match. The colour table
was the one correction: the first version used an even ramp, and MAME's
levels are uneven.

## 4. Work RAM against the boot trace window (verified)

The boot trace window ends at MAME's 434th VBlank handler entry. Native RAM at
its 434th entry differs from MAME's in 83 of 65,536 bytes (2,175 bytes at
entry 433 and 2,348 at 435):

| Range | Bytes | Cause |
| --- | ---: | --- |
| `0x2B64`–`0x2B85` | 34 | the tail of a buffer the foreground code is filling when the interrupt arrives; the port's interrupt lands a few dozen instructions later or earlier than MAME's |
| `0xBADA`–`0xBAFB` | 34 | the same, in a second buffer filled by the same loop |
| `0xFF37`–`0xFFAD` | 15 | the stack: registers saved by the interrupt, which hold the loop's progress |

All three follow from interrupt placement, which the spike showed cannot match
MAME exactly without cycle-accurate bus and video timing. No difference
suggests a logic error.

## 5. Shadow check: the model beside recorded hardware (verified)

`rr-trace-check --shadow-hw` runs the model alongside the replayed recording
and compares its answer to every read.

Boot window:

| Address | Reads | Differ | |
| --- | ---: | ---: | --- |
| `0xA10003`, `0xA10005` controller data | 808 each | 0 | |
| `0xA10001` version, `0xA10008`–`0xA1000C` | 5 | 0 | |
| `0xA11100` bus request | 810 | 0 | compared on bit 0; MAME returns open-bus leftovers in the other bits |
| `0xC00004` status | 2 | 1 | MAME has the frame-interrupt flag set and the model does not. In shadow mode the model only learns of vertical blanking when an interrupt is injected, and boot has interrupts masked at that point. The value read is discarded by the game |

Race window:

| Address | Reads | Differ | |
| --- | ---: | ---: | --- |
| `0xA11100` bus request | 230 | 0 | |
| `0xA10003` controller data | 260 | 164 | the recording has buttons held; the shadow model has no input |
| `0xC00008` line counter (word) | 43,599 | 43,595 | |
| `0xC00009` horizontal counter (byte) | 754 | 754 | |

The shadow model's picture is of no use for either window: the boot window
ends on a black screen, and the race window starts mid-race with the model's
video memory empty.

## 6. What this means for the race milestone

- **The line counter is the problem to solve** (verified that it is wrong,
  hypothesis about why). The race reads it about 335 times a frame and the
  model's value almost never equals MAME's. The model derives it from cycles
  since the frame started with an assumed phase and an even spread across the
  line; in shadow mode the frame start is itself inferred from when the
  recording took the frame interrupt. Next step: log MAME's `beamx`/`beamy`
  with a few hundred counter reads and fit the mapping, before deciding how
  much timing the race needs.
- **Line interrupts** fire about six times a frame in the race and are
  delivered here from a simple counter. They are untested: boot does not use
  them.
- **Raster effects.** Each line is drawn from the state at its start, and the
  line interrupt it raises is serviced afterwards, so a handler's changes show
  from the following line (unit-tested on a synthetic ROM, after an
  independent review found the first version applied them one line early).
  No real game code has exercised this yet.
- **Known inaccuracies to check before the race** (from the same review, not
  yet fixed): the frame-interrupt status flag is cleared by a status read
  where MAME clears it on interrupt acknowledge; sprite Y positions are
  masked to 10 bits where hardware uses 9 outside interlace; a register write
  does not update the address and code registers; the frame length drifts by
  the overshoot of each frame's last instruction; a line interrupt can be
  pending from line 0 of the very first frame.

## 7. Still stubbed or missing

Z80 and sound (no audio at all); six-button pads; controller input from a
real device; a window (output is a PPM file); shadow/highlight; interlace;
32-cell mode; the `stop` instruction and the other unimplemented instructions
listed in `port-spike-findings.md`; saving and restoring the hardware model
(`rr_vdp_save`/`rr_vdp_load` exist, the rest of `RrGenesis` does not).

## 8. Recommendation

Next, in the design's milestone order: controller input and a window, so the
title screen can be seen live and the front end can be driven. In parallel,
fit the line counter against MAME (section 6), because the first race frame
depends on it.
