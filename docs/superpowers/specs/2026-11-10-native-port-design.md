# Road Rash II native port: design

Date: 2026-11-10. Status: approved by the owner 2026-11-10; revised the same day
after the translation proof (`analysis/port-spike-findings.md`). Revisions are
marked **(spike)**.

This replaces the earlier direction (original code running in an emulator with a
new renderer on top) and the clean-room rebuild described in
`analysis/godot-plan.md`.

## Intent

Build a standalone native game that plays exactly like Road Rash II (Genesis,
RR206, SHA-256 `c7a60901…7848`) and looks and moves better. Once the baseline
feels right, the same engine is the base for changes: new levels, optional
smarter AI, new weapons and play modes, online multiplayer, and later a 3D
presentation.

Why a port and not an emulator wrapper: the owner wants a native program whose
code can be read and changed. Why a translation and not a rebuild from observed
behaviour: fidelity of feel is the point, and a routine-by-routine translation
of the original code keeps behaviour by construction.

Items marked **(owner)** were decided by the project owner. Items marked
**(my call)** are the designer's defaults and are open to challenge.

## Goals

1. **(owner)** A standalone native executable. No emulator of the game's CPU in
   the shipped product. "Standalone" means no emulator program is involved; the
   game still needs the user's ROM file, both to build and at run time for
   graphics, sound and track data (goal 6).
2. **(owner)** Plays the same as the original: physics, AI riders, combat, cops,
   crashes, tracks, progression.
3. **(owner)** Same visual style as the original, but crisp and smooth: flat
   sprite riders and scrolling road at high resolution and display frame rate.
4. **(owner)** An engine that can be extended afterwards.
5. **(owner)** Later stage: a real 3D world drawn from the same game logic,
   using free or royalty-free 3D assets adapted to match the 2D look.
6. **(my call)** The game loads graphics, sound and track data from the user's
   own ROM file at run time. Nothing derived from the ROM is committed or
   distributed. This keeps a later public release of the engine code possible.

## Non-goals (for this design)

- The 3D renderer, new content, smarter AI, new modes and multiplayer. They
  shape the architecture below but are not built in these stages.
- Rewriting in Go or Rust. Reasonable later, once the C is readable.
- Supporting RR205 or other regional builds.
- Platforms other than macOS **(my call)**. Code stays portable C with no
  macOS-only dependencies in the core, but only macOS is built and tested.
- Widescreen or changed field of view **(my call)**. Baseline keeps the original
  framing.

## Anti-goals

- A game that looks right but feels slightly off. Any stage that cannot show
  conformance against recorded original behaviour is not finished.
- A long dark period with nothing playable. Every stage ends with a runnable
  game.
- Committing ROM bytes, extracted art, or generated C that embeds ROM data.

## What lives in git, and what a build needs (my call)

- **Committed:** the translator, the annotation file, the hardware shim, hosts,
  tests, and all hand-written C from Stage 5. Hand-written code is new
  expression of documented behaviour and is treated like any decompilation
  project's source; it still carries the legal risk noted under Constraints.
- **Ignored:** generated C, the ROM, extracted and upscaled art, captures.
- **Build inputs:** a clean checkout plus the user's RR206 ROM. The build runs
  the translator on the ROM, then compiles. Ghidra is a research tool only and
  is not a build dependency; the annotation file alone must carry the complete
  code map (entry points, code/data ranges, jump tables).
- **Release shape, if it ever happens:** source only. Users build against their
  own ROM. No prebuilt executable, since it would embed translated ROM code.

## Current state

- Reproducible Ghidra import with annotations (`ghidra/`, `scripts/`).
- Python package `rr2rom` (ROM header, bike tables, RAM capture reader) with tests.
- MAME scripts: per-frame RAM diff capture, deterministic input replay, physics
  and timestep loggers (`mame/`). Captures `full-throttle` and
  `wild-thing-controls` exist locally under `generated/mame/`.
- Understood in depth: bike tuning tables, acceleration, braking, nitro, speed
  integrator (`0x175C4`), timestep scheduler (`0xDF3E`, `0xE038`). See
  `analysis/bike-physics.md`.
- Not yet understood: track format, road projection, sprite/draw lists, AI,
  combat, cops, front end, graphics compression, the Z80 sound driver.
- Godot 4.7.1 project with a physics lab and gray-box slice (`godot/`).
- Nothing is committed to git yet.

## Key facts the design depends on

- ROM is 1 MiB of 68000 code and data; work RAM is 64 KiB at `0xFF0000`.
- The race loop is CPU-bound. A VBlank callback counts frames into `0xFF1C06`;
  each loop pass takes that count as its timestep (`0xFF1C08`, typically 4–7,
  mean 5.43, about 11 Hz) and scales motion by it at 49 read sites.
- The VBlank callback also extrapolates road/camera position every frame
  (hypothesis, `0x16CE`/`0x179E`).
- So the original's update rate is a property of 68000 speed, not a constant.
  A native build must model that timing to reproduce a recording, and can drop
  it to run smoothly.
- Control flow includes indirect jumps: the callback slot at `0xFF0300` and
  jump tables such as `0x52F8` and `0xCC274`.

## Architecture

Three layers with hard boundaries:

```
  host (window, input, audio out, renderer)      Godot, later 3D
  ------------------------------------------------------------
  core library: game logic + 64 KiB RAM image     portable C, deterministic
  ------------------------------------------------------------
  hardware shim: video chip, sound chips, pads    shrinks over time
```

- **Core library.** Translated game code operating on a byte-exact copy of the
  original work RAM. Deterministic: same ROM, same inputs, same timing mode
  give the same RAM every step. No global host state, no wall-clock reads, no
  threads inside. Exposes: load ROM, reset, set inputs, advance, read RAM, read
  the frame/draw data, pull audio samples, save/restore full state. Save/restore
  and determinism are required now because conformance testing uses them and
  multiplayer will later.
- **Complete state.** Everything that determines future behaviour, and so
  everything save/restore must cover: the 68000 registers, condition codes and
  resume point; work RAM (which holds the original stack, top at `0xFFFFF6`);
  interrupt and timing counters; the video chip's registers and memories; the
  Z80, its RAM and the sound chips; controller port latches. Nothing that
  matters may live only on the native C call stack, because that cannot be
  saved. The memory map the translated code sees is the one in
  `analysis/memory-map.md`, big-endian, with every hardware-range access routed
  to the shim at the point in execution where the original made it.
- **Hardware shim.** Receives the game's writes to the video chip, sound chips
  and controller ports. Stage 1 uses a software model of the Genesis video chip
  producing the original 320×224 picture, and existing open-source models of
  the Z80 and the FM/PSG sound chips running the original sound driver
  unchanged. **(owner)** An emulated sound processor is accepted for the
  baseline. Any third-party chip code must carry a permissive licence
  (BSD/MIT/zlib class) so a release stays possible **(my call)**. The sound
  path sits behind its own interface (game sends sound commands, host pulls
  samples) so it can be replaced without touching game logic; see "Sound
  port" under Later.
- **Hosts.** A headless runner for tests, and Godot as the playable host via a
  native extension **(my call)**. Whether to also keep a minimal windowed test
  host outside Godot is left to the builder.

## Stages

Each stage ends playable and conformance-checked.

### Stage 0: housekeeping

First git commit of the existing work. Mark `analysis/godot-plan.md` as
superseded by this document.

### Stage 1: mechanical translation

A translator tool turns the ROM's 68000 code into C, exact in registers, flags
and memory effects. **(spike)** Each 68000 instruction becomes one small C
function, reached through a table indexed by address, with all state in the
machine structure. That is what lets execution stop, save and resume between
any two instructions. Original routines show up as the layout of the generated
file, not as C functions that call each other. Readability is
not a goal at this stage. Language for the generated code and core is C
**(my call)**; the translator itself can be Python alongside `rr2rom`.

**Proof first (done).** The proof described here was carried out and passed;
see `analysis/port-spike-findings.md`. Translated code reproduced MAME
instruction for instruction over 5.4 million instructions of boot and 1.5
million of a race, with pause and save/restore at any instruction. The
original brief is kept below for the record.

Before translating the whole game, build a small translator
that handles the boot path and a handful of routines covering arithmetic
flags, branches, subroutine calls, memory access and one interrupt, and show
it reproduces a short MAME execution trace. This spike must settle three
things, and its findings are reviewed before the rest of Stage 1 proceeds:

1. How execution is suspended and resumed. The game's main loop never returns
   and waits on interrupts, yet the host must get control back every frame and
   state must be saveable. The original stack has to live in work RAM anyway,
   since return addresses are part of the RAM being compared.
2. How interrupts are delivered mid-routine and how cycle costs are charged.
3. Whether reference timing can place interrupts exactly, or whether the
   injected-timestep fallback in Stage 2 becomes the standard check.

**Milestones.** **(spike)** In this order, each ending with a runnable build
and its own check:

1. Code discovery. Finding all the code turned out to be the hard part, not
   translating it: following control flow from the known entry points missed
   375 of 7,422 executed addresses, all behind indirect jumps and calls. The
   translator must find them from annotations alone, without looking at what
   an emulator ran.
2. A video model, to the point where boot draws its picture natively.
3. Controller input and a window: boot to title, then front end and bike shop.
4. One complete race. This needs video timing as well as video output: the
   race code reads the video chip's line counter about 335 times a frame.
5. Sound, on the emulated sound processor.
6. Traffic, combat, crashes and cops; remaining tracks, progression and
   ending; two-player mode.

Requirements:

- Code/data separation comes from the Ghidra project plus an annotation file in
  the repo. Indirect jumps and callbacks dispatch through a table of known
  entry points; hitting an unknown target must fail loudly with the address,
  never continue silently.
- Interrupts: the VBlank and HBlank handlers must be delivered to the translated
  code at the points the original timing would deliver them.
- Two timing modes:
  - **Reference timing** charges original 68000 cycle costs and places frame
    and line interrupts from the build's own cycle count. **(spike)** It is
    not required to match MAME's interrupt placement exactly: that needs
    cycle-accurate video and bus timing. It is validated statistically, by
    reproducing the recorded timestep distribution.
  - **Free timing** ignores cycle costs. Used from Stage 3 on.
- Routines can be swapped individually between the translated version and a
  hand-written one at build time. This is needed from the start because the
  mechanical translation is kept permanently as the reference (see Stage 5).
- Coverage is driven by play: boot, front end, bike shop, each of the five
  tracks at the first and last level, crash, bust, win, lose, the ending, the
  attract demo, and two-player mode **(my call on scope)**. A coverage report
  lists routines never reached. Routines still unreached are translated anyway
  and keep the loud failure on unknown indirect targets.
- **(owner)** Captures are a mix: the owner records real play sessions in
  MAME, and passwords, MAME save states and scripted inputs fill in wherever
  they reach the code more cheaply. The builder keeps a list of situations
  still uncovered and asks the owner for specific sessions.

Picture is checked automatically by comparing selected frames, pixel for
pixel, against MAME screenshots from the same replay. Sound is checked by ear
**(my call)**.

Done when: the native build boots from the ROM file, the whole game is playable
with original picture and sound, and conformance (below) passes.

### Stage 2: conformance harness

The existing MAME captures become the acceptance test. **(spike)** There are
two checks, which answer different questions:

- **Trace-driven conformance** checks the CPU translation. MAME records every
  instruction and hardware access for a short window; the build runs the same
  window with interrupts injected where MAME took them and hardware reads
  replayed, and must match every register before every instruction. It is
  exact and needs no hardware model, but a window is hundreds of megabytes, so
  it covers stretches, not whole races. Any newly translated code gets a
  window that exercises it.
- **Whole-capture conformance** checks the assembled game, on the build's own
  hardware models, as described next.

For whole captures, the headless runner replays a recorded input file and
compares work RAM against the capture.

- Target: byte-identical RAM and identical 68000 registers at every loop-pass
  handoff (`0xDF48`) for the whole of `full-throttle` and
  `wild-thing-controls`, plus new captures that cover traffic, combat, cops,
  crashes and each track. The MAME loggers are extended to record registers at
  the handoff. Video memory is compared at frame ends where MAME exposes it.
  Sound processor state is not compared.
- **(spike)** Exact interrupt placement did prove impractical, so the method
  is: inject the recorded timestep sequence and inputs per loop pass and
  require identical RAM at each handoff. The builder chooses, and documents any RAM
  ranges excluded and why. Unexplained differences are failures.
- Known hazard: anything the game derives from hardware timing (for example
  random seeds from the video counter) must be found and modelled.

Stages 1 and 2 are developed together; the harness is what makes Stage 1
debuggable.

### Stage 3: smooth motion

Two ways to get smooth motion, built in this order. The owner compares them by
feel and picks the default; both stay available **(my call)**.

**3a. Smooth picture, original logic.** Game logic keeps its original cadence
(about 11 updates a second, exactly as conformance-checked) and the picture is
drawn every display frame by interpolating positions between updates. The
original already does a form of this for the road (hypothesis, `0x16CE`).
Gameplay is unchanged by construction, including how quickly the bike answers
the controls. Fully effective only once the Stage 4 renderer exists; before
that it applies to whatever positions can be interpolated in the original
picture.

**3b. Faster logic.** Switch to free timing so the loop runs once per display
frame with a timestep of 1, instead of about every 5.4 frames. This directly
tests the open question from the research (does more frequent updating keep
race speed the same). Controls respond faster than the original, which is a
change in feel, for better or worse.

For 3b:

- Audit all logic that does not scale with the timestep (counters, AI decision
  cadence, animation steps, collision checks per pass). Each such site is
  either left on the original cadence or scaled, with the choice recorded.
- Acceptance **(my call)**, in three parts, because recorded inputs stop being
  valid once the two runs drift apart:
  1. On collision-free replays such as `full-throttle`, player distance, speed
     and finish time agree between the two modes within 1%.
  2. Each audited site has its own numeric check that its rate per second of
     game time matches the original cadence (for example steering ramp per
     second, gear-check frequency, AI decision frequency).
  3. Short scripted scenarios started from saved states (a punch landing, a
     car collision, a brake from top speed, a nitro burst) give the same
     outcome in both modes, where such scenarios can be set up.
  4. The owner plays it and signs off on feel. Where numbers and feel
     disagree, the owner decides.
  Exact RAM identity is not expected in this mode.
- Reference timing stays available permanently as the fidelity check, on the
  mechanical translation.
- Display rates above 60 Hz are handled by interpolation in the renderer, not
  by running logic faster **(my call)**.

### Stage 4: high-resolution 2D renderer

Replace the video-chip shim for the race view with a Godot renderer.

- Reverse engineer what the game decides to draw each frame (road segments,
  horizon, sprite list with positions, scales and frames, HUD) and expose that
  from the core as structured draw data, taken before it is turned into video
  chip writes.
- The stage starts by writing the draw-data contract as its own short
  document, once the draw path is understood: road geometry and perspective,
  horizon and camera, per-sprite position, scale, frame, draw order, palette
  and priority, HUD elements, and whatever per-scanline effects the road
  depends on. The owner approves it before renderer work begins. It cannot be
  written now because the draw path has not been reverse engineered.
- Extract original art from the user's ROM at first run and upscale it locally.
  Upscaled art is ROM-derived and is never distributed. The upscaling method is
  left to the builder; the result must keep the original style.
- Sprite scaling and road drawing happen at output resolution with sub-pixel
  positions, so motion is smooth and edges are crisp.
- Menus and front end may stay on the original-picture path until later.
- Acceptance: side-by-side with the original picture from the same replay,
  every object is in the same place at the same time; the owner signs off on
  look and feel.

### Stage 5: readable code, system by system

Replace translated routines with hand-written, named, documented C. Order:
bike physics (already understood), track data and road, rider AI, combat,
cops, progression, front end.

- The mechanical translation is kept forever as the buildable reference. It
  alone is checked against MAME, in reference timing. Hand-written code is
  never checked against MAME directly and needs no cycle accounting.
- Every replacement is checked differentially against the mechanical version:
  both run on the same recorded inputs with the same injected timestep
  sequence, and RAM effects must be identical at each loop pass.
- Data formats (tracks, bikes, riders) get documented schemas and loaders, so
  new content becomes data rather than code.
- The byte-exact RAM image may be replaced by ordinary structures only for
  systems whose every reader and writer has been rewritten. Such a system
  provides a function that writes its state back in the original RAM layout,
  so the differential check keeps working.

### Later (not designed here)

**Sound port (owner).** The owner designs synthesizers professionally and wants
to do this part: reverse engineer the Z80 sound driver and its music data,
convert the original tracks and effects into sequences (notes, timing,
instrument and FM patch parameters) in an editable format, then redo the sound
design with modern synths and effects. The original emulated sound stays
selectable as the reference. Format and tooling are to be designed with the
owner when this starts.

3D renderer on the same draw data; new tracks, weapons, modes, AI options;
online multiplayer built on the deterministic core and save/restore; possible
move to another language. Known risk for 3D: rider combat and crash animations
need custom work matched to original timing; free assets will not cover them.

## Constraints

- Personal, non-profit project. Road Rash is EA's property. The ROM and
  anything derived from it stay out of git and out of any release. A public
  release of engine code still carries some legal risk, since translated and
  hand-ported code derives from the original program even when users supply
  their own ROM. It is the owner's decision later, with legal advice first.
- Existing verified/hypothesis labelling in `analysis/` continues: claims about
  the original are marked as verified or hypothesis.
- Godot 4.7.1; MAME 0.289 for reference captures.

## Left to the builder

- Translator design, intermediate form, and how cycle costs are charged.
- Which permissive chip models to use, or whether to write them.
- Build system and test layout for the C core.
- The Godot extension's shape and whether a separate minimal test host exists.
- Upscaling method and art cache format.
- Order of work inside each stage.

## Risks

- **Interrupt timing fidelity** may make exact RAM conformance hard. Mitigated
  by the injected-timestep fallback.
- **Code discovery**: data decoded as code or missed routines. Mitigated by
  loud failure on unknown targets and play-driven coverage.
- **Unscaled logic** may change feel at 60 Hz. Stage 3's audit exists for this;
  the fallback is to run that logic at the original cadence and only smooth
  motion.
- **Draw data** may be entangled with video-chip specifics (raster effects via
  the HBlank handler at `0x14BE`). Stage 4 may need to model the road effect
  rather than intercept it.
- **(spike) Video timing in the race.** The race code polls the video chip's
  line counter heavily (43,599 reads in a 130-frame window) and takes about
  six line interrupts a frame. What it reads depends on timing, so the race
  cannot be bit-exact against MAME without a cycle-accurate video model. The
  plan is statistical validation in reference timing and exactness only at
  the CPU level. If the road picture proves sensitive to this, Stage 4's
  renderer replaces the mechanism rather than reproducing it.
- **Size**: this is months of work. Stage boundaries are where to stop and
  reassess.
