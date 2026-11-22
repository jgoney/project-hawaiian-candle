# Draw-data contract: what the game core hands a renderer each pass

Date: 2026-11-11. Status: **approved by the owner on 2026-11-11**, with the
decisions recorded at the end.

Evidence for everything here is in `analysis/race-picture.md`; section
numbers below refer to it. Each item is marked **measured** (the reading
reproduces the game's own numbers) or **read, not measured** (it is what the
code says, not yet checked against numbers).

## The idea in one paragraph

Today the port runs the original game code and an imitation of the video
chip turns the game's output into the original 320×224 picture. The game
decides what the scene is *before* it reduces it to that picture: where the
road goes, and where every rider, car and tree stands and how big it is. A
new renderer takes that description instead of the finished picture, and
draws it at any resolution. The game logic is untouched, so it stays the
game that was measured against MAME.

## One frame of the original, and where each part comes from

```
 line 0   +--------------------------------------------------+
          |  sky: one flat colour                             |  backdrop
          |  clouds, mountains, far hills: 3 strips that      |  two tile sheets,
          |  slide sideways at different speeds               |  shifted per strip
 horizon  |--------------------------------------------------|  (moves up and down
 (47..85) |  grass  /                       \   grass        |   with hills)
          |        /   road: flat grey,      \               |  grass = the same two
          |       /    the gap between the    \              |  sheets, shifted on
          |      /     two grass sheets        \             |  every line; road =
          |     /        [riders, cars,         \            |  backdrop colour
          |    /          trees, signs:          \           |  objects = sprites,
          |   /           scaled pictures]        \          |  scaled by the CPU
 line 152 |==================================================|
          |  dashboard, names, timer, mirrors                |  a fixed sheet
 line 223 +--------------------------------------------------+
```

The game redraws all of this about 10 to 11 times a second (once per pass of its
main loop). Between those moments the picture does not move at all
(measured, section 3.4).

## What the core hands over, once per pass

### 1. Camera (measured, section 4.1)

| Item | Meaning | Units |
|---|---|---|
| segment, fraction | where the camera is along the track | segment number; 0..65,535 through it |
| sideways | the player's position across the road | world units; the road is 448 wide (±224 from its centre) |
| height | camera height above the road | world units |
| view | first and last screen line of the 3D view, the horizon reference line, the screen centre | original pixels |

### 2. Road (measured, sections 4.1 and 4.2)

Two forms are offered. The renderer should use the first.

**a. The track ahead, to project itself.** For each of the 32 segments ahead
of the camera: a **curve** number (how much the road's direction changes
over the segment) and a **hill** number (how much it rises or falls). With
the camera data and the game's perspective table (a list of scales by
distance, taken from the user's ROM at start-up), the renderer can place
every road point itself, at any resolution, with fractions. The arithmetic
is written out in `src/rr2port/scene.py` (`project_road`) and reproduces the
game's result exactly in 2,155 of 2,176 memory images; 13 of the rest were
taken while the game was half-way through the calculation, and 8 are not
explained (section 4.1).

**b. The game's own projected points**, for checking and as a fallback: for
each of the 32 points, its screen line, centre, left edge and right edge in
original pixels, and the line above which a nearer hill hides it.

Distance is known for every road point: point *n* is *n* segments from the
camera, less the fraction already travelled.

### 3. Objects (measured, section 4.4, except where marked)

One record per vehicle, roadside object or sign, in drawing order (far to
near):

| Item | Meaning |
|---|---|
| x, foot line | where it stands on screen, in original pixels |
| size | 1.0 is the picture's natural size |
| hidden below | screen line below which a hill hides it |
| picture, second picture | which picture(s) from the game's picture table |
| mirrored, colour set | drawn flipped; which of four colour sets |
| copies, step | how many more of the same to place beside it, and how far apart |
| road point, distance | which road point it stands at or behind; distance from the camera in 256ths of a segment |
| sideways offset | how far from the road centre, in pixels at that distance |

How a record becomes a picture on screen is measured: the game's sprites
are reproduced exactly on every pass checked of the recorded race (2,174 of
its 2,176). So the renderer
can draw each object itself from the record and the ROM's picture: the
picture's top-left corner, its scaled size and what is cut off by the view's
edges or a hill all follow rules that are written out in
`src/rr2port/objects.py`. The pictures themselves are plain 16-colour
bitmaps and can be read straight from the user's ROM at start-up.

Two properties of the original that a high-resolution renderer will want to
*improve on* rather than copy: objects are scaled sideways in two-pixel
steps, and scaling is nearest-neighbour.

Depth: a roadside object stands exactly at one of the 32 road points
(measured). A vehicle has its own continuous distance (measured exactly for
72% of the records in the vehicle part of the table, 8,062 of 11,154; the remaining records are made
by code not yet traced, which is a **hypothesis** about what they are). That
is the distance information asked about: it exists per road point and per
object, not per pixel.

**Not yet settled:** which picture a vehicle shows (the animation logic),
and the records for objects linked to vehicles. The renderer does not need
either to draw what the core hands it; interpolation will need the second,
to follow those objects between passes.

### 4. Sky and scenery strips (not traced)

For each of the three strips: which part of the two scenery pictures it
shows and its sideways position. Today these exist only as the video chip
settings of sections 1 and 2. The renderer can start by taking the strips
from the original picture as they are.

### 5. Dashboard and mirrors (not traced)

Handed over as a finished 320×72 picture from the existing path (lines 152
to 223). It is separable: nothing in the 3D view depends on it (section
3.6). The design document already allows the dashboard to stay on the
original-picture path.

### 6. Colours (measured, section 2)

The 64 colours in force for the pass.

## What this makes possible for crisp edges

- **Road edges, edge lines, the horizon: real geometry.** With form 2a the
  renderer computes the edge of the road as a smooth curve through the 32
  points at the output resolution, so the staircase and the per-line wobble
  of the original go away. This is the measured part of the contract, so it
  can be built and checked first.
- **The grass and road surfaces** are flat colours with a dither pattern in
  the original. They can be drawn as clean fills, or keep the pattern at a
  finer scale; that is a look decision for the owner.
- **Objects stay pixel art, but placed and scaled finely.** The original
  scales each picture in whole pixels at 320×224. The renderer scales the
  same picture once to the output resolution, at a fractional position, so
  edges are as clean as the upscaling of the source art allows and objects
  no longer jump a whole pixel at a time.
- **True antialiasing by depth** (blending each pixel by its distance) is
  not available: depth is known per road point and per object, not per
  pixel, and the objects are flat pictures. What is available is exact
  ordering (what is in front of what) and smooth edges for everything drawn
  as geometry.

## What interpolation needs

The picture is still for the 3 to 9 frames of a pass, so there is nothing
of the original's to preserve between passes. To draw 60 pictures a second
the renderer blends two consecutive passes' descriptions:

- **Blend:** camera fraction, sideways position and height (then re-project
  the road, form 2a); each object's x, foot line and size.
- **Do not blend, switch:** which picture an object shows (animation
  frames), mirror, colour set, drawing order.
- **Needs care:** matching an object in one pass with the same object in
  the next. The records carry no identity; roadside objects can be matched
  by their track segment (+0x14 of the record), vehicles need their source
  object's address. The core must add an identity to each record. This is a
  change to what the core exposes, not to the game.
- **A known cost:** blending between the last two finished passes shows the
  game about one pass later than the original does (roughly a tenth of a
  second more delay between pressing a button and seeing the result).
  Blending forward from the last pass using its speed avoids the delay but
  can overshoot. This is the main feel decision in Stage 3a and wants the
  owner's eye.

One more measured fact that matters here: the original already shows the
road about one pass behind the physics (section 4.3). A renderer that
projects the road itself from the newest camera position would show it a
pass *earlier* than the original. That is more responsive, and different;
the default should match the original.

## Decisions (owner, 2026-11-11)

1. **Order of work:** as proposed. (a) Trace where object pictures land and
   their format, (b) high-resolution road with the original objects laid
   over it, (c) high-resolution objects, (d) interpolation.
2. **Surface look:** not a flat fill for the grass. Keep the original grass
   pattern at original pixel size and make the edges crisp (road edge, edge
   line, horizon); the road surface is already one flat colour in the
   original. A finer grass texture may be tried later as an option. (The
   owner asked for a recommendation and this was it; the owner had already
   said a flat fill would look bad.)
3. **Interpolation:** blend between the last two finished passes by default.
   The added delay is about half a pass to one pass (45 to 90 ms) and does
   not shrink with the display rate, because the logic stays at about 11
   passes a second. Prediction is built as the comparison, and the owner
   judges by feel.
4. **Second recording** (crash, police): the owner will make one when
   convenient. It does not block the renderer work.

## State after the first renderer step (2026-11-11)

Evidence: `analysis/race-picture.md`, sections 4.4 and 5.

**Done.** Decision 1(a): how a record becomes sprites is measured (section 3
above). Decision 1(b): the crisp road, switched on with R in the Godot
window. It uses the road in form 2b (the game's own 32 projected points) and
draws the same straight runs between points that the original draws, at the
enlarged resolution. It is written in C beside the game core, as an
extension of the video chip model, so that it could be checked against the
original picture frame by frame; the Godot window shows its output.

**What it showed, for the owner's eye.**

- Far from the camera the road edge still wobbles, because the game's road
  points sit on whole pixels.
- The grass speckles lean near the camera, because the grass pattern is
  shifted row by row together with the edge. Decision 2 asked for the
  original pattern at original size with crisp edges; the pattern is
  original and original-size, but it does not look unchanged. Keeping the
  speckles upright while the edge stays crisp is possible (shift the edge
  per row and the pattern per line) and is the next thing to try if the
  owner dislikes the lean.

**Left for the next plans.**

- Form 2a with fractions (projecting the road from the camera and the track
  bytes instead of taking the game's whole-pixel points), which removes the
  far wobble and is what interpolation needs anyway.
- Smoothing the edge into a curve between road points, as a look option.
- The centre-line dashes, which are still the original's sprites.
- High-resolution objects (decision 1c), from the pictures now extracted
  from the ROM.
- Interpolation (decision 1d). It needs each record to carry an identity,
  and the records made for objects linked to vehicles to be traced.

## State after fine road points (2026-11-11)

Evidence: `analysis/race-picture.md`, section 6.

**Done.** The road is now in the contract's form 2a as far as the points
are concerned: the port projects the road itself from the camera and the
track, keeping fractions of a pixel, and the result is proven to be the
game's own road on every frame it is used. On a synthetic straight road the
edges are measured straight where the whole-pixel ones are not; on the race
the far-edge wobble looks gone in the three frames compared.

**New, for the owner's eye** (F in the Godot window switches fine points on
and off while the crisp road is on):

- On about one frame in eight, at a sharp far bend or a hill crest, one
  line shows the road edge well away (8 to 107 pixels) from where the
  original has it on that line. It is where the original has it one line
  further on: the edge now moves part-way through the line instead of at a
  line boundary.
- Near the camera the fine edge can sit a few pixels, rarely up to 22, from
  where the original draws it. The original's edge there is its own
  rounding magnified (it extends a short run a long way). Whether the fine
  one is steadier from pass to pass is a hypothesis.
- The riders and centre-line dashes are still placed on the original's
  road.

**Still left.** The camera's place within a segment is used with the game's
8-bit precision (the finer value comes with interpolation); curve smoothing
between points; the leaning grass speckles; the centre-line dashes;
high-resolution objects; interpolation.

## State after high-resolution objects, stage 1 (2026-11-11)

Evidence: `analysis/race-picture.md`, section 7.

**Done.** Decision 1(c), first stage: in the enlarged picture every rider,
car and roadside object is drawn from its full picture in the ROM at the
output resolution, at the game's place and size, in the game's order. It is
built, like the crisp road, as an extension of the video chip model beside
the game core, so it could be checked: the port's own repeat of the game's
object drawing equals the game's on every pass of the recorded race, and a
frame uses it only when the sprites and tiles in video memory are byte for
byte the ones it describes. Nothing outside the object sprites changes.

**For the owner's eye** (O in the Godot window switches it off and on while
the crisp road is showing):

- Distant objects keep their detail: trees have their tops, far riders are
  riders. This is the visible gain.
- Objects that the original shrinks to a pixel or two are now plainly
  there, so the far scene is busier than the original's and far objects are
  seen sooner. Whether that is wanted is the owner's call; it can be
  limited (for instance by fading in below a size) if not.
- Near objects are almost unchanged: the ROM's pictures have no more detail
  to give when drawn larger than their natural size.

**Departures from "placed and scaled finely"**, decided while building:

- *Scaled finely: yes. Placed finely: not yet.* Objects still stand on the
  game's whole pixels and move once per pass. Placing them between pixels
  only shows once the picture moves between passes, and needs the
  vehicle-placement code that is still untraced, so it is left to
  interpolation.
- A picture now reaches its true scaled width; the game loses up to two
  pixels of its far side to rounding in pixel pairs.
- Two oddities of the game's scaler (a wrong first row or first pixel pair
  on pictures that are cut off) are not copied.

**Still left.** Smoother art for objects drawn larger than natural size
(stage 2: a swappable set of upscaled pictures); the centre-line dashes;
the leaning grass speckles; curve smoothing between road points;
interpolation, with object identities and fractional placement.

## State after in-between pictures (2026-11-12)

Evidence: `analysis/race-picture.md`, section 8. Design:
`docs/superpowers/specs/2026-11-12-interpolation-design.md`.

**Done.** The renderer no longer has to wait for the game: on every video
frame it can draw the scene part-way between two updates (the road
projected from an in-between camera; followed objects placed between
pixels), in three modes that differ in how late the picture is. "Placed
finely", left open after stage 1 of the objects, is now done for every
object that can be followed. At each update the in-between scene is
exactly that update's scene, and the game replays identically.

**For the owner's eye** (I in the Godot window steps through blend,
predict, late blend and off): whether predict's corrections are visible,
whether blend's delay is felt, and how much the stepping centre-line
dashes stand out against a road that now slides.

**Still left.** The centre-line dashes (stage 3 of the design); the riders
in the mirrors; poses (new art, not interpolation); a recording with a
crash and a police stop, to check the jump rules on the real game; softer
far objects; smoother enlarged art.

## State after the dashes and softer far objects (2026-11-13)

Evidence: `analysis/race-picture.md`, sections 9 and 7.

**Done.** The centre-line dashes are drawn from the fine road as strips and
move with the in-between pictures, wherever the port's description of them
is proven against the game's memory (all updates but those near the finish
line). Far objects can be softened (B): the mean of the art a pixel covers
instead of one pixel of it.

**For the owner's eye:** the dashes sliding in blend at 100% and 200%;
B on and off on the far trees and riders.

**Still left.** The road surface's banding (not traced); the dashes near
the finish line; the riders in the mirrors; poses; a recording with a crash
and a police stop; smoother enlarged art.

