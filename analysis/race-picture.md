# How the race picture is made

Plan: `docs/superpowers/plans/2026-11-11-draw-path-and-scene-description.md`.
Statements are **verified** or a **hypothesis**. Everything here is measured
on the native port replaying the `full-throttle` recording at original CPU
speed, which is measured equal to MAME 0.289 for that race
(`analysis/race-conformance.md`). Nothing here is a claim about a real
console.

## Summary

**Result.** How the race picture is made is now known from the picture down
to the game's own description of the scene, and both halves of it, the road
and the objects, are proven by prediction.

- **Verified.** Which layer draws each part of the picture (section 1). Who
  writes the video chip and from which staged copies in work RAM (section
  2). The road: from the track's curve and hill bytes and the camera
  position to the 32 projected road points (identical to the game's in
  2,155 of 2,176 images) and on to the per-line shifts sent to the video
  chip (with images taken when the table is finished: 4,350 of 4,350 staged
  tables exact) (section 4). That the picture does not move between passes
  (3.4). That a faster CPU makes line-interrupt writes straddle screen lines
  (3.5); that this is the stripe in the owner's screenshots is a hypothesis.
- **Verified.** How a draw record becomes sprites: the picture format, the
  software scaling, where the picture lands, clipping, the cutting into
  sprites, and copies of roadside objects. The game's sprite list and sprite
  pictures are reproduced exactly on all 2,174 passes checked (of the
  race's 2,176; the run starts and ends part-way through a pass); roadside
  records are
  placed exactly (28,603 of 28,603) (sections 3.2, 3.3, 4.4).
- **Partly verified.** Vehicles are placed at their own continuous distance,
  not at one of the 32 road points; the rule gives exactly the game's
  numbers for 8,062 of 11,154 records in the vehicle part of the table (and
  for 73 more apart from two low bits of the foot line), the rest being made
  by untraced code (4.4).
- **Not traced.** Which picture a vehicle shows, the centre-line dashes
  (read only), the sky strips, the dashboard.
- **Built and verified.** A first renderer step, the crisp road (section
  5): the picture enlarged, with the road's edges drawn at the enlarged
  resolution from the game's road points. The enlarged picture still holds
  the original exactly on all 12,843 frames of the recorded race, and the
  crisp road was drawn on 11,823 of the 11,824 race frames.

- **Built and verified.** Fine road points (section 6): the port repeats
  the game's road projection, keeping the fractions the game drops, and
  reproduces the game's own points on every crisp frame of the race
  (11,823 of 11,823). The crisp road is drawn through them. That this
  removes the far-edge wobble is verified on a synthetic straight road and
  seen by eye on three race frames; where and by how much the fine edge
  differs from the game's is measured in section 6.

- **Built and verified.** Fine objects (section 7): the game's drawing of
  the objects is repeated in C and equals the game's on every pass of the
  race (2,179 of 2,179), and the enlarged picture draws each object from
  its full picture in the ROM instead of from the game's shrunken sprite.
  Nothing outside the object sprites changes on any of the 12,843 frames.
  How the objects then look is by eye.

- **Built and verified.** In-between pictures (section 8): every frame can
  be drawn from the scene part-way between two of the game's updates. At
  each update the in-between scene is exactly the scene of sections 6 and 7
  (every pair of updates that could be made, 2,165 of 2,174 at original
  speed and 5,343 of 5,343 at 200%), the game still replays identically,
  and the share of frames in which the road edge stands still while the
  camera moves falls from 82% to about 5%. How far a guessed picture is
  from the truth is measured; how it looks in play is for the owner.

- **Built and verified.** The centre-line dashes (section 9): how the game
  describes them (a start and a width per screen line, every other track
  segment left blank) is repeated by the port and equals the game's memory
  on every update the model covers (2,018 of 2,018; the 161 near the finish
  line are not covered). The dashes are drawn from the fine road as strips
  and move with the in-between pictures.

**Limits.** One recorded race (`full-throttle`): one track, one bike, no
crash, no police, one player. The reference is the port at original speed,
measured equal to MAME 0.289 for this race.

**To repeat.** Build with `make -C port race-replay`. Sections 1 and 2:
`rr-race-replay` with `--sources-at`, `--line-state-at`, `--video-state-at`,
`--video-log` (see the tool's header comment), then `rr2-picture-report` and
`rr2-video-report`. Section 3: `--ram-writes` and `rr2-ram-writes`. Section
4: `rr2-scene-check`, `rr2-object-check`, both on images taken with
`--ram-at-pc`. Sections 5 and 6: `rr-race-replay --scaled-check 4`. Section
7: `--objects-check`, and `--scaled-check 3 --fine-objects`. Section 8:
`--scaled-check 3 --fine-objects --inbetween MODE --inbetween-check`. Section 9:
`--dashes-check`. What it means
for a renderer:
`docs/superpowers/specs/2026-11-11-draw-data-contract.md`.

Words used: the video chip builds each picture from **layers**. From back to
front: a **backdrop** (one flat colour), **plane B** and **plane A** (two
large sheets of 8×8-pixel tiles that can be shifted sideways and up or down,
each line separately if wanted), the **window** (a fixed sheet that replaces
part of plane A), and **sprites** (up to 80 small movable pictures of 8×8 to
32×32 pixels). A **line interrupt** lets the game run a little code at a
chosen screen line while the picture is being drawn, to change settings
part-way down.

## 1. Which layer draws what (verified on 12 frames)

Measured with `rr-race-replay --sources-at` (which layer supplied each
pixel), `--line-state-at` (the settings each line was drawn from) and
`--video-state-at`, summarised by `rr2-picture-report`. Frames 1042, 1166,
1518, 2889, 4000, 4064, 6196, 8278, 10261, 11981, 12501 and 12820: the frames
in which passes 3, 20, 70, 300, 500, 510, 900, 1300, 1700, 2000, 2100 and
2170 start. All twelve have the same structure.

| Part of the picture | Layer | How |
|---|---|---|
| Sky | backdrop | Colour 63 from the top of the screen down to the horizon line. |
| Clouds, mountains, far hills | planes A and B | Three strips above the horizon. Each strip has its own sideways shift, and the up/down shift is changed at the two lines where strips meet (for example lines 35 and 47 in frame 4000), so the strips slide past each other: parallax. |
| Road surface | backdrop | At the horizon line the backdrop colour is switched from 63 (sky) to 62 (dark grey). The road is simply what shows where no other layer covers. |
| Left verge and its edge line | plane A | Below the horizon plane A holds grass with the road's edge line drawn into the same tiles. Every line has its own sideways shift, which puts the left edge of the road where it belongs on that line. |
| Right verge and its edge line | plane B | The same for the right edge, shifted separately per line. |
| Centre-line dashes | sprites, behind the planes' priority level | 4 to 10 low-priority sprites per frame. |
| Riders, cars, trees, signs, the player's bike | sprites | 17 to 53 sprites in the list per frame. Each sprite has its own tiles, numbered upwards from tile 0x634 (0x434 in the first frame sampled). |
| Dashboard, names, timer | window | Lines 152 to 223. |
| The two mirrors | plane B (and sprites when a rider is behind) | Shows through gaps in the window at lines 200 to 215. |

So the road is not drawn as a shape. It is the gap between two grass sheets,
and its curves and width come entirely from the per-line sideways shifts of
planes A and B below the horizon (72 to 105 lines, a different value on
almost every line).

What changes part-way down a frame, in every frame sampled:

- The **line interrupt spacing** (register 10) is rewritten six times, at
  lines 2, 4, 5, then three lines that move with the horizon (for example 31,
  43, 74 in frame 4000). The game re-aims its next line interrupt each time.
- The **up/down shift** of both planes changes at four lines: the two strip
  boundaries above the horizon, the horizon itself, and line 157.
- The **backdrop colour** (register 7) changes at the horizon line (63 to 62)
  and back at line 157.
- No other register changes during a frame.

The horizon line itself moves between frames (lines 47 to 85 in the sample)
as the road rises and falls.

The video memory tables are in the same place in every frame: plane A at
0x4000, plane B at 0x6000, window at 0x3000, sprite list at 0xC400, sideways
shift table at 0xC000 (one pair of values per line).

**Hypothesis** (for sections 2 and 3 to test): because every sprite has its
own freshly numbered tiles, the game does not use ready-made pictures at
fixed sizes; it draws each object's picture at the needed size into tile
memory every pass (software scaling), and that is the work done by the code
at `0xA000`–`0xABFF`, the largest single cost in a pass.

## 2. Who writes the video chip (verified on passes 66–80 and 500–520)

Measured with `rr-race-replay --video-log` (every write to the video chip,
with the address of the instruction making it) and `rr2-video-report`. With
the log, a layer map and a line-state file all switched on, the whole race
still replays as `RESULT match` (2,176 of 2,176), so the instruments do not
disturb the game.

The game prepares almost everything in work RAM and has the video chip copy
it across in blocks (a "DMA"). Each pass ends with four block copies, all
made by one routine (the instruction at `0x12300`) that the frame interrupt
handler runs with other interrupts shut out, during the blank lines below
the picture (lines 226 to 229) so nothing is seen half-copied:

| What | Video memory | Size | Staged in work RAM at | Notes |
|---|---|---|---|---|
| Sprite pictures | from tile 0x634 (address 0xC680), or tile 0x434 (0x8680), alternating | 2,000 to 2,800 words, different every pass | `0xFF4742` or `0xFF7A0A`, alternating | Sent one frame before the other three. |
| Dashboard sheet | 0x3000 | 1,856 words (29 rows of 64 cells) | `0xFFACD2` or `0xFFBB52`, alternating | The window layer. |
| Road shifts and sprite list | 0xC000 | 832 words in one block | `0xFF3A40` or `0xFF40C2`, alternating | The first 896 bytes are the per-line sideways shifts of planes A and B (224 lines × 2 planes × 2 bytes). The sprite list follows at 0xC400 (80 entries × 8 bytes). |
| Colours | all 64 | 64 words | `0xFF1C1E` | |

Everything is staged twice, in two alternating sets of buffers. The game
builds the next picture in one set while the other is on screen: double
buffering. The sprite pictures alternate between two areas of tile memory for
the same reason, which is why the sprite list's tile numbers start at 0x634
in one pass and 0x434 in the next.

That the sprite pictures are a different size every pass and are copied from
work RAM confirms section 1's hypothesis in part: **the pictures of riders,
cars and roadside objects are drawn into memory by the CPU every pass**
(verified); that they are being *scaled* from larger art, and by which
routine, is still a hypothesis until section 3.

The two tile sheets of planes A and B (sky, hills, grass) are **not**
written during a race pass. They are fixed pictures; all the motion of the
scenery and the road comes from shifting them.

The interrupt handlers write only settings, never pictures:

- **Frame interrupt** (callback `0xE038`), once per frame at lines 224–225:
  display settings (register 1), the first line-interrupt distance (register
  10), the sky backdrop colour (register 7), both planes' up/down shift, and
  the addresses of the two plane sheets (registers 2 and 4).
- **Line interrupt** (`0x14BE`), six times per frame. Each time it first
  re-aims the next line interrupt (register 10, written twice), then a few
  lines later writes plane A's up/down shift, plane B's up/down shift and
  the backdrop colour. So each of the six interrupts carries one small
  record: distance to the next one, two shifts, one colour. The six are, in
  order: two near the top of the screen, the two cloud/mountain strip
  boundaries, the horizon (where the backdrop becomes road grey), and the
  top of the dashboard.

Nothing else writes the video chip during a race pass.

## 3. From the tables back to the scene

Found with `rr-race-replay --ram-writes` (which instruction writes a range of
work RAM) and by reading the code those instructions belong to. In this
section, "read from the code" means the statement is what the instructions
say; whether it predicts the game's numbers is tested in section 4, and only
what passes there is called verified.

Order of work in one pass (from the MAME trace of pass 500, 58,000
instructions in the main loop): turn the previous pass's road into per-line
shifts (`0x14822`), make draw records for vehicles (`0x15672`) and roadside
objects (`0x16E8C`), sort them by distance (`0x172C0`), draw every object's
picture at its size and build the sprite list (`0xA4EE`, 55% of the pass),
draw the centre-line dashes (`0x13F20`), run the physics (`0x175C4` and
others), project the road for the next pass (`0x14A86`).

### 3.1 The road (verified in section 4)

The road is a classic "pseudo-3D" road: a strip of 32 segments ahead of the
camera, each described by two numbers, projected one after another.

**Track data.** A ring of 256 two-byte entries at `0xFFD828`, one per track
segment: a signed **curve** byte and a signed **hill** byte. The camera's
place on the track is the long at `0xFF0A0A`: high word the segment number,
low word (`0xFF0A0C`) how far through that segment (0 to 65,535).

**Projection** (`0x14B44`). Starting at the camera's segment and walking 32
segments forward (`0xFF1CA0` holds the 32):

- sideways position starts at minus the player's own sideways position
  (`0xFF0A02`) and, per segment, moves by a step that itself grows by each
  segment's curve byte. So a curve byte is a change of direction, and a
  constant curve byte gives a steady bend;
- height starts at the camera height (`0xFF0A08`) and falls or rises by each
  segment's hill byte;
- distance is the segment's number counted from the camera, in 256ths of a
  segment, less the part of the first segment already travelled;
- a **perspective table** in the ROM at `0x27C84` gives a scale for each
  distance. Screen position = world position × scale ÷ 65,536, plus the
  screen centre (`0xFFF2BE`, 160) sideways or the horizon reference line
  (`0xFFF2BC`) vertically. The road's half-width is 0xE0 world units, scaled
  the same way.

The result is five arrays of 32 numbers, one per segment: screen line
(`0xFF1E20`), centre (`0xFF1DBC`), left edge (`0xFF1E84`), right edge
(`0xFF1F4C`) and the screen line clipped so that it never goes back down
(`0xFFD2B8`, which is how a hill hides the road behind it). A second set of
the same arrays (`0xFF1FB0` onward) is for the second player's half of the
screen and is all zero in a one-player race.

**From segments to screen lines** (`0x153EA`). Between each pair of
neighbouring segment points the edge is a straight line: the routine steps
down the screen lines between the two points and writes one position per
line (left edges to `0xFF27DE`, right edges to `0xFF2D1E`, two more arrays
for the centre line).

**From screen lines to the video chip** (`0x14822`). For each line from the
bottom of the 3D view up to the horizon: plane B's sideways shift is the
right edge (limited to 0..0x180) plus 0x2A0; plane A's is the left edge
(limited to −0x160..0x140) minus 0x2A0. Those two numbers per line are what
section 2's "road shifts" block carries to the video chip.

### 3.2 The objects (read from the code; tested in section 4.4)

Everything that is not road, sky or dashboard goes through one table of
**draw records** at `0xFFCEB8`, 22 bytes each (`0xFFDC42` counts them), with
a list at `0xFF08D6` giving the order to draw them in (far to near is
decided by sorting on distance, `0x172C0`; the list ends with 0xFFFF).

| Offset | Meaning |
|---|---|
| +0 | screen x of the object's anchor point |
| +2 | screen line of its foot |
| +4 | size: 0x4000 is the art's natural size, 0x2000 half, and so on |
| +6 | screen line below which it is hidden (by a hill) |
| +8 | which picture: an index into a table of pointers at ROM `0x56A0A` |
| +0xA | flags. Bit 15: draw mirrored. Bit 14 (roadside): which side of the road. Bits 12–13: colour set. Bits 8–10: how many extra copies to place beside it. Bit 7: the picture has an anchor point (see 3.3). Bit 6: which half of a two-player screen. Bit 5: uneven spacing for the copies. Bits 0–4: the road point it stands at or just behind |
| +0xC | sideways offset from the road centre in screen pixels; also the step between copies |
| +0xE | distance (256ths of a segment) |
| +0x10 | (roadside) the byte from the roadside list: size class in the high four bits, sideways offset in the low four |
| +0x12 | a second picture drawn with the first (0 if none), for two-part objects |
| +0x14 | the track segment it belongs to |

A parallel table at `0xFF06D6` holds, for each record, its distance from the
camera as a long (segments and a 16-bit fraction).

**Roadside objects** (`0x16E8C`, one record made by `0x17198`) come from a
list of 4-byte entries (pointer at `0xFFD352`) that says, for a stretch of
track, which picture, how many times to repeat it, how many segments apart,
which side, how far from the road and how big. Each object stands exactly at
one of the 32 projected road points *n*:

- distance = *n* × 256 − (the camera's fraction ÷ 256);
- size = (size class × 128 + 0x200) × 0x4000 ÷ (distance + 0x200);
- foot line and hidden-below line are that road point's screen line and
  clipped line;
- x = the road point's centre + (offset class × 64) × the perspective scale
  at that distance, to the left or right by bit 14;
- an object whose x falls outside 0..0x167, or that is too far to the side
  for its distance, gets no record (read from the code only: which records
  exist is not tested in 4.4, nor is the walk through the roadside list
  described above).

**Vehicles** (`0x15672`, placed by `0x1628A`) are *not* tied to the road
points. A vehicle object (addresses listed at `0xFF04F8`) has its own track
position (+4, segments and fraction), sideways position (+0x30) and height
(+0x32). The game takes the road's world centre and height at the road point
behind the vehicle and at the next one (a table of 4-byte entries through
the pointer at `0xFF1CE8`), blends them by the fraction of the vehicle's
position, adds the vehicle's own sideways position and height, and scales
the result by the perspective table at the vehicle's own distance. Its size
is 0x4000 × 0x280 ÷ (distance + 0x200) (0x300 instead of 0x280 for one class
of vehicle, by the byte at +0x74). So a vehicle's depth is continuous, not
one of 32 steps. Which picture a vehicle shows (`0x1641A`, `0x165B2`,
`0x166D0`) is animation logic and **was not traced**. Some records in the
vehicle part of the table are made by other code (`0x1612A`, `0x15BB0`,
`0x15AE2`, `0x15C68`: linked objects and extras); **not traced**.

### 3.3 Drawing the objects (read from the code; tested in section 4.4)

`0xA4EE` walks the order list and, for each record, `0xA5C8` draws the
picture at the record's size into the sprite-picture buffer and adds the
sprites that show it to the sprite list (`0x7A68`).

**Picture data.** Not compressed. A picture is a width byte, a height byte,
one word (a colour-set word that the game always replaces with the record's
own), then the rows. Each row is ⌈width ÷ 2⌉ + 1 bytes; each byte is two
pixels of four bits each, a colour number 0 to 15 within the colour set, 0
being see-through. Pictures used with flag bit 7 have two more bytes in
front: the anchor point's distance from the picture's left edge and from its
top.

**Scaling** (`0xA21C` across, `0xA2CA` down, copied by `0xA3CA`). The size
is added to a running total once per source step; each time the total passes
1.0 one output step is produced. So shrinking drops source steps and
enlarging repeats them (nearest-neighbour scaling, no blending). Down the
picture a step is one row. Across it a step is one **byte, that is a pair of
pixels**: objects are scaled sideways in two-pixel units.

**Placement.** Without an anchor (roadside objects): the picture is centred
on the record's x and stands on its foot line: left = x − (width × size) ÷
2, top = foot − height × size. With an anchor (vehicles): left = x − anchor
x × size, top = foot − anchor y × size, and mirrored pictures measure the
anchor from the right edge. A record with a second picture draws both from
the same anchor, the first one skipping the rows the second covers.

**Clipping.** Columns outside the view's left and right edges are dropped in
whole pixel pairs, rows above the view are skipped, and rows at or below the
record's hidden-below line are not drawn (this is how a hill hides the lower
part of an object). A picture entirely outside is dropped.

**Cutting into sprites** (`0x7A68`). The scaled picture's box is rounded up
to whole 8-pixel tiles and cut into sprites of at most 32×32 pixels, left to
right (right to left when mirrored, with the sprites flipped), then top to
bottom. Tiles are numbered consecutively from the first tile of the buffer
being built (`0xFFCE86`: 0x634 or 0x434). Sprites above screen line 152 get
high priority. If the 80 sprites or the 336 tiles of the buffer run out, the
whole object is dropped.

**Copies.** A record can ask for up to seven extra copies (flag bits 8–10).
The game does not draw the picture again: it duplicates the sprite entries
just made, each copy shifted sideways by the record's +0xC (plus a small
irregular amount from a table at ROM `0x16E2C` when bit 5 is set), and skips
copies that fall off the screen. This is how a row of identical objects
costs one drawing.

This is what section 1's hypothesis looks like in the code: the game holds
one picture of each object and scales it in software for every object in
every pass; the video chip cannot scale, so the scaled result is cut into
sprites.

**Centre-line dashes** (`0x13F20`; read, not tested). They are not pictures.
The routine works from two per-line arrays (start x at `0xFF2EDE`, width at
`0xFF309E`), takes the lines eight at a time and builds a small sprite for
each band from the leftmost start and widest extent in it. Not traced
further.

### 3.4 What moves between passes (verified on passes 66–80 and 500–520)

Nothing. The road shifts and sprite list are sent once per pass, and the
values the frame and line interrupts write (the six line-interrupt records
of section 2) are identical in every frame of a pass: 0 of 34 passes showed
any difference between frames. So for the 3 to 9 frames a pass lasts, the
picture is completely still. The earlier hypothesis that the frame interrupt
moves the road every frame (`0x16CE`/`0x179E`, in the design document) is
**not supported** for the picture.

For interpolation this is the simple case: there is no partial smoothing in
the original to preserve or fight.

### 3.5 Hand-over, and why a faster CPU glitches

The game runs as two strands. The **main loop** does all the work of a pass
(draw, physics, project the road) and builds its picture in the spare set of
buffers. The **frame interrupt**, 60 times a second, checks whether a
finished picture is waiting; if so it sends the four blocks (section 2) and
counts the pass (this is the "handoff" the conformance work compares at,
`0xDF48`; it runs inside the frame interrupt, verified from the MAME trace:
interrupt level 6, while the drawing and the projection run at level 3).
The main loop does not stop for this, so a handoff can arrive while it is
already part-way into its next piece of work. Section 4 measures which
frames show which pass.

**What a faster CPU does to the line interrupts (verified at 800%).** Each
line interrupt writes three things one after another: plane A's shift, plane
B's shift, the backdrop colour. At original speed all three land within the
same screen line in every one of 1,276 line interrupts logged. At 800% the
handler starts and finishes at different places along the line, and in 390
of 1,200 line interrupts logged the shifts landed on one line and the colour
on the next. In 118 of those 390 both the shift and the colour were changing
(the horizon and the top of the dashboard), so for that one line the picture
is drawn with the new shift and the old colour; in the others one of the two
was not changing and nothing would show.

**Hypothesis:** this is the stray one-line stripe in the owner's screenshots
(`analysis/smoothness-experiment.md`). It fits: a one-pixel-high line of the
wrong thing across the screen, at a strip boundary. It has not been shown by
producing such a picture at 800% and finding the stripe in it.

**Hypothesis** for the larger glitches in those screenshots (the far road
detached to one side, the whole picture displaced for a frame): the same
kind of race between a line interrupt and the code that swaps the two sets
of buffers. Not reproduced or measured.

### 3.6 Dashboard and mirrors

The dashboard is the window layer, re-sent whole every pass from a staged
copy (section 2); the mirrors are plane B showing through it. They are
separable from the 3D view: nothing above line 152 depends on them. Not
traced further.

## 4. The proof: predicting the game's own tables

`src/rr2port/scene.py` reads a scene from a work-RAM image alone and repeats
the game's road arithmetic. `rr2-scene-check` compares that with what the
game itself produced, for exact equality, over the work-RAM images taken at
every one of the 2,176 passes of `full-throttle`
(`generated/port/handoffs/full-throttle.bin`):

```sh
uv run rr2-scene-check --rom "Road Rash II (USA, Europe) (RR206).gen" \
  --handoffs generated/port/handoffs/full-throttle.bin \
  --video-state generated/port/draw/f4000.vdp
```

### 4.1 Track and camera to the 32 road points (verified)

Projecting the road from the track ring, the camera's place, the player's
sideways position and the camera height, with the ROM's perspective table,
gives **exactly the game's five arrays (160 numbers) in 2,155 of 2,176
images**.

Of the other 21, 13 are images taken while the main loop was in the middle
of projecting (section 3.5): the arrays equal this image's projection up to
some point and the previous image's projection after it. The remaining 8
are not explained (**hypothesis:** the same thing, where the previous image
was itself caught part-way, or where the track ring was being reloaded, as
it was in 3 of them).

### 4.2 Road points to the per-line shifts sent to the video chip (verified)

Each image holds two staged shift tables (the two buffers). For each, the
lines from the horizon to the bottom of the 3D view were compared with the
table predicted from the road points of this image and of the three before
it:

| Outcome | Tables | Share |
|---|---|---|
| Equal on every line to one prediction | 3,873 | 89.0% |
| Caught half-written: lower lines equal a newer prediction, all other lines an older one | 364 | 8.4% |
| Not explained | 115 | 2.6% |
| Total (2 tables × 2,176 images) | 4,352 | |

Of the tables equal to one prediction, 1,449 were made from the road points
in the same image, 2,081 from the image before and 343 from two before.

So the chain from the track bytes to the numbers the video chip receives is
reproduced exactly wherever an image shows a finished table. The 115 not
explained differ from the nearest prediction on 1 to 3 lines in 40 cases
and on more in the rest; **hypothesis:** they are tables half-written from a
projection that no image caught. A cleaner measurement would take the image
at the moment the main loop finishes the table instead of at the frame
interrupt; the handoff images were what existed.

**Re-measured with clean images (verified).** `rr-race-replay --ram-at-pc
14614:0:2176:FILE` takes an image each time the main loop reaches the
instruction at `0x14614`, where the routine that fills the shift table
(`0x14822`) has just returned, so every image shows finished work. The main
loop got there 2,175 times in the race (the replay still ends in `RESULT
match`). On those images:

| Check | Result |
|---|---|
| Road projection equals the game's five arrays | 2,175 of 2,175 |
| Staged shift tables equal to one prediction | 4,350 of 4,350 (2,221 from the road points in the same image, 2,129 from the image before) |
| Half-written or unexplained | 0 |

So the hypothesis above is confirmed: every earlier mismatch came from when
the image was taken, not from the reading. The chain from the track bytes to
the shifts sent to the video chip is reproduced exactly on every pass of
this race. (The two tables per image are the two buffers: one just filled
from this image's road, the other filled a pass earlier.)

Lines above the horizon (the cloud and mountain strips) are written by other
code and are **not predicted**.

### 4.3 Which pass is on screen (verified on 12 frames)

For the twelve frames of section 1, the shift table in video memory at the
end of the frame equals a staged table in the image of the pass that started
in that frame (all twelve; two frames also match the next image, where the
table had not changed). And at each handoff, the finished table (the one the
main loop is not building, by the pointer at `0xFF279A`) equals only the
prediction from the image before in 2,070 of 2,176 images. So the road on
screen while a pass runs is usually the road projected one pass earlier
(about 5 frames, a twelfth of a second, behind the physics). That the
pairing of frames and passes holds outside the twelve frames is inferred
from these two measurements, not measured frame by frame.

### 4.4 Objects (verified, except vehicle pictures and some vehicle records)

`src/rr2port/objects.py` repeats the game's object drawing (section 3.3)
from a work-RAM image taken just before the game does it, and
`rr2-object-check` compares the result with an image taken just after, for
equality. The images are taken with `--ram-at-pc` at `0xA4EE` and `0xA5C6`
(drawing) and at `0x16E8C` and `0x17196` (records), over the whole race:

```sh
B=generated/port/build/rr-race-replay; H=generated/port/handoffs/full-throttle
D=generated/port/draw; ROM="Road Rash II (USA, Europe) (RR206).gen"
$B --rom "$ROM" --handoffs $H.bin --timesteps $H.timesteps --pads $H.pads \
   --ram-at-pc A4EE:0:2176:$D/before-draw.bin --ram-at-pc A5C6:0:2176:$D/after-draw.bin \
   --ram-at-pc 16E8C:0:2176:$D/before-roadside.bin --ram-at-pc 17196:0:2176:$D/after-roadside.bin
uv run rr2-object-check --rom "$ROM" --before $D/before-draw.bin --after $D/after-draw.bin \
   --records-before $D/before-roadside.bin --records-after $D/after-roadside.bin
```

| Check | Result |
|---|---|
| Sprite list (every sprite's position, size, tile, colour set, priority, mirror, order) | identical on 2,174 of 2,174 passes |
| Sprite pictures (all 10,752 bytes of the tile buffer) | identical on 2,174 of 2,174 passes |
| Roadside records: x, foot line, size, hidden-below line, offset, distance | identical for 28,603 of 28,603 records |
| Records in the vehicle part of the table: x, foot line, size, hidden-below line | identical for 8,062 of 11,154; 73 more identical except the low two bits of the foot line |

The 2,174 passes drew 40,093 records. Among them: 7,764 mirrored, 18,724
with an anchor point, 12,038 two-part, 833 clipped at a side of the view,
25,485 cut off below by a hill line, 12,999 with copies, 196 dropped whole;
colour sets 0, 1 and 2 were used (3,328, 28,922 and 7,843 records), set 3
never. So each of those cases is covered by the equality above, except
colour set 3.

What this verifies: the picture format, the scaling, the placement, the
clipping, the cutting into sprites and the copies (section 3.3), and the
roadside placement rule (section 3.2). The earlier 58% came from reading the
picture header wrongly (there are two layouts) and from not knowing about
the copies.

What it does not: 3,019 records in the vehicle part of the table (27%) are
not placed by the vehicle rule of section 3.2. **Hypothesis:** they are the
records made by the untraced routines listed there (objects linked to a
vehicle, and extras), which have their own placement code (`0x1612A`). The
73 with different low bits fit the code that ORs two bits of the vehicle's
state into the foot line (`0x1590C`); what those bits are for is not traced.
Which records exist at all (the walk through the roadside list, the choice
of a vehicle's picture) is not predicted; only where an existing record is
placed.

**The art.** `rr2-extract-art` writes every picture in the ROM's table as a
PNG at natural size (674 pictures) with one sheet of them all, under
`generated/port/art/`. Bikes, cars, police, riders on foot and fallen, and
roadside objects are all recognisable on the sheet, which is a check of the
picture format by eye. Whether a picture has an anchor is told from its
length, and the colours are those of one race frame with the picture's own
colour set, so some pictures are not in the colours the game shows them in.

### 4.5 What was not attempted

- Redrawing the 3D view pixel for pixel from the scene. With 4.2 and 4.4
  exact and the block copies of section 2 verified, it would add nothing.
- The choice of a vehicle's picture; the records made for objects linked to
  vehicles; the centre-line dashes; the cloud and mountain strips; the
  dashboard and mirrors.
- Any race but `full-throttle`: no crash, police or two-player picture.

## 5. The crisp road: a first renderer step (verified as stated below)

What is verified by measurement: the enlarged picture still contains the
original exactly, the game is unchanged, and how often the crisp road is
drawn. That the rows in between form the intended slope follows from how
they are computed and was looked at, not measured.

Plan: `docs/superpowers/plans/2026-11-11-objects-and-crisp-road.md`.

**What it is.** The picture is drawn enlarged (each original line as S rows
of S times as many pixels). On the lines that show the road, the two grass
planes are given a sideways shift for each *row* instead of each line, worked
out from the game's own 32 road points by continuing the same straight runs
the game draws between them (`port/render/rr_road.c`, following section
3.1). A road edge that the original draws as a staircase (a whole pixel
sideways every few lines) becomes a slope at the enlarged picture's
resolution. Nothing else is changed: sky, objects, centre-line dashes and
dashboard are the original, each pixel repeated. The game is not touched;
the video chip model draws a second, larger picture beside the original one.

**Which road.** The road on screen is one the game projected a pass or two
earlier (section 4.3). The road points are noted each time the main loop
reaches `0x14566`, where it starts turning them into the picture, and the
last four are kept. Before each frame is drawn, the newest noted set whose
predicted per-line shifts equal the shift table actually in video memory is
used. If none does, that frame is drawn as the plain enlargement. And as
each line is drawn, its shifts are used only if their whole part is still
that line's value in the table at that moment, so if the game rewrites the
table part-way down a frame the lines after it are drawn plainly. A road
that is not on screen is never drawn.

**The check.** The game uses only the whole part of each line's edge
position. So the enlarged picture must still contain the original exactly:
the last of each original pixel's S pixels, on the first of its S rows.
`rr-race-replay --scaled-check S` compares that for every frame:

```sh
$B --rom "$ROM" --handoffs $H.bin --timesteps $H.timesteps --pads $H.pads --scaled-check 4
```

| Measured on `full-throttle`, 12,843 frames | Scale 2 | Scale 4 | Scale 8 |
|---|---|---|---|
| Frames that still hold the original exactly | 12,843 | 12,843 | 12,843 |
| Race frames drawn with the crisp road | 11,823 of 11,824 | 11,823 of 11,824 | 11,823 of 11,824 |
| Replay result with the option on | match | `RESULT match` (2,176 of 2,176) | match |
| Processor time per frame, whole run (1.3 ms without the option) | 3.3 ms | 6.3 ms | 17.3 ms |

The one race frame without the crisp road is the first, before any road
points have been noted. Scale 8 costs more than a frame's 16.7 ms on the
machine used. The Godot window picks the scale from its own size, at most
5, so that every row of the enlarged picture covers the same number of
screen pixels: 3 in the default 960×672 window.

**What it looks like** (frames 1518, 4000 and 10261, the enlarged picture
beside the plain enlargement; looked at, not measured). Near the camera the
road edges and the painted edge lines are clean slopes. Two things are
visible that were not asked for:

- *Far away the edge still wobbles.* The game's road points sit on whole
  pixels, so the straight runs between them change direction by small
  rounding amounts from point to point. Projecting the road with fractions
  (the contract's form 2a) would remove this; it was left to the
  interpolation plan.
- *The grass speckles lean.* The grass pattern is part of the same planes,
  so it is shifted row by row with the edge. Near the camera, where the
  shift changes by several pixels a line, the pattern's dots become short
  slanted strokes. The pattern is still the original at original size, but
  it does not look the same. Whether that is acceptable is for the owner to
  judge by eye.

## 6. Fine road points (verified as stated below)

Plan: `docs/superpowers/plans/2026-11-11-fine-road-points.md`.

**What it is.** The game places each of its 32 road points by multiplying a
world position by a scale and dropping the remainder, so the points sit on
whole pixels and whole lines; that is the far-edge wobble of section 5. The
port now repeats the game's projection itself (`rr_road_project` in
`port/render/rr_road.c`, a C version of the arithmetic of section 4.1) from
the camera and track values as they stand when the game starts projecting
(`0x14A86`), and keeps the remainders. The crisp road's edges are then drawn
through these fine points.

**How it is known to be the same road.** A set of noted camera and track
values is attached to a road only if projecting it, with the remainders
dropped as the game drops them, gives exactly that road's 32 points as the
game left them in memory. A road without such a set is drawn from the
whole-pixel points as in section 5. Which road is on screen, and the check
on each line as it is drawn, are still made on the whole-pixel values
against video memory, exactly as before.

**Measured** on `full-throttle`, scale 4 (`rr-race-replay --scaled-check 4`,
with `--crisp-points whole` for the first column):

| | Whole-pixel points | Fine points |
|---|---|---|
| Replay result with the option on | `RESULT match` | `RESULT match` |
| Race frames drawn with the crisp road | 11,823 of 11,824 | 11,823 of 11,824 |
| Of those, drawn from fine points | 0 | 11,823 (all) |
| Frames in which every line without road is the original, enlarged | 12,843 of 12,843 | 12,843 of 12,843 |
| Frames that hold the original exactly on the road lines too (section 5's check) | 12,843 | 1,020 (the frames with no road) |

So the port's projection reproduced the game's own road points on every
crisp frame of the race (a second implementation of section 4.1's
arithmetic, in C, checked against the game's memory; it is a port of the
Python one, not an independent derivation), and nothing outside the road
lines changed. On the road
lines the picture is, by design, no longer the original pixel for pixel.

**How far the fine edge is from the game's.** Compared on the first row of
each line, for both edges, over all fine frames (2,010,832 line edges):

| Difference from the game's whole-pixel edge | Line edges | Share |
|---|---|---|
| under 1 pixel | 1,266,226 | 63.0% |
| 2 pixels or more | 210,848 | 10.5% |
| 4 pixels or more | 28,844 | 1.4% |
| 8 pixels or more | 3,223 | 0.16% |
| largest | 106.9 pixels | |

Some difference is expected: the game drops up to a pixel from each of the
two products that make an edge, and up to a line from each point's screen
line. **Hypothesis**, not checked line by line: that this accounts for the
differences under 4 pixels.

Every line edge 4 pixels or more away (28,844, in `--edge-log FILE
--edge-log-over 4`) was classified (**verified** by that log):

| Class | 4 px or more | 8 or more | 16 or more | 32 or more | Largest |
|---|---|---|---|---|---|
| **Between two lines of the game's road**: the fine edge lies between the game's edge on that line and on the line above or below (to within 2 pixels) | 21,136 | 2,887 | 1,232 | 657 | 106.9 px |
| **On the extended near run** (below the game's second road point) | 7,660 | 315 | 26 | 0 | 22.0 px |
| On the horizon line, where there is no line above to compare with | 37 | 10 | 5 | 5 | 47.7 px |
| Outside all of these | 11 | 11 | 11 | 5 | 40.0 px |

- **Between two lines** is where the game's own edge leaps many pixels from
  one line to the next: sharp bends far away, and hill crests, where the
  edge jumps from the road in front to the road behind. The game's edge
  there is one line early or late (it dropped the remainder of the point's
  line); the fine edge makes the same move part-way through the line. So
  the difference is large in pixels but under one line in position. Edges
  8 pixels or more away occur in 1,404 of the 11,823 fine frames (12%),
  nearly all of this kind.
- **The extended near run.** The game does not draw the road between its
  first and second points: from the second point down it extends the run
  between the second and third points to the bottom of the view (lines 115
  to 151 in this log; the game's value there equals that extended run to
  within 0.07 pixel). Extending a short run a long way magnifies the two
  points' dropped remainders. The fine road extends the same run from the
  fine points. If the two points are not one above the other at all, the
  fine road leaves those lines to the whole-pixel road.
- **The 11 outside** are all on line 76 (the horizon line of those frames),
  in 11 frames, 2 pixels or so beyond the neighbouring line's value. **Not
  explained.**

The earlier, narrower log (lines outside "two pixels plus the edge's
movement over one line": 3,691) could not see the first class at all,
because at a leap the movement over one line is the leap.

**What this means for the picture.** Looked at, not measured, on frames
1518, 4000 and 10261 in both modes: the far and middle road edges are clean
curves where the whole-pixel road had small kinks. That the wobble is
removed is **verified only on a synthetic straight road** (the test
`test_a_straight_road_has_straight_edges`: fine edges straight to within a
quarter of a pixel, whole-pixel ones not); on the race it is by eye. From
the measurements above:

- On about one fine frame in eight, at a sharp far bend or a crest, the
  rows of one line show the edge well away from where the original has it
  on that line (it is where the original has it on the next line). In
  motion this should read as the edge moving smoothly through the line; it
  has not been watched for.
- Near the camera the edge can sit a few pixels, and in rare frames up to
  22, from where the original draws it. **Hypothesis:** the fine edge is
  the steadier of the two from pass to pass, since the original's is its
  rounding magnified; not measured.
- The riders, the centre-line dashes and everything else are still placed
  by the game on its own whole-pixel road, so the dashes and the edges are
  not guaranteed to agree to the pixel. No disagreement was noticeable in
  the three frames.
- The leaning grass speckles are unchanged.

In the Godot window F switches between fine and whole-pixel points while
the crisp road is on, for comparing by eye.

**Limits.** One recorded race at original speed. The camera's place within
a segment is still used with the game's 8-bit precision, so the road still
moves between passes in the game's own steps; that belongs with
interpolation.

## 7. Fine objects (verified as stated below)

Plan: `docs/superpowers/plans/2026-11-11-fine-objects.md`.

**What it is.** The game shrinks or enlarges each object's picture to the
size it needs at 320×224 and cuts the result into sprites (section 3.3). At
a distance that throws most of the picture away: a tree 24 pixels wide
drawn 4 pixels wide keeps one pixel in six of each row, and thin parts (the
top of a tree) can vanish altogether. In the enlarged picture each screen
pixel is several output pixels, so there is room for more of the picture
than the game kept. The port now draws each object sprite's output pixels
straight from the picture in the ROM.

Two parts:

1. **The game's drawing repeated in C** (`rr_objects_draw` in
   `port/render/rr_objects.c`, a routine-for-routine version of the Python
   of section 4.4). Besides the sprite list and the sprite pictures it
   keeps what the game throws away: for every sprite, which picture it
   shows, at what size, where the picture's first column and row stand on
   screen, whether it is mirrored, and what was cut off at the view's edges
   or by a hill.
2. **Sprites drawn from the picture** (`rr_vdp_render_line_fine` in
   `port/hw/rr_vdp_render.c`). The video chip model still walks the sprite
   list as before, so the order of sprites, their priority against the
   planes, their colour sets and the per-line limits are the original's.
   Only where a sprite's pixels come from changes: output pixel by output
   pixel from the ROM picture, at the picture's scale, instead of from the
   sprite's tiles. A sprite still covers only its own rectangle.

**How it is known to be the same objects.** Three checks, all for equality:

- The C drawing against the Python reference on 150 random synthetic
  scenes (`tests/port/test_objects_c.py`): all of work RAM equal.
- The C drawing against the game, every time the game draws
  (`rr-race-replay --objects-check`, which runs the C version at `0xA4EE`
  and compares at `0xA5C6`): the sprite list, the sprite-picture buffer
  (10,752 bytes) and both counters.
- At run time a description is only used for a frame if every one of its
  sprite entries (all bytes but the link to the next sprite, which the game
  changes when it adds the centre-line dashes) and every one of its tiles
  is in video memory byte for byte at the start of the frame; the last four
  passes' descriptions are kept and the newest that is there is used. If
  video memory changes while the frame is being drawn, the lines after that
  are drawn from the tiles.

**Measured** on `full-throttle`:

```sh
B=generated/port/build/rr-race-replay; H=generated/port/handoffs/full-throttle
ROM="Road Rash II (USA, Europe) (RR206).gen"
$B --rom "$ROM" --handoffs $H.bin --timesteps $H.timesteps --pads $H.pads --objects-check
$B --rom "$ROM" --handoffs $H.bin --timesteps $H.timesteps --pads $H.pads \
   --scaled-check 3 --crisp-points none --fine-objects
```

| Check | Result |
|---|---|
| Replay result with the options on | `RESULT match` |
| The C drawing equals the game's | 2,179 of 2,179 times the game drew (40,223 draw records, 196 dropped by the game, 43,668 pictures described) |
| Frames that began with a description noted | 11,878 |
| ... of which one was in video memory, so the objects were drawn fine | 11,861 |
| Lines that fell back because video memory changed during a frame | 0 |
| Object sprites on those frames; drawn from a picture | 395,559; 395,517 |
| Frames in which every output pixel outside those sprites' rectangles is the original pixel repeated (no road shifts, `--crisp-points none`) | 12,843 of 12,843 |
| The same with the fine road on: lines without road, and the road's sample-back, outside those rectangles | 12,843 of 12,843 and 1,020, as before (section 6) |
| Screen pixels inside those rectangles whose centre output pixel has the original's colour | 64,240,400 of 75,895,024 (84.6%) |

Cost, measured with the replay tool at scale 3 on the development machine
(a whole frame of game and picture; a frame at 60 a second has 16.7 ms):
1.4 ms with the plain picture only, 4.9 ms with the fine road, 6.1 ms with
the fine road and fine objects.

The last row of the table is a description, not a pass mark. The fine picture is not the
game's sprite enlarged, on purpose:

- The game scales sideways in **pixel pairs** and starts each picture's
  scaling from a fixed phase; the fine picture gives every picture pixel
  the same room. So a sprite pixel and the centre of the fine picture there
  can show neighbouring picture pixels.
- A shrunken sprite shows one picture pixel per screen pixel; the fine one
  shows up to scale × scale of them, so its centre is often a pixel the
  game dropped.
- The game loses up to two pixels of a picture's far side to pair rounding
  (and stops four pixels short of the screen's right edge); the fine
  picture runs to its true scaled width.
- Two oddities of the game's scaler are not copied: the first row of a
  picture whose top is cut off (by the top of the view, or by the second
  half of a two-part object) is the picture's row 0, not the row that
  belongs there, and likewise the first pixel pair of a picture cut at the
  left. The fine picture shows the row and pair that belong.

What *is* tied down exactly: at natural size the fine picture equals the
game's sprites enlarged, pixel for pixel, for plain, mirrored, copied,
two-part and hill-cut objects and for objects cut at the left or the top of
the view (the test `test_natural_size_fine_picture_is_the_plain_enlargement`,
apart from the two oddities). An object cut at the right of the view agrees
wherever the game drew and differs only in the last four screen columns,
where the game draws nothing and the fine picture runs on
(`test_a_picture_cut_at_the_right_runs_on_to_the_screen_edge`). These tests
are what show that the position, mirror, cut and size the C drawing reports
mean what the renderer takes them to mean. That the sprite order, the
priority against the planes and the per-line limits are the original's
inside the sprites' rectangles rests on unit tests of the renderer, not on
the race.

**At other CPU speeds** (the reviewer's measurement, replay tool at 200%,
400% and 800%): no line fell back, and fine frames were 5,060 of 5,068,
4,485 of 4,489 and 4,484 of 4,486 frames with a description noted. The set
on screen was at most two passes old, of the four kept.

**What it looks like** (by eye, frames 1518, 4000 and 8278, scale 3;
pictures under `generated/port/draw/`, for example
`f4000-objects-zoom.png`): distant trees are recognisable trees with their
tops, where the original has a blob of a few pixels; distant riders show a
bike and a rider; a small roadside animal that was a brown speck is an
animal. Some far objects that the original reduces to one or two pixels are
now plainly visible, so the far scene is busier than the original's. Near
objects (the player's bike) are almost unchanged, with slightly more even
pixel sizes.

In the Godot window O switches the fine objects off and on while the crisp
road is showing, for comparing by eye.

**Softer far objects** (added 2026-11-13; B in the Godot window, off by
default; `--soft-objects` in the replay tool). The owner found the far
objects "almost too crisp". With the softening on, a picture drawn below
its natural size takes for each output pixel the mean colour of the picture
pixels that pixel covers (see-through ones left out; a pixel less than half
covered stays see-through) instead of one of them. Pictures at natural size
or larger are untouched. A smaller picture is untouched only where every
output pixel covers exactly one picture pixel (half size at scale 2, for
instance); at other ratios neighbouring pixels are mixed even when the
picture is only a little below natural size, so there is a step in look
between a picture just under natural size and one at it. **Verified** on the
race with it on: `RESULT match`, every frame still identical outside the
object sprites (12,843 of 12,843), 6.3 ms a frame against 6.1. **By eye**
(frame 4000, `generated/port/draw/soft-compare.png`): far trees lose their
single-pixel speckle and far riders become small smooth shapes. The mean
colours are not among the game's 64, which is a step away from the
original's pixel art: whether it is wanted is the owner's call.

**Not done, and limits.**

- Objects still stand on the game's whole pixels and move once per pass.
  Placing them between pixels comes with interpolation.
- The pictures are the ROM's, at their own resolution: an object drawn
  larger than natural size is still blocky. Smoother enlarged art is the
  next stage.
- The centre-line dashes are not objects of this kind and are still the
  game's sprites.
- The 17 frames with a description noted but none in video memory were not
  looked at one by one. **Hypothesis:** they are the frames before the
  first block copy of the race.
- Two-player races: the C drawing assumes the one-player view; a pass it
  does not reproduce is simply drawn from the tiles. Not tested on a
  two-player race.
- One recorded race at original speed.

## 8. In-between pictures (verified as stated below)

Design: `docs/superpowers/specs/2026-11-12-interpolation-design.md`. Plan:
`docs/superpowers/plans/2026-11-12-inbetween-pictures.md`.

**What it is.** The game makes a new picture about 11 times a second and
the picture stands still in between (3.4). The port now keeps the last few
updates as they appear on screen, each stamped with the video frame it
first showed in, and for every frame describes the scene at a moment
between two of them, or a little past the newest
(`port/render/rr_between.c`):

- **the road** is projected again (section 6's projection) from a camera
  whose place on the track, sideways position and height are part-way
  between the two updates';
- **each object that can be followed** from one update to the next (a
  roadside object by its track segment, side, picture and roadside byte; a
  vehicle by the entry of the game's vehicle list whose distance its record
  has) has its draw record taken part-way: position in a straight line, size
  through 1/size. The game's object drawing (section 7's) is then run on
  those records, and the fraction of a pixel left over moves the picture
  within the enlarged frame;
- **an object that has just left the view** goes on as it was moving in the
  update before; **the sky and hills** take their sideways shift part-way;
- everything else (poses, the centre-line dashes, the dashboard, records
  that cannot be followed) changes when the game changes it.

Three modes differ only in how far behind the present the moment drawn is:
**predict** (not at all: the newest update's motion is carried forward),
**blend** (the average length of recent updates) and **late blend** (the
longest of them). The game is not touched: this only reads memory.

**Measured** on `full-throttle`, scale 3 (`rr-race-replay --scaled-check 3
--fine-objects --inbetween MODE --inbetween-check`). All runs at original
speed end in `RESULT match`, 2,176 of 2,176 passes; a run under the address
and undefined-behaviour sanitizers (predict) reported nothing.

*Frames.* Of 12,842 frames, 1,020 are not race frames with two updates kept
(menus, the first update).

| | Predict | Blend | Late blend |
|---|---|---|---|
| Frames drawn in between | 11,749 | 11,742 | 11,737 |
| of those, past the newest update (a guess) | 9,579 | 137 (1.2%) | 21 |
| Fell back: the objects could not be made (see below) | 68 | 70 | 72 |
| Fell back: more than 80 sprites | 5 | 10 | 13 |
| Fell back: camera jump | 0 | 0 | 0 |
| Frames on which an earlier update was on screen again (ignored) | 13 | 13 | 13 |
| Lines drawn from the game's sprites because video memory changed mid-frame | 0 | 0 | 0 |
| Time per frame, game and checks included | 8.5 ms | 8.5 ms | 8.4 ms |

"The objects could not be made" is one count for three causes: records that
could not be read, the game's drawing refusing the records, or a sprite
entry with no full picture. Which of them these frames are was not
measured; **hypothesis:** the last, since section 7 found 42 such entries
in this race. A line whose sideways shift in video memory changed during
the frame is drawn plainly too, as before, but is not counted anywhere.

*The ends are the game's own picture.* For each of the 2,174 pairs of
neighbouring updates the scene was made at the older one (t = 0) and at the
newer (t = 1) and compared with what sections 6 and 7 draw for that update:
every row of every road line, every object sprite entry and picture, and at
the newer one the whole sprite table against video memory's own chain. (The
three comparisons are tested to report a difference when there is one:
`test_the_comparisons_report_a_difference`.)

| | Pairs |
|---|---|
| Made at both ends, and equal in every respect compared | 2,165 |
| Not made: the objects could not be made (4 updates, each in 2 pairs) | 8 |
| Not made: more than 80 sprites | 1 |
| Made but different | 0 |

So wherever the in-between scene exists, at an update it *is* that update's
scene. (For the objects this holds by construction; for the road it is a
measurement: the road projected from the older camera with the newer
update's track data equalled the older update's road every time.)

*Skip one.* The only comparison of an in-between picture with something the
game drew: for updates k−1, k, k+1 the scene is made from k−1 and k+1 at
the moment of k and compared with the real k (2,159 triples). The gap
bridged is two updates, twice what is bridged in play.

| Distance from the real update, pixels of 320 | Median | 90% within | 99% within |
|---|---|---|---|
| Left road edge (lines 100, 115, 130, 145; 8,577 samples) | 0.86 | 3.50 | 12.04 |
| Followed objects' x (28,649 samples) | 0.54 | 2.82 | 16.27 |

**Hypothesis**, not measured: over the single update bridged in play the
error is well under half of this, since it comes from the motion bending
(steering, bends in the road) and that grows faster than the gap.

*Does it move every frame?* Counted while the camera was moving, from the
scene handed to the renderer (the road's shift on line 130 and the draw
record's x with its fraction), not from the pixels drawn: frames in which
the left road edge was where the frame before had it (to 1/16 pixel), and
the same for the largest followed roadside object.

| | Today's picture | Predict | Blend | Late blend |
|---|---|---|---|---|
| Edge still / frames | 9,504 / 11,585 (82%) | 650 / 11,463 (5.7%) | 590 / 11,460 | 564 / 11,446 |
| Object still / frames | not counted | 900 / 10,595 | 686 / 10,596 | 722 / 10,583 |

**Hypothesis** for the still frames that remain: on a straight with no
steering the edge does not move on screen however fast the camera goes, and
an object that jumps too far to be followed stands still by rule. Not
checked frame by frame. The largest single-frame step of the edge was 111
pixels (120 in blend). What those frames are was not looked at;
**hypothesis:** a sharp bend arriving, where the edge on one line swings
far in one update. It is larger than any guess measured below, so it is
not explained by a correction.

*How wrong is a guess?* When an update arrives, the last frame drawn past
the one before is compared with it.

| | Updates | Edge: median / 90% / 99% | Objects: median / 90% / 99% |
|---|---|---|---|
| Predict | 2,155 | 1.71 / 7.01 / 23.47 | 1.17 / 5.50 / 18.00 |
| Blend | 116 | 4.13 / 15.28 / 45.80 | 2.96 / 13.86 / 40.75 |
| Late blend | 5 | too few to say | too few to say |

So predict's last frame before each update is typically 1 to 2 pixels from
where things turn out to be, and one time in ten 5 to 7 pixels: that is the
size of the correction seen when the update arrives. Blend guesses rarely,
but when it does (an update that ran long) it is further off.

*At 200% CPU speed* (as in `analysis/smoothness-experiment.md`: the same
buttons by frame; the race is not the same race, so `RESULT` does not
apply). 5,343 updates were kept.

| | Predict | Blend |
|---|---|---|
| Frames drawn in between | 11,821 | 11,820 |
| of those, a guess | 6,478 | 142 |
| Pairs made and equal at both ends | 5,343 of 5,343 | 5,343 of 5,343 |
| Frames on which an earlier update was on screen again (ignored) | 247 | 247 |
| Skip one, edge: median / 90% / 99% | 0.00 / 1.15 / 7.54 | the same |
| Skip one, objects | 0.50 / 1.00 / 2.00 | the same |
| Guess, edge | 0.70 / 4.61 / 19.68 | 73 updates only |
| Guess, objects | 0.50 / 2.00 / 5.00 | |
| Edge still / frames (today: 7,384 / 10,702) | 3,708 / 10,700 | 3,606 / 10,713 |
| Largest single-frame step of the edge | 204 pixels | 204 pixels |
| Time per frame | 8.6 ms | 8.6 ms |

With updates twice as often every error is smaller, as expected. That a
third of the frames still count as "still" at 200% is **not explained**
(**hypothesis:** this run spends long stretches slow or stopped after its
crashes, where the motion per frame is under the 1/16 pixel threshold), and
neither is the 204-pixel step.

*An earlier update on screen again* (**verified**, found by the final
review): on 13 frames at original speed and 247 at 200%, the newest object
set in video memory was one from before the newest update kept. Before the
review's fix each such frame was kept as a new update, which paired a newer
update with an older one (5,580 "updates" for 5,392 passes at 200%); now
such a frame is counted and the history left alone. Why video memory shows
an earlier set is not known (**hypothesis:** the newest set's tiles are
part-copied for a frame, so the chooser of section 7 falls back to the one
before).

**The records that cannot be followed** (a bounded look). 3,227 of 40,093
draw records (8.0%) are neither roadside objects nor tied to a listed
vehicle by distance; 2,026 of them show pictures 501 and 502, which
alternate on the same object. A key of picture and flags, where unique,
followed only 48.7% of them from one update to the next, so they are left
to change when the game changes them. They stand at lines 201 to 212, below
the 3D view, with one copy 178 to 294 pixels to the right, and are made by
code at about `0x124F0`–`0x12616` from a vehicle object. **Hypothesis:**
they are the riders behind, as the mirrors show them. Not traced further.

**What it looks like** (by eye: frames 4000 to 4007 in blend beside the
same frames without, `generated/port/draw/between-blend-sheet.png` and
`between-off-sheet.png`). Without, six of the eight frames are the same
picture. In blend the road's bend, the trees and the rider ahead are in a
slightly different place in every frame, and nothing is doubled or torn.
The player's bike changes pose one update later than without, as blend
must.

**Not done, and limits.**

- The centre-line dashes, the riders in the mirrors, the dashboard, rider
  and bike poses, and the up/down movement of sky and hills change only
  when the game changes them. The dashes are the design's stage 3.
- In blend and late blend the things that are not followed (dashes, mirror
  riders, up/down shifts, colours) are the newest update's while the rest
  of the scene is up to an update older.
- **The horizon is the game's (fixed 2026-11-15, verified).** The line where
  the video chip turns from hills to ground belongs to the newest update,
  and the in-between road, drawn for an earlier moment, can start a few
  lines above or below it (over a crest, into a dip). Until this fix those
  lines were drawn wrongly: road shifts on lines holding hills (a band of
  the background's lower rows across the picture where the far road should
  be), or sky shifts on lines holding ground. Now such lines are drawn as
  the screen has them: hills with the shift video memory holds, ground with
  the road on screen. Found from the owner's captures of one session at
  200% (`rr-session-replay`, 27,675 frames, work RAM identical): of 27
  captured pictures 13 change, each on 1 to 5 lines at the horizon and
  nowhere else; the two with a visible band (five lines each) no longer
  have it. Whether it also happened at 100% was not measured; nothing in
  the cause depends on the speed.
- A crash, a police stop and a restart are **unverified**: the recorded
  race has none. The rules for them (a camera move of more than 4 segments
  in one update, an object moving more than 64 pixels or 32 lines or
  changing size more than twofold, is shown at once and not slid) are
  tested on made-up data only. The recorded race does end with the bike
  rolling backwards, which is slid like any other motion.
- When nothing the port can describe is on screen (a menu, an object set
  not found in video memory), or the newest update has lasted more than 30
  frames (a pause), the updates kept are forgotten and in-between pictures
  resume once two new ones have shown. If the game stops redrawing while
  paused, predict holds the picture up to one update ahead of the last real
  one until then; whether the game does stop was not looked at.
- After a change of mode the update already on screen is kept again with
  the frame of the change as its stamp, so the first pair's motion is
  timed slightly wrong for one update.
- Two-player races fall back to the picture without in-between frames.
  Not tested.
- One recorded race; the 200% figures are of a different race driven by
  the same buttons.

(The wide view, W, which shows 39 more pixels at either side of all this, is in
`analysis/widescreen.md`.)

## 9. The centre-line dashes (verified as stated below)

Plan: `docs/superpowers/plans/2026-11-13-dashes-and-soft-objects.md`.

**How the game makes them** (read from the code, then measured). They are
not pictures.

- `0x14822`, the loop that makes the per-line shifts (3.1), also writes for
  each road line a **start** (`0xFF2EDE`): the road's centre on that line,
  the per-point centres (`0xFF1DBC`) run through `0x153EA` like the edges;
  and a **width** (`0xFF309E`): three halves of the road's width on that
  line, divided by 64, at least 1. A start at 0x140 or beyond is no dash; a
  negative start is cut off at the screen's edge.
- `0x12768` walks the bands between neighbouring road points from the
  bottom of the view up and sets the width to 0 on the lines of every band
  whose track segment (the camera's segment number plus the band's number)
  is odd. So the centre line is painted on every other track segment: each
  dash is one segment long.
- `0x13F20` packs the lines that have a width into sprites of colour 12 in
  colour set 3, low priority, eight lines at a time, after the objects in
  the sprite list.

**Measured** (`rr-race-replay --dashes-check`, each time the game reaches
`0x13F20`; `RESULT match`):

| | Updates |
|---|---|
| The game reached its dash routine | 2,179 |
| Covered by the port's description | 2,018 |
| of those, every line's start and width equal to the game's | 2,018 |
| Not covered: near the finish line (the word at `0xFF0698` above 0) | 161 |

Near the finish line `0x12768` takes another path (it paints something
across the road; **not traced**), and the description does not hold there
(found on the way: 85 of 157 such images differed). Those updates keep the
game's dash sprites.

Every sprite the game added after its objects was a dash by its attribute
(9,544 of 9,544), which is what lets the port leave exactly those out. Their
tiles hold colour 12 and nothing else (343,729 pixels), except one sprite in
one update (pass 1985) with 52 pixels of other colours: leftovers of an
object's picture in the buffer, a glitch of the game's that the strips do
not copy. (The colour was first read from the code as 13; the game's table
of pixel patterns at `0x13EF4` and this count say 12, and the first frames
drawn with 13 came out grey.)

Adding the centre to the road model also checks it: the port's projection
gives the game's per-point centres on every crisp frame (fine frames still
11,823 of 11,823).

**What the port draws.** Where the road on screen has fine points and its
dash description is proven as above, and the objects on screen are known,
the dashes are drawn as strips of the enlarged picture straight from the
fine road (`rr_dashes_strips`): on every row of a painted segment, from the
road's centre to the centre plus 3/128 of the road's width on that row. The
game's dash sprites are left out of the sprite list (they still count
towards the per-line limits, so nothing else changes). With in-between
pictures the strips are made from the in-between road, when both updates'
dashes are proven. Otherwise the game's sprites are drawn as before.

| Scale 3, `full-throttle` | Dashes as strips | Game's sprites kept |
|---|---|---|
| Without in-between pictures | 10,827 frames | 996 |
| Blend | 10,726 | 1,097 |
| Predict | 10,725 | 1,098 |
| Blend at 200% | 10,945 | 878 |

(of 11,823 frames with the crisp road; all runs at original speed `RESULT
match`). With the strips on, every frame is still the original, enlarged,
on every line without road (12,843 of 12,843, without in-between pictures
and in blend). The final review found that this had not held: the first
version drew strips on the one line just above the road's top line on 202
frames (`off_road_identical` 12,641), because the fine road reaches a
fraction of a line further up than the game's road lines. Strips are now
kept to the game's road lines, and the figure above is from after that
fix.

*Do they move every frame?* In blend at original speed the far end of the
nearest dash moved from one frame to the next on 10,249 frames and stood
still on 322, counted while the camera was moving, from the strips handed
to the renderer; in predict 10,000 and 578; in blend at 200%, 8,975 and
1,504 (that run's still frames are **not explained**, as in section 8).
Before, the dashes moved only when the game made an update.

**What it looks like** (by eye, frames 4000 to 4003 in blend,
`generated/port/draw/dashes-blend-sheet.png`): yellow dashes with clean
slanted edges on the road's centre, the nearest one further down the road
in each frame.

A strip is not the game's dash pixel for pixel, by design: its ends and
width are taken from the fine road on every row, where the game has one
whole-pixel start and width per line. On a synthetic road without fractions
they agree to within a pixel at scale 1 (unit test). On the race (the
reviewer's probe at scale 1 over the 2,018 proven updates, before the fix
above; not re-run): painted against unpainted differed on 17,636 of 306,736
lines, in single-line runs at the ends of bands; the start was more than a
pixel off on 4,282 lines and the end on 7,148. **Hypothesis:** these are
the fine road's fractions (sections 6 and 8 measure the same kind of
difference for the edges); not checked line by line.

**Not done, and limits.**

- Near the finish line the dashes are the game's sprites and step.
- The game's walk of the bands ends at a "stop line" (the word the pointer
  at `0xFF27BA` points to). The whole-pixel description models it; the
  strips do not. In the recorded race it changed nothing on any covered
  update, so whether leaving it out of the strips matters is a
  **hypothesis** (that it does not), not a measurement.
- Which sprites are the game's dashes is told by their attribute (colour
  set 3, low priority, after the objects): 9,544 of 9,544 in this race. A
  crash, the police and two-player races are unverified.
- `0x12768` also chooses, for each band, one of two 16-entry patterns
  (`0x12668`, `0x12728`) by the same odd and even segments and writes it
  per line through the pointer at `0xFF27C6`. **Hypothesis:** this is the
  road surface's banding. How it reaches the screen was not traced, and it
  still changes only when the game changes it.
- The dash description is proven per update against the game's memory; the
  strips between updates rest on the in-between road (section 8) and on the
  unit tests.
- One recorded race.

