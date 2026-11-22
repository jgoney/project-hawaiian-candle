# Project: Hawaiian Candle

A native port of the Sega Genesis game *Road Rash II* (1992): the game's own
68000 code translated to C and run without an emulator, measured against the
original, with an optional crisp, smooth picture on top.

**No game is included.** There is no ROM, no translated game code and no
game art in this repository. You build the port from your own ROM.

**Unofficial.** This is a fan's research project. It is not made, endorsed
or supported by Electronic Arts or Sega; *Road Rash* is Electronic Arts'
trademark and the game is its work.

## What it does

- **Plays like the original, measured.** Six recorded sessions (several
  tracks, one ending in an arrest) were replayed by the port and compared
  with the original running in MAME 0.289: all 64 KB of the game's memory is
  identical at the start of every pass of the game loop, 14,968 passes in
  all (`analysis/race-conformance.md`).
- **The original sound.** The game's own Z80 sound program runs beside the
  game and plays a model of the FM chip. Every music and effect write to
  the chip in those six sessions is the original's, in the same order; the
  sampled sounds differ by a few single bytes per session, unexplained
  (`analysis/sound.md`).
- **A crisp picture (R).** The road's edges are drawn at the window's own
  resolution from the game's own road points, and riders, cars and
  roadside objects from their full-size pictures, in place of the 320 x 224
  original enlarged (`analysis/race-picture.md`).
- **In-between pictures (I).** The game works the race out about 11 times a
  second; the window shows the scene part-way between updates on every
  frame, while the game itself runs untouched.
- **A wide view (W).** The race 398 pixels across instead of 320: the game
  runs untouched and the port draws the extra at the sides from what the
  game holds (`analysis/widescreen.md`).
- **A CRT screen (V)**, a faster game loop (1 to 4), gamepads, and a list of
  every key on Tab.
- **Capture and replay (P).** One key saves the picture, the game's memory
  and everything done since start-up; `rr-session-replay` brings the port
  back to that exact frame. This is how picture faults are found and fixed.

## What it is not, yet

- **Apple Silicon Macs only.** The build script and the native library are
  macOS-only today.
- **Not a packaged app.** It runs from this repository
  (`docs/building.md` says what an app would need).
- **Only the one-player race is drawn crisply.** Menus, the two-player race
  and the dashboard are the original picture enlarged.
- **A fresh copy complains about a missing editor plugin.** `godot/project.godot`
  names a Godot plugin that is not in the repository
  (`godot/addons/README.md`); Godot reports it as missing and carries on.
- **The end of each recorded session is not compared** (the results and
  arrest screens after the last pass of the race loop), and two-player
  races have not been recorded.
- **Known picture faults:** riders in the mirrors and the far end of the
  road are not smoothed between updates, and at 400% game speed and above
  a one-line stripe can cross the picture.
- The square-wave sound chip is not modelled. In the six sessions the game
  never plays anything on it.

## You need

- A Mac with Apple Silicon, and Apple's command-line tools
  (`xcode-select --install`).
- [`uv`](https://docs.astral.sh/uv/) (it fetches Python itself).
- Godot 4.7 at `/Applications/Godot.app` (or set `GODOT_BIN`).
- Your own ROM of *Road Rash II (USA, Europe)*, revision RR206, as
  `Road Rash II (USA, Europe) (RR206).gen` in the repository's top folder.
  The build checks its SHA-256 and refuses any other file.

## Build and play

```sh
scripts/build-godot-extension.sh   # translate your ROM to C and build the library
scripts/run-godot-console.sh       # play
```

| | Player 1 | Player 2 |
|---|---|---|
| Directions | arrow keys, d-pad or left stick | I J K L, gamepad 2 |
| A, B, C | Z, X, C; gamepad X, A, B | T, Y, U; gamepad X, A, B |
| Start | Enter; gamepad Start | Backspace; gamepad Start |

Tab lists the other keys and what each is set to. `godot/README.md` explains
them.

## Checking it

```sh
make -C port test                  # the port's C tests
uv run --group dev pytest          # the tools and the replay tests
uv run --group dev ruff check .
```

To measure a recording of your own against the original (needs MAME):
`analysis/race-conformance.md`, "What can be claimed", has the three
commands.

## How it is put together

| Folder | What is in it |
|---|---|
| `src/rr2port/` | The translator: finds the game's code in the ROM and writes it out as C (`analysis/code-discovery.md`) |
| `port/core`, `port/hw` | The C runtime and the model of the Genesis: video chip, timing, controllers, sound |
| `port/render` | The crisp road, full-size objects and in-between pictures |
| `port/host`, `godot/` | The console as a library, and the Godot window that shows it |
| `port/third_party/` | Two sound models by other authors, unchanged, with their licences (`port/third_party/README.md`) |
| `mame/`, `scripts/` | Scripts that record the original in MAME for comparison |
| `analysis/` | What was measured. Every claim is marked verified or hypothesis |
| `docs/` | Build notes, a wishlist, and the design notes and plans the work followed |

Everything derived from the ROM (the translated code, recordings, pictures,
sound) is written under `generated/`, which Git ignores.

## Research tools

Inspect a ROM:

```sh
uv run rr2-rom-info "Road Rash II (USA, Europe) (RR206).gen"
```

Dump RR206 bike, rider, police, and dialogue text as Markdown:

```sh
uv run rr2-text-dump "Road Rash II (USA, Europe) (RR206).gen"
```

Inspect the per-bike gear and motion tables:

```sh
uv run rr2-bike-tuning "Road Rash II (USA, Europe) (RR206).gen"
```

Run the checks:

```sh
uv run --group dev pytest
uv run --group dev ruff check .
```

See `ghidra/README.md` for the reproducible raw-binary import workflow.
See `mame/README.md` for interactive runtime debugging and physics logging.

## How the port came about

The project is now a native port of the original game code. See
`docs/superpowers/specs/2026-11-10-native-port-design.md` for the design and
`docs/superpowers/plans/2026-11-10-port-spike.md` for the first implementation
plan (the translation proof).

How the translator finds the game's code is described in
`analysis/code-discovery.md`; `uv run rr2-code-report` shows what is found and
which indirect jumps are resolved.

The translated game boots natively to its title screen:
`analysis/first-picture-findings.md` describes the hardware model and how its
pictures are checked against MAME.

To run the game natively in a window:

```sh
scripts/build-godot-extension.sh
scripts/run-godot-console.sh
```

See `godot/README.md` for controls and `analysis/video-timing.md` for what
has and has not been checked.

A whole recorded race has been replayed natively and compared with the
original pass by pass: same frames per pass and byte-identical work RAM at
every one of 2,176 passes. `analysis/race-conformance.md` has the numbers,
what they do and do not show, and how to measure another recording.

`analysis/smoothness-experiment.md` reports what happens when the game's
processor is run faster (keys 1 to 4 in the window): more updates a second,
up to 30, but the bike no longer performs quite the same.

`analysis/race-picture.md` explains how the race picture is made, from the
track's curve and hill numbers to what the video chip is sent, and
`docs/superpowers/specs/2026-11-11-draw-data-contract.md` sets out what a
high-resolution renderer is handed each pass. The first step of that
renderer is in: press R in the window for the crisp road (the picture
several times as large, with the road's edges drawn at that size, and the
riders, cars and roadside objects drawn from their full pictures at that
size; F and O switch the two refinements off and on to compare). On top of
that the window draws in-between pictures, so the scene moves on every frame
while the game itself runs unchanged (I steps through blend, predict, late
blend and off; `rr-race-replay --inbetween MODE --inbetween-check` measures
them on the recorded race; `analysis/race-picture.md`, section 8). The
centre-line dashes are drawn from the same road and slide with it (section
9), and B softens the far objects. The object
pictures can be written out with `rr2-extract-art` (into the ignored
`generated/` directory).

The original sound plays in the window (M mutes): the game's own Z80 sound
program playing a model of the FM chip. In the replay tool
(`rr-race-replay --sound-wav FILE`) every one of the recorded race's 69,265
music and effect writes to the chip is the same as the original's, in the
same order; `analysis/sound.md` has the numbers and what is still open.

## Licence

The project's own code and write-ups are under the MIT licence (`LICENSE`).

`port/third_party/` holds two pieces by other authors under their own
licences, unchanged: a Z80 processor (MIT) and the Nuked-OPN2 FM chip model
(LGPL 2.1 or later). A library you build contains both, so passing a built
copy on means following the LGPL for that part: say it is there, and let
whoever receives it replace it. `port/third_party/README.md` has the
sources and versions.

The licence covers this repository only. It gives no rights to the game, and
anything you make from your own ROM (the translated code, pictures, sound,
recordings) is not yours to pass on.
