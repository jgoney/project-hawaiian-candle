# The original sound

Written 2026-11-13, stages 1 and 2 of
`docs/superpowers/specs/2026-11-13-original-sound-design.md`: the sound runs
in the replay tool, where it has been compared with the original (sections
1 to 6), and plays in the Godot window (section 7). Every claim is marked **verified**
(measured, with the measurement named) or **hypothesis**.

All measurements are on the one recording there is, `full-throttle`: boot,
menus and a full-throttle race, 214 seconds, with little fighting, no crash
and no police stop.

## 1. How the game makes sound

"SOUND line" below means the line `rr-race-replay --sound-wav … --fm-log …`
prints: `SOUND z80_steps=86838703 fm_writes=594781 fm_writes_68000=0
psg_writes_z80=0 psg_writes_68000=4 z80_ram_writes_68000=542273
samples=11416509`.

- The Genesis has a second processor, a Z80. The game loads a sound program
  into the Z80's memory and starts it; the 68000 wrote that memory 542,273
  times in the run. **Verified** (SOUND line; the Z80 ran 86.8 million
  instructions).
- The program is about 3.2 KB (addresses 0x0000 to 0x0C99). The 68000
  rewrites a 48-byte block at 0x0256 to 0x0285 about once a frame (10,891
  times each) and five bytes at 0x00BB to 0x00BF now and then (84 times
  each). **Measured once** on 2026-11-13 with throwaway instrumentation that
  is not in the repository, so it cannot be re-run from here. That the
  block holds that frame's notes and the five bytes start a sampled effect
  is a **hypothesis**; the Z80 program has not been read.
- The game's main (68000) code never writes the FM chip (YM2612) itself.
  **Verified**: 0 of the port's 594,781 FM writes came from the 68000 (SOUND
  line), and 0 in MAME's log, where each write is tagged with the processor
  that made it (`rr2-fm-compare` prints `FROM_68000 port=0 mame=0`).
- The Z80 program does all the writing to the FM chip: 594,781 writes in the
  run (297,365 register writes, each an address and a value). 228,100 of
  them are to register 2A, the chip's sample player, at about 9,300 a
  second while a sample plays. **Verified** (`rr2-fm-compare`; rate from
  the times in the log). The samples are read from the ROM through the
  Z80's bank window: that is how the model serves them, and the sample
  bytes agree with the original's (section 3). Which sounds they are
  (engine, hits, voices) is for the owner's ears.
- The square-wave chip (PSG): the 68000 wrote it 4 times in the run, the
  Z80 never (addresses 0x7F10 to 0x7F17). **Verified** (SOUND line). That
  those 4 writes only silence it is a **hypothesis**; the PSG is not
  modelled, so if they do make a sound it is missing.

## 2. The model

`port/hw/rr_sound.[ch]` holds a Z80, the FM chip, the Z80's memory map and
the samples that come out. The Z80 and the FM chip are other people's
models, kept unchanged (`port/third_party/README.md`): a small Z80 core and
Nuked-OPN2, which models the real chip's circuit.

- The console model (`rr_genesis.c`) has an optional sound side. With none
  attached (the default, and every test and tool that does not ask for it)
  nothing changes.
- The sound side runs on hardware time. The 68000 runs first; the Z80 and
  the chip catch up whenever the 68000 touches the Z80's memory, its bus or
  its reset line, and at the end of every frame.
- While the 68000 holds the Z80's bus or its reset, the Z80 stands still.
  The Z80's interrupt comes once a frame at the start of the vertical blank
  and is withdrawn one line later if not taken.
- Because it runs on hardware time, running the 68000 faster (keys 2 to 4)
  does not change the pitch or the amount of sound. **Verified** by a unit
  test (sample count at 200%). The tempo of the music is in section 7.

## 3. Is it the same? The measurement

The original was run in MAME on the same recording with a script that logs
every write to the FM chip (`scripts/run-mame-fm-log-rr206.sh`). The port
logs the same (`rr-race-replay --fm-log`). `rr2-fm-compare` compares the two
lists. Results, **verified** on 2026-11-13: the first row by
`rr-race-replay` with the sound options on, the rest by `rr2-fm-compare`:

| What | Port | Original (MAME) | Result |
|---|---|---|---|
| The game with sound attached | | | still `RESULT match`: all 2,176 passes identical |
| Music and effect writes (every register but 2A) | 69,265 | 69,265 | **identical: same register, same value, same order, all 69,265** |
| When those writes happen | | | median 0.2 samples early; 98% within 4.3 samples (0.08 ms); worst 11.8 samples (0.22 ms) |
| Sample-player writes (register 2A) | 228,100 | 228,097 | 228,093 the same in order; **10 slips** of one byte each inside the run (the port has one byte more 6 times, one fewer 4 times), plus one byte at the very end of the run |
| All writes as one list, in order | 297,365 | 297,362 | the same for the first 9,998, then they interleave differently |

What this does and does not show:

- The music and effects the game asks the chip for are the same as the
  original's, write for write, for this recording.
- The last row is expected and is not a fault by itself: a sample byte and a
  music write that fall within a fraction of a sample of each other can
  come out in either order. That is why the two streams are compared
  separately.
- **The 10 slips are unexplained.** They come where a sampled sound starts
  or stops (at 52.9, 53.2, 54.5, 54.7, 109.4, 144.1, 144.2, 212.6, 213.4 and
  214.0 seconds): one byte of about 9,300 a second is played once more or
  once less. **Hypothesis**: the Z80 notices a new command from the 68000
  one turn of its loop sooner or later than in MAME, because the two models
  do not step the two processors in exactly the same slices. MAME is itself
  a model here, not the hardware, so neither side can be called the right
  one from this measurement. A slip is 0.1 ms of a sample.
- The comparison is of what is *written to* the chip. What the chip then
  *plays* is Nuked-OPN2's model in the port and a different model in MAME,
  so the two recordings are not the same sample for sample and were not
  compared that way.
- The 13,160 register writes MAME made after the port's run ended (the
  recording goes on a little after the last compared pass) are set aside.
  The port's log ends with the time its run ended, and only what MAME wrote
  after that time is set aside, so a port whose sound stopped early would
  not pass (checked: a port log cut to its first 1,000 register writes
  gives `RESULT different`).
- `rr2-fm-compare`'s own verdict for this run is `RESULT music-same`, not
  `RESULT same`: the music stream is identical and the sample stream lines
  up apart from the slips.

## 4. For listening

Both under `generated/sound/` (not in the repository):

- `full-throttle.port.wav`: the port, 214 seconds, at the chip's own 53,267
  samples a second.
- `full-throttle.mame.wav`: MAME on the same recording, at 48,000. It runs
  longer and is quieter (peak 6,404 against the port's 16,200 of 32,767),
  so match the levels before comparing.

To make them again:

```sh
generated/port/build/rr-race-replay --rom "Road Rash II (USA, Europe) (RR206).gen" \
  --handoffs generated/port/handoffs/full-throttle.bin \
  --timesteps generated/port/handoffs/full-throttle.timesteps \
  --pads generated/port/handoffs/full-throttle.pads \
  --sound-wav generated/sound/full-throttle.port.wav \
  --fm-log generated/sound/full-throttle.port-fm.log
scripts/run-mame-fm-log-rr206.sh full-throttle
uv run rr2-fm-compare generated/sound/full-throttle.port-fm.log \
  generated/sound/full-throttle.mame-fm.log
```

## 5. Cost

The replay of the race took 18.7 s of processor time without sound and
29.9 s with it: 11.2 s for 214 s of sound, about 0.9 ms a frame.
**Verified** (`time`, one machine, Apple M1 Max).

## 6. Not done, not covered

- In the window: the sound card's own delay is not known, and clicks at
  reset or mute have not been listened for (section 7).
- The PSG is not modelled (4 writes in this race).
- No output filter: a real console softens the top end a little; the port's
  recording is the chip's raw output.
- The output level is fixed (peak 16,200 of 32,767 in this run); six
  channels at full level together could clip.
- Crash, police and heavy fighting sounds are not in the recording, so they
  are **unverified** against the original.
- The 10 sample slips above.

## 7. In the Godot window (stage 2)

How it gets to the speakers:

- The console library can hand out the sound each frame made
  (`rr_console_take_sound`), converted from the chip's 53,267 samples a
  second to the sound card's rate by a band-limited filter
  (`port/host/rr_resample.c`: 24 input samples a side, flat to about 18 kHz
  at a 48,000 card rate). **Verified** by unit tests: a 1 kHz tone keeps its
  level within 3% and its pitch; a 15 kHz tone is kept; a 25 kHz tone, which
  a 48,000 card cannot carry, comes out more than 30 dB down.
- The window pushes it into Godot's audio output and asks for very slightly
  more or less than the card's rate (0.5% at most, which is about a twelfth
  of a semitone) to keep a steady amount waiting. This is needed because
  the console's frame is 59.92 a second and the window runs 60.
- **M** mutes and unmutes. Reset and a halt drop what is waiting.

Measured on the owner's machine (Apple M1 Max, card rate 44,100), a
60-second windowed run from switch-on through the title music, sampled
every frame after the first second. **Verified** by a throwaway probe
script on 2026-11-13 (not in the repository):

| What | Result |
|---|---|
| Sound waiting to be played | median 50.8 ms; 90% of frames between 43.1 and 59.5 ms; least 34.8, most 73.9 |
| Frames on which it ran empty (a dropout) | 0 of 3,540 |
| Rate asked of the console | median 44,038.8 (0.14% under the card's); never more than 0.5% off |
| Cost | 9.08 ms a frame with sound against 8.54 without, for everything the window does in a frame: about 0.5 ms (one 40-second run each) |

What this does not show:

- **The whole delay is not known.** Godot reports the sound card's own
  output delay as 0 ms on this machine, which cannot be right, so only the
  50 ms waiting in the buffer is measured. The design's aim was under 60 ms
  measured as "buffer plus what Godot reports"; by that rule it is met
  (50.8 ms), but the true figure is higher by whatever the card adds.
- No dropouts in one minute is not "never". A long session, a busy
  machine and the crisp picture during a race were not measured.
- Whether reset or mute make a click louder than the music was not
  listened for. Mute turns the volume down; reset restarts the output.
  **Unverified**, for the owner's ears.

Tempo at other game speeds: the replay tool was run at 200% and its log
compared with the 100% log over the title music (seconds 20 to 40 of
hardware time, where both runs are in it): 145 and 141 notes started per
ten seconds at 100% against 144 and 146 at 200%. **Verified** for that
music: the tempo does not follow the game speed. **Hypothesis**: the music
is timed by the frame, not by how fast the game's code runs. Music during a
race at 200% was not measured.

## 8. Five more recordings (2026-11-14, verified)

Same method as section 3 (`rr-race-replay --fm-log`, `run-mame-fm-log-rr206.sh`,
`rr2-fm-compare`), on the owner's five new recordings. Every one ends
`RESULT music-same`.

| Recording | Music writes (port = original, same order) | Music timing, samples: median / p1 / p99 / min / max | Sample writes port / original | Slips | 68000 FM writes | PSG writes (68000 / Z80) |
|---|---|---|---|---|---|---|
| `arrested` | 46,050 | −0.2 / −4.1 / 3.8 / −5.5 / 8.1 | 96,254 / 96,252 | 4 | 0 | 4 / 0 |
| `vermont` | 50,082 | −0.3 / −4.2 / 3.8 / −6.8 / 5.9 | 214,949 / 214,946 | 13 | 0 | 4 / 0 |
| `arizona` | 64,803 | −0.2 / −6.4 / 6.0 / −8.7 / 8.6 | 170,231 / 170,230 | 3 | 0 | 4 / 0 |
| `tennessee` | 81,918 | −0.3 / −4.9 / 4.8 / −8.3 / 10.3 | 218,921 / 218,921 | 4 | 0 | 4 / 0 |
| `hawaii` | 90,185 | −0.5 / −5.0 / 5.0 / −13.4 / 14.2 | 508,918 / 508,914 | 10 | 0 | 4 / 0 |

- Every slip is one sample byte, as in section 3. Still unexplained
  (**hypothesis**: how finely the two processors are interleaved).
- The square-wave chip gets the same 4 writes from the 68000 and none from
  the Z80 in every recording (counted in the port, whose memory matches the
  original's at every pass). So in these six recordings the game does not
  play anything on it. That the 4 writes only silence it remains a
  **hypothesis**.
- The original's writes after the port's run ended (23,248 to 121,223 per
  recording) are **not compared**; see the limit in `race-conformance.md`.
