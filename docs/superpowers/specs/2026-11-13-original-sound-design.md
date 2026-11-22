# The original sound: design

Date: 2026-11-13. Status: for the owner's approval.
"Owner" marks what the owner decided; "my call" marks choices made while
writing this that the owner has not discussed.

## Why

The port is silent. The owner wants the original sound brought in as it is
(owner: "just bring in the original sound"). The owner designs synthesisers
for a living, so "about right" will be heard as wrong.

## How the game makes sound (measured on the recorded race)

- The Genesis has a second processor, a Z80, that runs a small sound
  program (about 3.2 KB). The game loads it into the Z80's memory.
  **Verified.**
- The game's main code never writes to the FM chip (YM2612) itself: zero
  writes in the whole race. It rewrites a 48-byte block in the Z80's
  memory about once a frame (10,891 times) and five other bytes now and
  then (84 times). **Verified** (counts). What the block means (notes for
  that frame) is a **hypothesis**.
- The Z80 program writes the FM chip and reads sample data from the ROM
  through the Z80's "bank" window. **Verified** in the spike below (594,737
  FM writes in the race).
- The square-wave chip (PSG) got 4 writes in the race. **Verified**; that
  these only silence it is a **hypothesis**.

So the music logic is in the game code the port already runs; what is
missing is the Z80, the two chips and a way out to the speakers.

## The spike (throwaway, 2026-11-13)

A Z80 core and an FM chip model were attached to a scratch copy of the
replay tool. On the recorded race it wrote 214 seconds of sound
(`generated/sound/full-throttle-spike.wav`, ignored by Git) with signal
throughout and no clipping, and the race still ended `RESULT match`. Whether
it *sounds* right is for the owner's ears; nothing has been compared with
the original yet. The spike's code is not kept.

## What the owner will hear

- Running the game in the Godot window plays the game's music, engine and
  effects, in step with the picture.
- **M** mutes and unmutes (my call). It is listed on the Tab key list.
- At other game speeds (keys 2 to 4) the chips still run at their real
  speed, so pitch does not change. What happens to tempo depends on what
  the game's music code is timed by; it is measured and reported, not
  designed (my call).

## Approach (owner agreed)

1. **A Z80**, running the game's own sound program untouched. An existing
   open-source core is brought in: `superzazu/z80` (MIT licence, one C
   file). (Owner: bring in the cores.)
2. **The YM2612 FM chip**: Nuked-OPN2 (LGPL 2.1 or later), which models the
   real chip's circuit, including the DAC's "ladder" distortion. (Owner:
   bring in the cores.)
3. **The PSG**: written here, since it is simple (my call). Stage 3.
4. **Audio out through Godot**: the native library hands the window the
   samples made during each frame; the window plays them through Godot's
   audio output.

The two cores are kept as they were downloaded, in their own folder with
their licence files, and can be replaced without touching the port's own
code. They are not derived from the ROM and are committed.

## What must hold (requirements)

1. **The game is untouched.** With sound on, `rr-race-replay` still ends in
   `RESULT match`, and every existing measurement of the picture is
   unchanged. If the game ever reads something the sound processor wrote
   and it changes the game's course, that is found by this check and
   reported, not hidden.
2. **The same chip writes as the original.** The original, run in MAME on
   the same recording, is made to log every write its Z80 makes to the FM
   chip. The port logs the same. The two lists are compared: which
   register, which value, in which order. The aim is that they are
   identical; the result is reported as counts with the first difference,
   and any class of difference is explained or listed as unexplained.
   Differences in *when* a write happens are reported as a distribution
   (in samples). This is the measurement that lets the sound be called the
   same; listening is not.
3. **The owner's ears.** A recording of the same race from MAME and one
   from the port are written side by side under `generated/` for the owner
   to compare. MAME's FM chip model is a different one, so the two files
   are not expected to be identical sample for sample; that comparison is
   for listening only (my call).
4. **No crackle, little delay.** In the window the sound does not break up
   or drift away from the picture over a long session. The delay between
   the game making a sound and the speaker playing it is measured and
   reported; the aim is under 60 ms (my call). "Measured" means the
   samples waiting in the buffer plus the output delay Godot reports, not
   a microphone recording (my call). The owner's machine is the test.
5. **Clean stops.** Reset (F5), a halt, mute and closing the window leave
   no stuck note and no click louder than the music.
6. **Speed.** A frame with sound still fits well inside one video frame
   (16.6 ms) with the crisp picture on. The cost is measured and reported.
7. **Off is silent and free.** A tool that does not ask for sound (the
   tests, the replay tool by default) runs as today.

## What the comparison does not cover

Requirement 2 is measured on the one recording there is: boot, menus and a
full-throttle race with little fighting, no crash and no police stop.
Effects that only happen there will play in the window but are
**unverified** against the original until the owner's further recordings
exist. Those recordings are not a condition for finishing.

## Timing (left to the builder, within these limits)

The Z80 and the 68000 run from the same clock on the real machine. The
port runs the 68000 first and lets the Z80 catch up whenever the two
touch: when the game writes the Z80's memory, takes or gives back its bus,
resets it, and at least once a frame. While the game holds the Z80's bus
or its reset line the Z80 stands still, as on the real machine. The Z80's
once-a-frame interrupt arrives at the start of the vertical blank.
Requirement 2 is the judge of whether this is fine enough.

## Stages

1. **Sound in the replay tool.** The cores, the Z80's memory map, a WAV
   option, the write log, the MAME capture script and the comparison
   (requirements 1, 2, 3, 7). Ends with numbers and two files to listen to.
   Stage 2 waits for the owner's verdict on them (my call): "sounds wrong"
   sends the work back into timing before any window work.
2. **Sound in the window.** Samples out of the library, playback in Godot,
   mute key, the clean stops, delay and speed measured (requirements 4, 5,
   6). Needs a rebuild of the library, so the owner closes the window
   first.
3. **Finishing** (decided after listening): the PSG; whether to add the
   console's own gentle output filter, which softens the top end on a real
   Genesis (the owner chooses by ear; my call to leave it out of stages 1
   and 2 and say so).

## Not part of this (non-goals)

- New or improved sound, remixing, stereo widening, higher sample quality.
  That belongs to the later game and is the owner's own field.
- Sound for the in-between pictures: sound follows the game, not the
  smoothed picture.
- Recording sound to a file from the window.

## What would make this a failure (anti-goals)

- The race no longer replays identically.
- Sound that is "close": wrong pitch, wrong envelope, missing samples,
  notes late enough to hear, with nothing measured to say so.
- Crackle or growing delay in a normal session.

## Constraints carried over

- Nothing derived from the ROM is committed: recordings, write logs and
  Z80 memory images stay under `generated/`. Test data is synthetic (small
  hand-written Z80 programs and register writes).
- Every claim in `analysis/` is marked verified or hypothesis.
- The library is not rebuilt while a Godot window may be using it.

## Left to the builder

- Where the sound code lives and how samples are buffered and handed over.
- How the chip's own sample rate (about 53,267 a second) is matched to the
  sound card's, and how small timing drift is absorbed without changing
  pitch audibly.
- The MAME logging script, in the style of the existing capture scripts.
- The output level (the spike peaked at about half of full scale).
