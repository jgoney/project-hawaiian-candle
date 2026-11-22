# Widescreen: what the game has beyond the 320-pixel edge

Date: 2026-11-22. A read-and-measure study for
`docs/superpowers/specs/2026-11-22-widescreen-design.md`. Nothing was
built into the port.

**How to read this.** Every number below was **measured once** by throwaway
scripts on the six recorded sessions (full-throttle, hawaii, tennessee,
vermont, arizona, arrested; 14,491 race updates), and has not been
re-measured or reviewed. The scripts and their result tables are kept in
`generated/widescreen-study/scripts/` (ignored by Git; they read
ROM-derived captures that were deleted afterwards and would have to be
made again with `capture_frames.sh`). Statements about the game's code are
**read from the code** and marked so. Treat the numbers as the reason for
the design, not as pass marks: the build measures its own.

## Planes

- Planes A and B are 1,024 x 256 pixels (register 16 = 0x03), at 0x4000
  and 0x6000; scrolling is per line sideways and whole-screen up and down
  (register 11 = 0x03). Measured.
- **Ground rows:** both planes hold one grass block exactly 320 pixels wide
  (plane columns 352 to 671) with the painted edge line in a band up to
  about 21 pixels outside each side (331 to 350 and 673 to 693). The rest
  of those rows is empty. Held in all 22 race frames sampled.
- The game's shifts put plane A's grass on screen x = [left - 320, left)
  and plane B's on [right, right + 320). A 320-wide view is always
  covered; a wider one is not. Simply drawing the planes wider shows road
  grey and a stray copy of the other side's edge line beyond the block.
- Every ground row of the block repeats sideways with a period of 8 pixels
  or less (rows 62 and up tested; a striped band on rows 56 to 80, just
  above the block, is **not explained**).
- **Sky, clouds, hills:** full 1,024-pixel panoramas that wrap; over a
  recording the strips take 905 to 1,024 of the 1,024 possible shifts.

Share of road lines (and of updates with at least one such line) where
the grass block falls short of the view:

| Recording | 398 wide | 426 wide | 480 wide |
|---|---|---|---|
| full-throttle | 2.49%; 3.9% | 2.70%; 4.0% | 3.18%; 5.9% |
| hawaii | 0.04%; 1.5% | 0.18%; 4.4% | 0.45%; 7.0% |
| tennessee | 1.23%; 5.4% | 1.87%; 7.8% | 3.79%; 12.4% |
| vermont | 0; 0 | 0.01%; 0.3% | 0.10%; 1.4% |
| arizona | 1.25%; 3.5% | 1.61%; 4.6% | 2.68%; 12.3% |
| arrested | 3.46%; 5.7% | 3.79%; 5.9% | 4.50%; 8.8% |

The worst shortfall is the whole margin; it happens with the bike far to
one side of the road. The game clamps the left edge to -0x160..0x140 and
the right to 0..0x180 (`port/render/rr_road.c`, the clamps in
`rr_road_shifts`); in a 398 view the clamp hides an edge that would be on
screen on 532 to 996 lines per recording in four of the six.

## Projection

The perspective table at ROM 0x27C84 is floor(65536 x 256 / (d + 256)) for
all 8,191 entries checked: a pinhole projection with a focal length of 256
pixels, the same across and down, centred on 160 (0xFFF2BE). So a wider
picture at the same scale is consistent: 64 degrees across at 320, about
76 at 398, about 80 at 426.

## Objects

Read from the code:

- **Vehicles** (0x15672, placed by 0x1628A): no sideways cull. A record is
  dropped only when its x is 0x8000, behind the camera.
- **Roadside** (0x17198): two culls, both to 0x172B0. A cone, |sideways
  world position| >= distance, which on screen is |x - 160| >= 256 d /
  (d + 256) (never more than 256 pixels from centre); and the anchor x
  outside 0..0x167 (uneven: 0 on the left, 360 on the right).
- **The drawing** (0xA5C8) clips every picture to 0..319; the port's copy
  has this as `VIEW_LEFT`/`VIEW_RIGHT` in `port/render/rr_objects.c`.

Measured (213,928 records in the 3D view; the 14,736 mirror-rider records
left out):

- Roadside anchor x is always within 0..359; 740 of 160,336 are in
  320..359. Other records range from x = -6271 to 2631.
- Against the 320 window, per recording, 98.3% to 99.2% of pictures are
  wholly inside, 0.7% to 1.1% partly outside, 0.1% to 1.4% wholly outside.
- Records whose picture reaches into the extra 39 pixels of a 398 view:
  in 11% to 21% of updates. These need only the port's clip widened.

Roadside candidates the game culled (captured at 0x172B0; 12,808, 92% by
the cone). Share of updates in which at least 4 pixels of a culled
object's picture would lie inside the view:

| Recording | 320 (the original) | 398 | 426 |
|---|---|---|---|
| full-throttle | 8.9% | 27.0% | 32.0% |
| hawaii | 6.5% | 23.5% | 27.4% |
| tennessee | 7.6% | 21.2% | 25.2% |
| vermont | 4.6% | 16.2% | 19.7% |
| arizona | 5.5% | 18.9% | 24.2% |
| arrested | 9.2% | 28.7% | 32.4% |

The count is rough: the height test uses the top of the view and line 152
only, and copies of a culled object are not counted. Widening the cull
inside the game would change the record table (0xFFCEB8) and count
(0xFFDC42), so it cannot be done there; the port would have to repeat the
roadside walk (0x16E8C to 0x172BE) on a copy of work RAM.

## Other things tied to 320

- **Dashes:** the game drops a dash starting at 0x140 or beyond and cuts a
  negative start at 0; the port's strips are cut at the screen width. The
  game's own dash sprites near the finish line stop at 0..319.
- **Line interrupts:** whole lines; nothing tied to 320.
- **Sprite limits:** the chip model keeps 20 sprites and 320 sprite pixels
  a line; a sprite's x is 9 bits (-128..383), so a view reaching past 383
  does not fit a sprite table.
- **Dashboard and mirrors** (lines 152 to 223): the window plane's columns
  beyond 320 are blank filler. There is no art there. The riders in the
  mirrors are draw records at lines 201 to 212: drawn with a wider clip,
  one grew a stray copy inside the dashboard, so records below line 152
  must keep the 0..320 clip.

## The experiment

A Python script (`wide.py`) drew seven recorded frames at 398 wide from
the video memory, the per-line state and the game's draw records. Its
320-wide picture equals the port's own pixel for pixel on all seven. On
three, the centre 320 columns of the wide picture are identical to the
original; the other four differ on 8, 88, 697 and 697 pixels (the 88 are
the mirror rider's copy; the others were not looked at). It used neither
the C renderer nor the fine road, fine objects or in-between pictures.

The comparison sheets are in `generated/widescreen-study/`. Each has four
panels: top left the original centred; top right the planes simply drawn
wider; bottom left wide from the game's own records, grass continued;
bottom right the same with culled roadside objects put back by hand.
