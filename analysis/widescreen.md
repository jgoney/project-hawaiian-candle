# The wide view (stage 1)

Design: `docs/superpowers/specs/2026-11-22-widescreen-design.md`. Plan:
`docs/superpowers/plans/2026-11-22-widescreen-stage-1.md`. What the game
holds beyond its 320 pixels: `analysis/widescreen-study.md`.

With **W** on, a one-player race is shown 398 game pixels across instead
of 320. The game is not changed and does not know: the port draws 39 more
pixels on either side of the lines of the race's view (lines 0 to 151)
from what the game holds in memory. Below them the dashboard is the
original, with its margins filled in: the mirrors' inner frames mirrored as
outer ones, and the dashboard's blue beyond. Everything else (menus, a
two-player race) is the original between black margins.

Every claim below is marked **verified** (measured as stated) or
**hypothesis**.

## How the sides are drawn

- **Sky and hills** are full 1,024-pixel pictures that wrap: the planes
  are simply shown 39 pixels further each way (`rr_vdp_render_line_wide`).
- **Ground.** On a ground line each plane holds one block of ground at
  plane columns 352 to 671, with the road's edge line as a mark within 24
  pixels of either end, and slides it so its road-side end stands at the
  road's edge. **Verified** on the six recordings
  (`rr-race-replay --ground-check`): 11,834,764 ground rows, every one
  the block with its marks (in pieces at the finish line); the block
  repeats sideways every 8 columns or fewer; the study's "21 pixels" for
  the mark was too small. In the margins the port shows only the copy of
  the block that belongs on the row, carries the grass on beyond the
  block's far end by repeating its last 8 columns, and draws nothing
  beyond the edge line.
- **A row is ground only if the line is drawn with the road's backdrop**
  (register 7 = 62). **Verified**: on Tennessee 32,466 rows of the sky's
  picture have the block's shape; none of them, and no ground line, is on
  the wrong side of that rule on any recording.
- **The road's edge beyond the game's limits.** The game holds a road
  edge that leaves its screen at the screen's side (limits -0x160..0x140
  and 0..0x180). The road code now also says how far beyond the limit the
  edge really lies (`RrRoadShifts.beyond_a/b`), and the grass is carried
  on to there and the edge line drawn there.
- **Centre-line dashes** run on into the margins (`rr_dashes_strips_wide`).
- **Objects.** The objects are drawn once more with a wider view
  (`rr_objects_draw_view`, `rr_objects_margin`): a picture the game cut
  at a side of its screen is put back whole, standing where the screen's
  own half stands; a picture wholly beyond the screen's side is drawn;
  and so are copies of roadside objects that the game did not make. They
  are a second sprite layer with line limits of its own, so they never
  cost the screen a sprite. The game stops a picture it cuts four pixels
  short of its right edge (a mirrored one four or five); columns 315 to
  319 are filled from the same layer where the screen has no object.
  **Verified by tests only** (pictures wider than the margin, plain and
  mirrored, odd cuts, a stop at 315 and at 316; a console test draws a
  picture across each side pixel for pixel) and by a count on the
  recordings that no column of a margin picture is left without a sprite
  entry (`gaps`, below). That the joined picture is the right picture on
  the recordings was judged by eye. The independent review found two
  faults here that 4 thousand million checked ground pixels had not
  shown (a hole beside the screen for a picture reaching more than 39
  pixels past it, and a one-pixel slit at column 315); both are fixed.
- **A copy that stands across the screen's side shows only its margin
  half**: the game does not draw such a copy at all, the middle is not
  changed, so the copy is cut hard at x = 0 or 320. How many of the
  margin's copies are of this kind is not counted.

## The design's requirements

Measured with `rr-race-replay … --scaled-check 3 --fine-objects
--dashes-check --inbetween blend --inbetween-check --wide 39` on the six
recordings, and the same without `--wide`.

1. **The game is untouched: verified.** `RESULT match` on all six with
   and without `--wide` (all 64 KB of work RAM at every pass). The owner's
   27,675-frame session replayed with `wide 1` added ends in the captured
   RAM (`RESULT reproduced`).
2. **The original picture is untouched; W off is today's picture:
   verified** for the port's code path by tests: a fingerprint of the
   narrow enlarged picture taken before the renderer was changed still
   holds (`test_the_narrow_picture_is_what_it_was`, two made-up scenes),
   and a console with W
   switched on and off again draws the same frame as one that never had
   it. On the recordings every line of the tool's output other than the
   `WIDE_` lines is the same with and without `--wide`, except the
   in-between check's count of moving dashes, which samples strips that
   now include dashes lying wholly in a margin.
3. **The middle is the same picture: verified for the renderer as the
   measuring tool drives it** (the console's own path is covered by tests,
   not by the recordings). Pixels of the middle
   320 columns that differ from the narrow picture, all six: road line 0,
   dash 0, other 0; every differing pixel is one of the seam's
   (`differing_outside_seam=0`).

   | Recording | frames | middle identical | seam pixels | of them differing |
   |---|---|---|---|---|
   | full-throttle | 12,843 | 10,778 | 747,215 | 731,380 |
   | arrested | 8,316 | 6,808 | 564,453 | 549,829 |
   | vermont | 13,270 | 10,063 | 1,099,216 | 1,002,051 |
   | arizona | 14,586 | 12,549 | 773,897 | 768,830 |
   | tennessee | 17,251 | 15,611 | 920,468 | 876,700 |
   | hawaii | 21,609 | 20,011 | 901,409 | 867,332 |

   (In-between pictures on; pixels are of the picture enlarged 3 times.)
4. **No garbage at the sides: verified for the ground, counted for the
   sky.** Every margin pixel of every ground line drawn with the road's
   shifts was compared with what the road alone says belongs there (grass
   outside the edges, the edge line within 24 pixels inside them, road
   between): 4,080,938,785 pixels, none wrong. The check is itself tested
   against deliberately wrong pictures (black margins, a wrapped block,
   road where grass belongs). No margin pixel of a view line was a panel
   (3,717,551,916 sky-line pixels counted). What the sky's margins show
   is the planes as they are; that they look right was judged by eye
   only.
5. **Mirrors and dashboard are safe: verified** by construction and test:
   lines 152 to 223 are the narrow rows, unchanged, between margins
   (`test_lines_from_wide_lines_down…`), and a picture whose top is at
   line 152 or below is left out of the margin objects. On a race frame
   the margins of those lines are filled from the dashboard itself
   (`rr_wide_dashboard`, `test_the_dashboard_runs_on_into_the_margins`):
   on lines 179 to 219 the mirrors' inner frames (columns 64 to 66 and
   255 to 253) mirrored, then colour 50, the dashboard's blue; on the
   other lines the edge pixel runs on. **Verified** on the owner's
   27,675-frame session: colour 50 was that blue on every race frame, and
   the picture without margins is unchanged to the bit. That it looks
   right was judged by eye on four frames. **Hypothesis:** nothing but the
   dashboard has the window at both edges of a line below 152 in a race
   (a pause or a message there would be filled the same way).
6. **Width 398: done.** One fixed width.
7. **Speed: verified on one session.** `rr-session-replay` of the
   owner's session (scale 3, blend, soft objects): 9.09 ms a frame
   without, 10.98 ms with (user time over 27,675 frames, menus included).
   The window itself has not been timed.
8. **The other features keep working: verified in the tool and the
   console, not yet in the window.** In-between pictures carry the
   margins (table above is with blend on); soft far objects are softened
   in the margins (test); the session records `wide` and replays it (a
   test compares the pictures of a menu frame; the owner's session ends
   in the same RAM with a 1194 x 672 picture, which was not compared with
   one the window drew). The CRT
   shader is told the picture's width. **Not done:** the Godot library
   has not been rebuilt with this (a window was open), so W has not been
   seen in the real window and `godot/tests/test_console.gd` has not been
   run against it.

## What stage 1 leaves

- **Roadside objects the game itself leaves out.** The game does not make
  a draw record for a roadside object outside its own view, so nothing
  can be drawn of it until it would reach the original's screen
  (`analysis/widescreen-study.md`: in 16 to 29% of updates such an object
  would show at 398). They pop in at the old edge. This is stage 2.
- **Counts over the six recordings** (frames with an object set on
  screen: 73,770): margin pictures 84,809, of them 17,211 put back whole,
  3,938 wholly beyond the screen's side, 68,051 copies; 7 records left out
  for want of sprites (all on full-throttle). Columns of a margin picture
  that no sprite entry shows (`gaps`): 0 on all six. Lines drawn without the
  road's shifts while held at the screen's side: 0. Race frames with no
  road chosen (drawn wide from the planes alone): 1 per recording.
- **Short lines** (the game's block ends inside a margin, so the grass
  beyond is the port's continuation): 0 to 3.1% of ground lines, as the
  study found.
- **A two-part object whose first part the game dropped** is joined to
  the part on screen as the wide drawing places it, which can be one line
  off. Not measured.
- **Tennessee's sky** has rows only 362 to 368 pixels wide; at 398 the
  margin beyond them shows the layer behind. Looked at: nothing visible.
- **Why a margin picture is legitimate: hypothesis.** The margin objects
  are drawn from the RAM image the object tracker noted, which is known
  to be the one on screen only because the narrow set drawn from it
  matches video memory byte for byte. That two consecutive updates cannot
  be confused this way (the tile base alternates) is not proven.

## Stage 2

The port would repeat the game's roadside walk with a wider cone and add
its own records to a copy of the RAM image (`rr_objects_margin` already
takes a second image for this). Open question: the tracker keeps RAM as
at 0xA4EE, and the walk needs its inputs as at 0x16E8C.
