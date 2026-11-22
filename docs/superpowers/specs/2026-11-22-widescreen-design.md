# Widescreen race view: design

Date: 2026-11-22. Status: for the owner's approval.
"Owner" marks what the owner decided; "my call" marks choices made while
writing this that the owner has not discussed.

## Why

The port shows the race in the original's 320 x 224 shape (4:3-ish) in the
middle of a wide monitor. The owner liked the idea of a widescreen view
(owner: "Your ideas are good, let's look into those next"; then "Yes
please" to this design). A feasibility study on 2026-11-22 measured what
the game has beyond the 320-pixel edge on the six recorded sessions and
drew real 398-wide frames from recorded data
(`generated/widescreen-study/`, ignored by Git). Its findings are written
up in `analysis/widescreen-study.md`, and its scripts are kept beside the
pictures. The numbers quoted here are the reason for the design, not pass
marks: the build measures its own and reports them.

## What the owner will see

- **W** (my call) switches the one-player race between the original shape
  and a wide view. It is on the Tab list. Like the in-between pictures it
  needs the crisp road (R), the fine road points (F) and the full-picture
  objects (O) all on (my call); with any of them off the picture is the
  original shape and W says what it needs.
- The wide view shows more of the scene on each side at the same size:
  more road shoulder and grass, more sky and hills, riders and cars that
  the original cuts off at its edges. Nothing is stretched.
- The dashboard and mirrors stay their original size in the middle, with a
  plain dark panel either side (my call).
- Menus, the two-player race and everything else are the original picture,
  centred, as today.
- The game plays exactly the same with it on.

## Decided by measurement (the study)

- **The geometry allows it.** The game's projection is an ordinary pinhole
  with a focal length of 256 pixels, so a wider picture at the same scale
  is consistent: 64 degrees across at 320 wide, about 76 at 398.
- **Sky, clouds and hills** are 1,024-pixel panoramas that wrap. Nothing to
  do.
- **Riders and cars** are never dropped by the game for being off to the
  side; only the port's own clip at 0 to 319 hides them.
- **The grass** is stored as a block exactly 320 pixels wide per side. It
  covers a 398-wide view except when the bike is far to one side: on up to
  3.5% of road lines, in up to 5.7% of updates, depending on the
  recording. Every ground row repeats with a period of 8 pixels or less,
  so it can be continued.
- **Roadside objects** (trees, signs, houses) are dropped by the game once
  they pass the edge of its own view. In a 398-wide view at least one
  object would be visibly missing inside the side bands in 16% to 29% of
  updates (5% to 9% in the original, where it also happens).
- **The dashboard** has no art beyond 320 pixels.

## Requirements

1. **The game is untouched.** Every existing conformance measurement
   (`rr-race-replay` on the six recordings: `RESULT match`) holds with the
   wide view on. The wide view only reads.
2. **The original picture is untouched.** The 320 x 224 picture the video
   chip model makes is not changed, and with W off every picture the port
   draws is what it draws today, pixel for pixel.
3. **The middle is the same picture.** With W on, the centre 320 columns
   of the race view (above the dashboard) are what the crisp picture shows
   with W off, except where something the original cut off at its edge is
   now drawn whole, or a centre-line dash or object the original dropped
   at its edge now appears. That is measured on the six recordings and
   reported as a count of frames and pixels that differ, with the classes
   of difference named; any other difference is a fault.
4. **No garbage at the sides.** The side bands never show wrapped plane
   content or a stray copy of the other side's edge line; where the game's
   grass block falls short the grass is continued. Measured: on the six
   recordings, no line of the wide view shows plane content from outside
   the grass block and its edge-line band.
5. **Mirrors and dashboard are safe.** Anything the game draws below the
   race view (line 152 on), including the riders in the mirrors, is drawn
   exactly as with W off.
6. **Width.** The wide view is 16:9 for the window's square pixels: 398
   game pixels (my call: one fixed width for now; the cap that makes sense
   is about 448, beyond which the game has nothing to show at the sides).
7. **Speed.** A frame with the wide view still fits well inside a video
   frame (16.6 ms) at the window's usual scale. The cost is measured and
   reported.
8. **In-between pictures, the crisp road, fine objects, soft far objects,
   the CRT screen and capture/replay (P) all keep working** with W on. A
   capture records the setting, and `rr-session-replay` reproduces a wide
   picture exactly.

## Frames the port cannot fully describe (my call)

Today the port draws a frame from the game's own sprites, enlarged,
whenever it cannot prove what is on screen (the first frame of a race, an
object set it cannot match, an in-between frame it cannot make, a sprite
with no full picture, the dashes near the finish line). The project's rule
stands: never a picture for something the port has not matched. On such a
frame the view **stays wide**; the side bands show the sky, hills, ground
and road, and whatever cannot be described is drawn as the game has it,
which means cut at the old edge. The picture never snaps back to the
narrow shape for a frame. How often this happens is measured and reported.

## The middle and the sides (my call)

The centre 320 columns show exactly the set of objects the picture shows
with W off: the wide view never adds an object there that the original
dropped (for lack of sprites or tiles, or as an off-screen copy), and an
object in a side band never takes the place of one in the centre. Where
the port's drawing has limits (sprites, tiles, sprites per line), the
centre is served first and the bands get what is left, or the limits are
raised for the bands only. The allowed differences in requirement 3 are
therefore only these: a picture the original cut at x = 0 or 319 now
continues past it, and a centre-line dash the original cut or dropped at
the edge now runs on.

## Stages

1. **Wide view, objects as the game has them.** The wide picture, grass
   continued, the port's clip widened for riders, cars and the roadside
   objects the game did make records for, dashes running wider, dashboard
   centred. Roadside objects still vanish inside the side bands where the
   game drops them: this is expected at this stage and is reported with
   its measured frequency. All eight requirements are met and measured
   before the owner tries it (my call). The stage ends with the owner's
   verdict: is widescreen worth having, and is the pop-out tolerable?
2. **Roadside objects the game dropped** (only if the owner wants it after
   stage 1; it is planned then, not now). The port works out the roadside objects for the wider view
   itself, by repeating the game's roadside walk on a copy of its memory
   with a wider cut-off, and draws them with its own drawing. Proven the
   way the crisp objects are: with the original cut-off the port's walk
   must give exactly the game's own records, on every update of the six
   recordings. (The game's roadside walk and its culls have so far only
   been read from the code, not tested: `analysis/race-picture.md`, 3.2.)

## Not part of this (non-goals)

- Widescreen menus, two-player races or dashboard art.
- Carrying the road view down beside the dashboard.
- Changing the field of view or the camera.
- Any change to what the game computes (wider culling inside the game).

## What would make this a failure (anti-goals)

- The race no longer replays identically, or the original picture
  changes.
- Wrong things drawn in the side bands (wrapped scenery, doubled edge
  lines, a mirror rider's copy on the dashboard).
- A wide view that is the old picture stretched.

## Left to the builder

- How the wider picture is carried from the renderer to the window, and
  how the window chooses its scale and letterboxing for it.
- How the grass is continued, and how the road's own edges are computed
  beyond the game's clamps.
- The sprite and tile limits of the port's drawing where a wider view
  needs more than the original's.
- How the in-between pictures treat objects entering and leaving the wider
  view.

## Constraints carried over

- Nothing derived from the ROM is committed; tests use made-up data.
- Claims in `analysis/` are marked verified or hypothesis.
- The Godot library is not rebuilt while a window may be using it.
