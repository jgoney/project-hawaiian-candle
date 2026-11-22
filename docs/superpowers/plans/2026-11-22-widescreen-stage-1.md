# Widescreen race view, stage 1: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** With the crisp road, fine road points and fine objects on, **W** shows the one-player race 398 game pixels wide (16:9) instead of 320, with the game and the original picture untouched.

**Architecture:** `RR_SCREEN_W` stays 320 and the 320 x 224 picture is not touched. Only the enlarged ("hires") picture grows a margin of M = 39 game pixels on each side. The centre 320 columns stay on today's code path and data (same shifts, same sprite table, same limits). The margins are separate work added beside it: extra plane columns sampled with the same per-row shifts, the ground continued where the game's 320-wide grass block ends, a separate margin sprite layer made by the port's object drawing with a wider view, and dash strips allowed to run on. Lines from 152 down (dashboard, mirrors) are the original between black panels.

**Tech Stack:** C11 (`port/`), Python tools and pytest (`src/rr2port`, `tests/port`), Godot 4.7 GDScript and a shader (`godot/`).

**Spec:** `docs/superpowers/specs/2026-11-22-widescreen-design.md` (the authority). Background: `analysis/widescreen-study.md` and the study's tables in `generated/widescreen-study/scripts/` (`planes.txt`, `grass.txt`, `shifts.txt`, `records.txt`, `wide.py`): measured once; reasons, not pass marks. `analysis/race-picture.md` sections 3 and 5 to 9.

**Reviewed:** an independent review on 2026-11-22 found two critical and nine important faults in the first version of this plan; this version answers each (see "What the review changed" at the end).

## Global Constraints

- The game is untouched: `rr-race-replay` on the six recordings (full-throttle, hawaii, tennessee, vermont, arizona, arrested) ends `RESULT match` with the wide view on.
- The 320 x 224 picture is not changed; with W off every picture the port draws is today's, pixel for pixel. `RR_SCREEN_W` and `RR_CONSOLE_W` stay 320.
- The wide view is 398 game pixels: margin M = 39 each side. One width.
- W acts only with the crisp road (R), fine road points (F) and fine objects (O) all on and a scale above 1; otherwise the picture is the original shape.
- Never a picture for something the port has not matched. A margin picture is drawn only from a work-RAM image that the narrow object set, made from the same image, has proven byte for byte against video memory.
- On a frame or line the port cannot describe, the view stays wide and what cannot be described is drawn as the game has it.
- The centre 320 columns show exactly the object set shown with W off, and are pixel for pixel the picture with W off **except** the seam fill of Task 13 (columns 316 to 319 of a picture the game cut short of its own edge), which is counted. A margin object never costs the centre a sprite.
- Lines 152 to 223 are drawn exactly as with W off, between panels of colour 0x000000.
- Nothing derived from the ROM is committed; test data is made up, in the style of the existing tests. The Python `race` fixture (`tests/port/test_race_replay.py` 17-113) has no planes and no race frames: Python tests cover options and output format only; correctness is tested in C on made-up planes, and measured on the recordings with counts that must be above zero.
- `godot/bin/librr2.dylib` is not rebuilt while a Godot window may be using it: check with `pgrep -lf "[G]odot.app"` and say so before the rebuild (Task 16).
- Claims added to `analysis/` are marked verified or hypothesis.
- Stage 2 (the port working out the roadside objects the game dropped) is definite. Interfaces marked "stage 2" are shaped for it; nothing of it is built here.

## The ground, as the study's tables show it (`planes.txt`, `grass.txt`)

Both planes hold the same thing on a ground row: a **block** of grass at plane columns 352..672, opaque across its whole width, and on **both** sides of it a thin edge-line mark that slants with the row: from 331..338 and 686..693 on the nearest rows to touching the block (351..673) at the horizon. Everything else on the row is blank. The game shows plane A's block at screen `[L - 320, L)` (left verge; its mark above column 672 is the road's left edge line, its mark below 352 is never on a 320 screen) and plane B's at `[R, R + 320)` (the mirror image). The block's rows repeat sideways with a period of 8, 4 or 1 pixels. Two exceptions per frame: the first ground row is blank in plane A and full (0..1024) in plane B. Sky rows are wrapping panoramas, some sparse (plane B has rows with only 396 to 498 opaque columns).

So in a wide view, per plane, a ground row is drawn as: the block and its **road-side** mark as they are; the **grass-side** mark and everything beyond it replaced by the block continued; nothing beyond the road-side mark. For plane A the road side is high (columns above 672), for plane B low.

## Review Focus

1. **The bike far to one side, and crests:** rows at a clamp, fine edges that leap many pixels in a line, negative screen x (the unsigned compare in `render_plane`), the copy of the block 1,024 columns away. Expected: no stray edge line or wrapped scenery in a margin, centre unchanged. Tests in Tasks 3, 5, 6.
2. **Lines the trackers and video memory disagree on:** first race frame, a table entry rewritten mid-frame, the in-between "doubtful" lines at the horizon, other CPU speeds. Expected: the line is still wide, drawn from video memory as it is, with no `beyond`. Tests in Tasks 5, 7, 14.
3. **Objects on the old edges:** odd-pixel left cuts, mirrored pictures cut at the right (which end at 315 or 316), two-part records whose first part is off the narrow screen, copies straddling an edge, pictures moved by a fraction of a pixel. Expected: a cut picture continues into the margin with no doubled or missing column. Tests in Tasks 12, 13.
4. **Crowded scenes:** 20 sprites or 320 sprite pixels on a line, 80 sprites, 0x150 tiles, 80 pictures. Expected: the centre draws exactly what it draws with W off; the margins lose objects first. Tests in Tasks 12, 13.
5. **Frames that are not a plain race, with W on:** menus, race start and finish, a crash, a pause, two-player, F5, and switching R, F, O, W or the scale mid-race (the buffer changes size between frames). Expected: no out-of-bounds write, no frame at a stale size, menus and two-player centred between black. Tests in Tasks 3, 15, 17; pictures looked at in Task 18.

---

## File Structure

| File | Responsibility in this plan |
|---|---|
| `port/hw/rr_vdp.h`, `rr_vdp_render.c` | Wide rows: planes beyond the edge, ground continued, margin sprite layer, strips over the wide span |
| `port/hw/rr_genesis.h`, `rr_genesis.c` | The frame loop hands the wide description to the renderer |
| `port/render/rr_wide.h`, `rr_wide.c` (new; add to `RENDER_SRC`/`RENDER_HDR` in `port/Makefile`) | The game's ground constants in one place; the checks that say whether a wide picture is right (used by the tool, tested in C) |
| `port/render/rr_road.h`, `rr_road.c` | `beyond_a/b`: how far each row's edge lies past the game's clamp |
| `port/render/rr_objects.h`, `rr_objects.c` | Drawing with a view other than 0..319; what became of each record; the margin object set |
| `port/render/rr_dashes.h`, `rr_dashes.c` | Strips cut at the wide view |
| `port/render/rr_between.h`, `rr_between.c` | In-between scene carries `beyond` and a margin set |
| `port/host/rr_console.h`, `rr_console.c` | The `wide` setting, buffer size, session log |
| `port/host/rr_godot.c` | `set_wide`, `get_wide`, width of the frame handed out |
| `port/tools/rr_race_replay.c`, `rr_session_replay.c` | `--ground-check`, `--wide M`, the `WIDE_*` lines; pictures at the console's width |
| `godot/scripts/console.gd`, `shaders/crt.gdshader` | W key, window fit and size for a 398-wide picture, saved setting, capture settings, tube size |
| `analysis/widescreen.md` | What was measured |

Line numbers are as surveyed on 2026-11-22 at commit `da260ba`; find the named function if they have moved. Each task's steps are: write the named failing tests; run `make -C port test` (or the named command) and see the stated failure; implement; run again and see every test pass; commit with the stated message.

---

### Task 1: The opaque runs of a plane row

**Files:** Modify `port/hw/rr_vdp.h`, `rr_vdp_render.c`; Test `port/tests/test_vdp.c`.

**Produces:** `int rr_vdp_plane_row_runs(const RrVdp *v, int which, int line, int *first, int *end, int max_runs);` For plane `which` (0 = A, 1 = B) and the plane row shown on display `line` (through the vertical scroll), the runs of plane pixel columns that hold an opaque pixel **on that pixel row**, lowest first, as `[first[i], end[i])`; returns the number of runs (more than `max_runs` returns `max_runs + 1` with the first `max_runs` filled).

- [ ] Failing tests: `test_runs_of_a_block_with_two_marks` (made-up row: pixels at 331..338, 352..672, 686..693 → 3 runs with those bounds); `test_runs_follow_the_vertical_scroll`; `test_a_full_row_is_one_run_0_to_1024`; `test_an_empty_row_has_no_runs`; `test_more_runs_than_room_says_so`. Failure: undeclared function.
- [ ] Implement beside `render_plane`. Commit `Plane rows: the runs of opaque pixels`.

---

### Task 2: The ground's constants, and the row test

**Files:** Create `port/render/rr_wide.h`, `rr_wide.c`; Modify `port/Makefile`, `port/tools/rr_race_replay.c`; Test `port/tests/test_road.c` (or a new `test_wide.c` added to `make test`), `tests/port/test_race_replay.py`.

**Produces:**
```c
/* port/hw/rr_vdp.h */
typedef struct {
    uint16_t first, end;  /* the ground block's plane columns: [first, end) */
    uint16_t mark;        /* the widest the edge-line mark gets beyond either end */
    uint8_t road_high;    /* 1: the road is on the high-column side (plane A); 0: the low (plane B) */
    uint8_t period;       /* the block repeats sideways with this period */
    int16_t fit_low, fit_high; /* on a line with no shift given: the screen x of the block's road-side end lies in [fit_low, fit_high] */
} RrWideBlock;
/* port/render/rr_wide.h */
enum { RR_WIDE_MARGIN = 39, RR_WIDE_LINES = 152 };
void rr_wide_blocks(RrWideBlock block[2]); /* the game's: {352, 672, 21, 1, 8, -352, 320} and {352, 672, 21, 0, 8, 0, 384} */
/* Is this a ground row of `b`: opaque right across [first, end) and blank outside [first - mark, end + mark)? */
int rr_wide_is_ground_row(const RrVdp *v, int which, int line, const RrWideBlock *b);
```
Tool option `--ground-check` (needs `--scaled-check`), printing at the end, over every line of every race frame:
`GROUND ground_lines=N block_rows=B full_rows=F empty_rows=E other_ground_rows=O sky_lines=S sky_rows_passing=P mark_max=W period_max=Q` where the tracker's `whole.have` says which lines are ground: `block_rows` pass `rr_wide_is_ground_row`, `full_rows` are one run 0..1024, `empty_rows` have no run, `other_ground_rows` are anything else; `sky_rows_passing` counts **non**-ground lines that pass the row test.

- [ ] Failing tests (C): `test_a_block_with_marks_is_a_ground_row`; `test_a_block_alone_is_a_ground_row` (horizon rows); `test_a_sparse_sky_row_inside_the_window_is_not` (opaque at 396..498 only); `test_a_full_row_is_not`; `test_a_block_with_something_at_column_100_is_not`. Python: `test_ground_check_prints_its_line_and_does_not_change_the_race` (format only; the fixture has no race, so `ground_lines=0`).
- [ ] Implement.
- [ ] **Gate, decided in advance.** Run on the six recordings and ledger the six lines. Then:
  - `other_ground_rows = 0` and `sky_rows_passing = 0`, `mark_max <= 21`, `period_max <= 8`: go on.
  - `mark_max` or `period_max` larger: change the two constants in `rr_wide_blocks` to the measured values, ledger a ruling, go on.
  - `sky_rows_passing > 0`: the row test alone is not safe. Rule: a row counts as ground only on a line where the backdrop register (reg 7) holds the road's colour too (`wide.py` did this); add that to `rr_wide_is_ground_row`'s callers in Task 5 and a test for it; go on.
  - `other_ground_rows > 0`: dump one such row's runs (`rr_vdp_plane_row_runs`), describe the class in the ledger, and add it to this section before Task 5. If it cannot be described as "block, marks, full or empty", stop and ask.
- [ ] Commit `The ground's block and marks, measured on the recordings`.

---

### Task 3: Wide rows, planes as they are

**Files:** Modify `port/hw/rr_vdp.h`, `rr_vdp_render.c`; Test `port/tests/test_vdp.c`.

**Produces:**
```c
enum { RR_WIDE_MARGIN_MAX = 64 };
typedef struct {
    int margin;            /* game pixels each side, 0..RR_WIDE_MARGIN_MAX */
    int lines;             /* lines 0..lines-1 are drawn wide; the rest are the original between panels */
    uint32_t panel;        /* 0x00RRGGBB of the side panels */
    RrWideBlock block[2];  /* plane A, plane B; period 0: the ground is not continued */
} RrWide;
/* One display line as `scale` rows of (320 + 2 * margin) * scale pixels. */
void rr_vdp_render_line_wide(const RrVdp *v, int line, int scale, const int32_t *shift_a,
                             const int32_t *shift_b, const int32_t *beyond_a, const int32_t *beyond_b,
                             const RrFineSprites *fine, const RrFineSprites *margin_fine,
                             const RrWide *wide, uint32_t *out, RrPixelSource *sources);
```
`wide == NULL` or margin 0 is `rr_vdp_render_line_fine` exactly, which becomes a call of this. `sources`, if not NULL, receives the rows as sources. In this task `beyond_*`, `margin_fine` and `block` are accepted and ignored.

Behaviour: on `line < wide->lines`, each plane is sampled from screen x = -margin to 319 + margin with the row's shift (`plane_x` starts `margin` pixels earlier; x signed; the vertical-scroll column held at its first value for x < 0 and its last for x >= 320). The window, tile sprites and the fine sprite table exist only in the centre. A line with any window pixel, and every line >= `wide->lines`, is the narrow row between panels.

- [ ] Failing tests: `test_margin_0_is_render_line_fine` (every line, scales 1..4, shifts and fine sprites: `memcmp`); `test_the_centre_of_a_wide_row_is_the_narrow_row`; `test_margins_show_the_plane_beyond_the_edge` (x = -8 shows plane column 1016 at shift 0, x = 327 column 327); `test_a_fractional_shift_carries_into_the_margins`; `test_a_window_line_is_the_original_between_panels`; `test_lines_from_wide_lines_down_are_the_original_between_panels`; `test_every_scale_and_margin_stays_in_bounds` (scales 1..8, margins 0, 1, 39, 64; pattern of `assert_guards`); `test_the_plain_fast_path_is_wide_too`.
- [ ] Implement; buffers sized by `(RR_SCREEN_W + 2 * RR_WIDE_MARGIN_MAX) * RR_SCALE_MAX`. Commit `Wide rows: the planes sampled beyond the 320-pixel edge`.

---

### Task 4: The road beyond the game's clamps

**Files:** Modify `port/render/rr_road.h`, `rr_road.c`; Test `port/tests/test_road.c`.

**Produces:** `RrRoadShifts` gains `int32_t beyond_a[RR_SCREEN_H * RR_SCALE_MAX], beyond_b[...]`, indexed like `a`/`b`: the unclamped shift minus the clamped one (the local in `plane_shift` before its clamp, `rr_road.c` 201-208), 16.16, saturated to ±256 pixels; 0 where the clamp does not bite. `a`, `b`, `have` exactly as today. Filled by `rr_road_shifts` and `rr_road_shifts_fine`.

- [ ] Failing tests: `test_beyond_is_zero_inside_the_clamps`; `test_beyond_is_the_excess_on_every_row` (whole and fine: an edge 30.5 pixels past the clamp gives `0x1E8000`); `test_beyond_saturates` (5,000 pixels out gives `0x1000000`); `test_a_b_and_have_are_unchanged_by_beyond`. `test_clamped_edge_stays_clamped_on_every_row` still passes.
- [ ] Implement. Commit `Road shifts: how far each row's edge lies beyond the clamp`.

---

### Task 5: Ground rows: nothing wrapped, nothing beyond the road-side mark

**Files:** Modify `port/hw/rr_vdp_render.c`; Test `port/tests/test_vdp.c`.

**Consumes:** `RrWideBlock` and `rr_vdp_plane_row_runs` (the renderer makes its own row test from them, cached by plane row and `v->vram_writes`; it does not include `rr_wide.h`).

Behaviour, per plane, on a wide line whose row passes the ground-row test with `period != 0`: every pixel's plane column `q` is taken **unwrapped**. On a line with shifts, from the row's own 16.16 shift (the road code's shifts are not wrapped). On a line with no shift given, from the line's scroll value, taking the copy of the plane for which the block's road-side end falls at a screen x within `[fit_low, fit_high]`. Then: `q` on the road side beyond the block's mark (`q >= end + mark` for plane A, `q < first - mark` for B) is see-through; the grass side is left for Task 6 (in this task: see-through there too). Rows that fail the test are sampled wrapped as in Task 3. The centre is never changed.

- [ ] Failing tests (a made-up block at 352..672 with marks at 331..338 and 686..693): `test_nothing_shows_beyond_the_road_side_mark`; `test_the_wrapped_copy_of_the_block_is_not_shown` (plane A with its left edge at the -0x160 clamp: without this the copy 1,024 away lands at x >= 352); `test_an_unshifted_line_picks_the_copy_that_fits_the_clamps`; `test_a_row_that_fails_the_test_wraps_as_before` (a sparse sky row; a full row); `test_the_centre_is_unchanged`. If Task 2's gate added the backdrop rule: `test_a_block_row_on_a_sky_backdrop_line_is_not_ground`.
- [ ] Implement. Commit `Wide rows: no wrapped ground, nothing beyond the edge line`.

---

### Task 6: Ground rows: the grass continued, and the edge beyond the clamp

**Files:** Modify `port/hw/rr_vdp_render.c`; Test `port/tests/test_vdp.c`.

**Consumes:** `beyond_a/b` (Task 4), the unwrapped `q` of Task 5.

Behaviour on a ground row: on the grass side of the block (`q < first` for plane A, `q >= end` for B) the pixel is the block's own, continued: column `first + ((q - first) mod period)` on the low side, `end - period + ((q - end) mod period)` on the high side. This replaces the grass-side mark. With `beyond != 0` on a row (only possible where the road-side end sits at a clamp), the road-side mark is drawn displaced by `beyond` and the block continued up to it. A line whose shifts were dropped (Task 7) has no `beyond`: its mark stays where the game has it; if that puts a mark inside a margin with nothing known beyond it, grass is continued through the margin instead and the line is counted by the tool (Task 9).

- [ ] Failing tests: `test_grass_continues_on_the_grass_side` (pixel at x = L - 350 has the block's colour at its phase, periods 8, 4 and 1); `test_the_grass_side_mark_is_not_drawn`; `test_beyond_moves_the_mark_and_fills_with_grass` (edge at the clamp x = 320, beyond 20 pixels: grass to 339, mark from 340); `test_no_beyond_on_a_line_without_shifts`; `test_a_block_wholly_off_the_view_gives_grass_across_its_side`; `test_the_centre_is_unchanged_by_continuation`.
- [ ] Implement. Commit `Wide rows: the grass continued and the edge drawn beyond the clamp`.

---

### Task 7: The frame loop draws wide

**Files:** Modify `port/hw/rr_genesis.h`, `rr_genesis.c`; Test `port/tests/test_console.c`.

**Produces**, fields of `RrGenesis` (NULL/0 by default, never read by the model):
```c
const RrWide *hires_wide;                       /* NULL: the enlarged picture is 320 * scale wide */
const int32_t *hires_beyond_a, *hires_beyond_b; /* used only on a line whose shifts are used */
const RrFineSprites *hires_margin_sprites;      /* under the same stamp rule as hires_sprites */
uint32_t *hires_narrow;        /* tools: the same frame with no margin, 320 * scale wide, or NULL */
RrPixelSource *hires_sources;  /* tools: sources of the wide rows, or NULL */
```
With `hires_wide` set, row `r` of line `l` is at `hires[(l * scale + r) * (320 + 2 * margin) * scale]`.

- [ ] Failing tests: `test_genesis_draws_wide_rows_at_the_wide_stride`; `test_beyond_is_not_used_on_a_line_whose_entry_changed` (pattern of `choose_then_change_line_130`: still wide, shift and beyond dropped); `test_margin_sprites_are_dropped_when_video_memory_changed`; `test_hires_narrow_is_the_picture_without_margins`.
- [ ] Implement at the call in `rr_genesis_run_frame` (c 497-516). Commit `The frame loop draws the enlarged picture wide when asked`.

---

### Task 8: Checks that can fail

**Files:** Modify `port/render/rr_wide.h`, `rr_wide.c`; Test the C test file of Task 2.

**Produces:**
```c
typedef struct { uint64_t pixels, differing, road_line, object, dash, other; } RrWideCentre;
/* Compare the centre columns of `lines` wide lines, and all of the rest, with the narrow picture. */
void rr_wide_centre(const uint32_t *wide_px, const uint32_t *narrow_px, int scale, int margin, int lines,
                    const uint8_t *have, const uint8_t *object_cover, const RrStrips *strips, RrWideCentre *out);

typedef struct {
    uint64_t examined;     /* margin pixels looked at on ground lines */
    uint64_t grass, mark, road;   /* as expected */
    uint64_t wrong;        /* drawn as one kind where another was expected */
    uint64_t short_lines, beyond_lines;
} RrWideGroundCount;
/* What each margin pixel of a ground line should be, worked out from the road alone
 * (its unclamped edges: shifts plus beyond), not from the planes: grass left of the left
 * edge and right of the right, the edge-line mark at each edge, road between. Compared
 * with what was drawn (sources: backdrop = road; plane A or B = grass or mark, told
 * apart by the tile). */
void rr_wide_ground(const RrPixelSource *sources, const RrRoadShifts *road, const RrVdp *v, int line,
                    int scale, int margin, RrWideGroundCount *out);
typedef struct { uint64_t examined, panel, plane, backdrop; } RrWideSky; /* margin pixels on non-ground wide lines */
void rr_wide_sky(const RrPixelSource *sources, int scale, int margin, RrWideSky *out);
```

- [ ] Failing tests, each rendering a made-up frame with `rr_vdp_render_line_wide` and then with a **deliberately wrong** variant: `test_a_right_picture_scores_zero_wrong_and_examines_pixels` (`examined > 0`, `wrong == 0`, `differing == 0`); `test_black_margins_are_caught` (margins overwritten with the panel colour: `wrong > 0`); `test_a_wrapped_block_is_caught` (rendered with `block.period = 0` and Task 5 off, at the left clamp: `wrong > 0`); `test_road_where_grass_belongs_is_caught` (continuation off); `test_a_changed_centre_pixel_is_classed` (one each on a road line, under an object, on a dash, elsewhere).
- [ ] Implement. Commit `Checks for a wide picture, tested against wrong pictures`.

---

### Task 9: Measure it on the recordings: `--wide`

**Files:** Modify `port/tools/rr_race_replay.c`; Test `tests/port/test_race_replay.py`.

**Produces:** option `--wide M` (0 < M <= 64; needs `--scaled-check` and `--fine-objects`, else the tool's usual input error, exit 2). The tool's own `choose_road` (about 1265-1306) sets `hires_wide` (`rr_wide_blocks`, `lines` = 152 on race frames), `hires_beyond_a/b`, and `hires_narrow`/`hires_sources`. `--scaled-at` writes the wide picture. Existing checks index the centre. Lines at the end:
- `WIDE_CENTRE frames=N identical=K pixels=T differing_pixels=P road_line=A object=B dash=C other=D first_frame=F|none`
- `WIDE_GROUND examined=E grass=G mark=M road=R wrong=W short_lines=S short_updates=U beyond_lines=B no_edge_lines=X`
- `WIDE_SKY examined=E plane=P backdrop=B panel=Q untracked_race_frames=F`

- [ ] Failing tests (format and options only): `test_wide_needs_scaled_check_and_fine_objects`; `test_wide_prints_its_lines_and_does_not_change_the_race`; `test_scaled_at_writes_the_wide_picture` (PPM header width `(320 + 78) * scale`).
- [ ] Implement; run the six recordings with `--scaled-check 3 --fine-objects --wide 39`; ledger the lines. **Pass:** `RESULT match`; `WIDE_CENTRE … differing_pixels=0`; `WIDE_GROUND examined > 0, grass > 0, wrong = 0`; `WIDE_SKY examined > 0, panel = 0` on race frames. `short_lines` should be of the order of the study's table (0 to 3.5% of ground lines). A `wrong` or `other` pixel is a fault to find (superpowers:systematic-debugging) before going on.
- [ ] Commit `rr-race-replay --wide: the wide picture checked on the recordings`.

---

### Task 10: Dashes run into the margins

**Files:** Modify `port/render/rr_dashes.h`, `rr_dashes.c`, `port/hw/rr_vdp_render.c` (strip clamp, 373-376); Test `port/tests/test_dashes.c`, `test_vdp.c`.

**Produces:** `void rr_dashes_strips_wide(const RrFineRoad *road, uint32_t place, const uint8_t *have, int scale, int margin, RrStrips *out);` `rr_dashes_strips` is it with margin 0. `from`/`to` stay counted from the centre picture's left edge (so may be negative or beyond `320 * scale`). `rr_dashes_lines` (the model of the game) does not change.

- [ ] Failing tests: `test_wide_strips_run_into_the_margins`; `test_wide_strips_equal_the_narrow_ones_in_the_centre`; `test_a_strip_is_drawn_in_the_margin` (vdp).
- [ ] Implement; recordings with `--dashes-check --wide 39`: `WIDE_CENTRE … dash=0`. Commit `Dash strips are cut at the wide view`.

---

### Task 11: Objects: a view other than the game's, and what became of each record

**Files:** Modify `port/render/rr_objects.h`, `rr_objects.c`; Test `port/tests/test_objects.c`.

**Produces:**
```c
typedef struct { int16_t left, right; } RrObjectView;  /* pictures are cut to x in [left, right) */
enum { RR_RECORD_DRAWN, RR_RECORD_OFF_VIEW, RR_RECORD_NO_ROOM, RR_RECORD_NO_PICTURE, RR_RECORD_HIDDEN };
enum { RR_OBJECT_RECORDS = 96 }; /* new: the most record slots the drawing looks at; check it against the game's table size and say so */
/* in RrObjectSet: */ uint8_t outcome[RR_OBJECT_RECORDS]; /* by record slot */
/* rr_objects_draw with another view and no tile budget (a fine picture needs no tiles):
 * the sprite limit is still 80. With {0, 0x140} and `tiles` 1 it is rr_objects_draw. */
int rr_objects_draw_view(const uint8_t *ram, const uint8_t *rom, size_t rom_size,
                         const RrObjectView *view, int tiles, RrObjectSet *out, uint8_t *work);
```
The game's "4 pixels short" rule (`VIEW_RIGHT - 4` in `edges()` and `centre()`, applied whatever the mirror bit) belongs to the game's view only: with another view the cut is at `right` exactly.

- [ ] Failing tests: `test_the_games_view_is_todays_drawing` (whole set `memcmp`, with `outcome` aside); `test_outcomes_are_recorded` (one record of each kind); `test_a_wider_view_keeps_a_picture_the_game_cut`; `test_a_wider_view_has_no_four_pixel_rule`; `test_no_tile_budget_in_a_view_without_tiles`.
- [ ] Implement; `--objects-check` on full-throttle unchanged (ledger before and after). Commit `Objects: drawing with another view, and each record's outcome`.

---

### Task 12: The margin object set

**Files:** Modify `port/render/rr_objects.h`, `rr_objects.c`; Test `port/tests/test_objects.c`.

**Produces:**
```c
/* The objects of the margins. `narrow` is the set drawn with the game's view from
 * `narrow_ram`; `wide_ram` holds the records to draw wide (stage 1: the same image;
 * stage 2: a copy with the port's own roadside records). Fills `out`, `table`
 * (RR_SPRITES_MAX * 8 bytes) and `fine` for the renderer's margin layer.
 * Returns out->ok. */
int rr_objects_margin(const RrObjectSet *narrow, const uint8_t *narrow_ram, const uint8_t *wide_ram,
                      const uint8_t *rom, size_t rom_size, int margin, int bottom,
                      RrObjectSet *out, uint8_t *table, RrFineSprites *fine, uint8_t *work);
```
Rules, by record slot:
- `RR_RECORD_DRAWN` in `narrow`, and its picture touches x = 0 or ends at 315 or beyond: the margin picture is the narrow `RrFinePicture` **uncut**: same y, `first_row`, `rows_shown` and art, `first_column` 0 and the full `columns`, x moved to match, so both halves sample the same art columns on the same lines. Its entries in `table` are moved and widened with it (a picture only shows inside its entry rectangle, `render_table_fine` 315-318).
- `RR_RECORD_OFF_VIEW` in `narrow`: drawn with the view `[-margin, 320 + margin)`; kept if it reaches a margin.
- `RR_RECORD_NO_ROOM`, `RR_RECORD_NO_PICTURE`, `RR_RECORD_HIDDEN`: left out.
- A record whose picture's top line is at or below `bottom` (0xFFF2B2, 152: the riders in the mirrors): left out.
- Copies: the game makes none for a cut picture and drops a straddling copy whole; a copy that the wide view would show is drawn in the margin only, cut hard at x = 0 or 320. Counted (Task 13).
- Margin pictures in the order of the records, at most 80; beyond that the farthest are left out and counted.

- [ ] Failing tests: `test_a_record_wholly_right_of_319_is_in_the_margin_set`; `test_a_record_wholly_in_the_centre_is_not`; `test_a_record_dropped_for_room_stays_dropped`; `test_a_left_cut_picture_is_the_narrow_one_uncut` (odd cut: column at x = -1 is the art column just left of the centre's first, on the same lines); `test_a_mirrored_picture_cut_at_the_right_is_uncut` (the narrow picture ends at 315 or 316: the margin picture covers from there on); `test_a_two_part_record_whose_first_part_is_off_the_narrow_view` (the second part stays on the narrow picture's lines; the first part is added above it); `test_a_record_below_the_view_is_left_out`; `test_an_entry_without_a_picture_is_left_out`; `test_a_straddling_copy_is_in_the_margin_only`; `test_a_second_ram_supplies_extra_records` (stage 2's shape: one more roadside record in `wide_ram`).
- [ ] Implement. Commit `Objects: the set that reaches the margins`.

---

### Task 13: The margin sprite layer, the seam, and the recordings

**Files:** Modify `port/hw/rr_vdp_render.c`, `port/render/rr_wide.h`/`.c`, `port/tools/rr_race_replay.c`; Test `port/tests/test_vdp.c`.

Behaviour: `margin_fine`'s table is drawn by the fine sprite code on lines `< wide->lines`, into margin columns, with its own per-line counters (not shared with the centre), composed with the planes by the same priority rule; soft pictures are softened. **Seam fill:** the margin layer may also draw in centre columns 316 to 319, and only where the centre's own sprite layer has no pixel of any object (this is where the game stopped a picture short of its own edge). Those pixels are counted as `object` by `rr_wide_centre`.

Tool: `choose_road` builds the margin set from the object tracker's chosen RAM image and sets `hires_margin_sprites`. New line:
`WIDE_OBJECTS frames=N margin_pictures=P uncut=U off_view=V copies=C left_out_room=R seam_pixels=S`.

- [ ] Failing tests: `test_margin_sprites_draw_only_in_the_margins`; `test_a_crowded_margin_does_not_cost_the_centre_a_sprite` (21 sprites on a line); `test_a_cut_picture_continues_without_a_seam` (left, columns -3..3 consecutive art columns); `test_a_mirrored_right_cut_continues_without_a_gap` (the narrow picture ends at 316: columns 312..323 consecutive); `test_the_seam_fill_never_covers_a_centre_object`; `test_margin_sprites_are_not_drawn_below_the_wide_lines`; `test_soft_margin_pictures`; `test_margin_sprites_keep_plane_priority`.
- [ ] Implement; six recordings: `WIDE_CENTRE … object=` equal to `seam_pixels`, `other=0`; `margin_pictures > 0`. Commit `Wide rows: a sprite layer for the margins, joined to the centre`.

---

### Task 14: In-between pictures

**Files:** Modify `port/render/rr_between.h`, `rr_between.c`; Test `port/tests/test_between.c`.

**Produces:** `RrBetween` gains `int margin;` (set by the owner; 0 = none), `RrObjectSet margin_set; uint8_t margin_table[RR_SPRITES_MAX * 8]; RrFineSprites margin_fine; int margin_ok; uint64_t margin_fell_back;`. `rr_between_objects` gains an output of each record slot's `fx`/`fy` (today private statics, `rr_between.c` 113) so that margin-only records get theirs. `rr_between_scene`, when `margin > 0`: `b->shifts.beyond_a/b` come from the scene's road, from the road on screen for the rows copied at 366-369, and are 0 on every line where `road_have` is 0 (including those cleared at 352); strips via `rr_dashes_strips_wide`; the margin set from `b->scratch`. If the margin set cannot be made the frame is still made, `margin_ok = 0`, counted.

- [ ] Failing tests (fixture `update_shows()`): `test_the_scene_at_an_update_has_that_updates_margin_set`; `test_margin_objects_move_between_updates` (x 340 → 344 is 342 half way, fraction in `fx`); `test_a_margin_set_that_cannot_be_made_leaves_the_frame`; `test_beyond_comes_with_the_road_on_screen_rows`; `test_beyond_is_zero_where_the_road_was_cleared`; `test_margin_0_scene_is_todays` (`memcmp`).
- [ ] Implement; `--inbetween blend --inbetween-check --wide 39` on full-throttle: `INBETWEEN` lines as before the task. Commit `In-between pictures carry the margins`.

---

### Task 15: The console's wide setting

**Files:** Modify `port/host/rr_console.h`, `rr_console.c`, `port/render/rr_objects.h`/`.c` (tracker); Test `port/tests/test_console.c`.

**Produces:**
```c
void rr_console_set_wide(RrConsole *c, int on);   /* off as opened; kept across a reset */
int rr_console_wide(const RrConsole *c);
int rr_console_scaled_width(const RrConsole *c);  /* pixels across of rr_console_scaled_frame */
uint64_t rr_console_wide_frames(const RrConsole *c);
```
Wide is **in effect** when the setting, the crisp road, the fine road mode and the fine objects are all on and the scale is above 1; the buffer is then `398 * scale` wide for every frame. `lines` = 152 on a frame whose RAM is a one-player race (`rr_road_read` succeeds), else 0: a menu or a two-player frame is the original between black margins. Session: event `wide <0|1>` (`EV_WIDE`, name `"wide"`, its own case in `apply_event`; the header stays `rr2-session 1`). One resize step, called from `set_scale`, `set_crisp_road`, `set_fine_road`, `set_fine_objects` and `set_wide`, reallocates the buffer; a failed allocation leaves the console as it was. The margin set is built once per update, when the object tracker notes a set (not every frame): the tracker keeps it beside the set (stage 2 hooks in here), and `choose_road` hands the chosen one, or the in-between scene's, to the frame loop.

- [ ] Failing tests: `test_wide_is_off_by_default_and_needs_crisp_fine_road_and_objects`; `test_wide_off_frames_are_todays` (`memcmp` over a made-up race); `test_the_centre_of_a_wide_race_frame_is_the_narrow_frame`; `test_a_menu_frame_is_the_original_between_black_margins`; `test_a_two_player_frame_is_the_original_between_black_margins`; `test_the_session_lists_wide_and_replays_it`; `test_switching_f_o_r_scale_or_wide_mid_race_never_draws_at_a_stale_width` (guard words; every combination, 20 frames each); `test_wide_survives_a_reset`; `test_the_margin_set_is_built_once_per_update`.
- [ ] Implement. Commit `The console's wide setting`.

---

### Task 16: Replay tool, Godot bridge, library

**Files:** Modify `port/tools/rr_session_replay.c`, `port/host/rr_godot.c`; Test `godot/tests/test_console.gd`.

**Produces** (GDScript): `set_wide(on: bool)`, `get_wide() -> bool`; `get_frame()` and `get_frame_width()` follow `rr_console_scaled_width`. `rr-session-replay` writes pictures at the console's width and prints it in its `PICTURE` line.

- [ ] Failing checks in `test_console.gd`: `get_wide()` false before and after loading; with scale 3, crisp, fine road, fine objects and wide on, `get_frame_width() == 1194` and `get_frame().size() == 1194 * 672 * 4`; the session text has `wide 1`.
- [ ] Implement. **Before rebuilding the library:** `pgrep -lf "[G]odot.app"`; if a window is open, stop and tell the owner. Then `make -C port godot-extension session-replay`.
- [ ] The three Godot test scripts → 0 FAIL. End to end: drive the library through a throwaway GDScript with wide on into a race for a few thousand frames, save session and RAM; `rr-session-replay --ram` → `RESULT reproduced`, picture 1194 wide and equal to the one Godot saw (ledger).
- [ ] Commit `The wide picture reaches Godot and the session replay tool`.

---

### Task 17: The window

**Files:** Modify `godot/scripts/console.gd`, `godot/shaders/crt.gdshader`; Test `godot/tests/test_console_scene.gd`. (`godot/project.godot` is not changed: the content scale is set at run time.)

**Produces:** `func set_wide(on: bool) -> void`, `func wide() -> bool`, `func picture_width() -> int` (320 or 398). Key **W**. Help line `W  Wide view: on|off|not available` in the group "(… only show with the crisp road on)". Note when pressed without what it needs: `Wide view — needs R, F and O on`. `Wide view: on|off` in the capture's `settings.txt`. `picture/wide` saved and restored with the other settings; in `_restore_settings` it is applied **after** the crisp road, fine road and fine objects.

Fit: the window's content scale size follows `picture_width()` x 224, so the picture is always at a whole-number factor; `set_wide`, like a resize, re-runs `set_scale(crisp_scale())`, and `crisp_scale()` divides by `picture_width()`. **Size:** the design says the wide view shows more "at the same size". In a window that is neither fullscreen nor maximised, turning W on makes the window `398 * k` wide and turning it off `320 * k`, where `k` is the factor it had (960 x 672 becomes 1194 x 672), so the picture does not shrink; fullscreen and maximised windows are left alone and may show the picture at a smaller factor, which the note then says (`Wide view on (2×)`). The tube is told the game size through a new shader uniform `game_size` replacing the literals at `crt.gdshader` 68 and 86; `scanlines` stays 224.

- [ ] Failing checks (`ScaleStub` gains `wide`, `set_wide`, `get_wide`, and a frame 398 * scale wide while wide and crisp): W toggles; the note says what it needs when R is off; the help line follows; `picture_width() == 398` and the texture is `398 * scale` wide; a windowed 960 x 672 window becomes 1194 x 672 and `get_scale()` stays 3; in a window held at 960 x 672 (the test marks it maximised) the scale becomes 2 and the note says so; `crisp_scale()` for 2388 x 1344 is 3; the saved setting comes back on a second scene with crisp restored first; the capture's settings say `Wide view: on`; `screen_size()` with wide off is as today.
- [ ] Implement. All three Godot scripts → 0 FAIL. Commit `W: the wide view in the window`.

---

### Task 18: Measure, look, write up

**Files:** Create `analysis/widescreen.md`; Modify `README.md` (one line under "What it does"), `godot/README.md` (the W key), `analysis/race-picture.md` (a pointer).

- [ ] For each of the six recordings: `rr-race-replay … --scaled-check 3 --fine-objects --dashes-check --inbetween blend --inbetween-check --wide 39`, and the same without `--wide`. Record `RESULT`, the four `WIDE_*` lines, and the fine-objects, dashes and in-between lines with and without.
- [ ] **Speed (requirement 7):** the tool's `--wide` draws every line twice, so it is not the window's cost. Time the console itself: `rr-session-replay` on one of the owner's captured sessions with a `wide 1` line added at frame 0 and without, `time` both, and report ms per frame.
- [ ] **Look at pictures.** With `--scaled-at`, write and view (Read tool) wide pictures of: the first race frame, the last, a frame the trackers fell back on, a crash, the bike far off the road at each side, a crest, a frame with a `beyond` line, a frame with a seam fill, a menu and the results screen. Anything wrong is a fault to find before the write-up.
- [ ] Count what stage 1 leaves: updates in which a roadside object the game culled would be visible in a margin (the study's `records.py` count, re-made or re-run), frames drawn wide from the game's own sprites, `no_edge_lines`, straddling copies, margin pictures left out for room.
- [ ] Write `analysis/widescreen.md`: each of requirements 1 to 8 with its measurement, marked verified or hypothesis; why a margin picture is legitimate (the RAM image is proven by the narrow set's byte match with video memory: a **hypothesis** that consecutive updates cannot be confused, since the tile base alternates); the limits; what stage 2 changes and its open question (the tracker keeps RAM as at 0xA4EE; the roadside walk needs its inputs as at 0x16E8C).
- [ ] `make -C port test`, `uv run --group dev pytest -q`, `uv run --group dev ruff check .` (9 known line-length findings in `tests/port/test_race_replay.py`), the three Godot scripts. Commit `Widescreen stage 1: measured on the six recordings`.

---

## Rulings already made in writing this plan

- **The centre is identical except for the seam fill.** The design allows a picture the original cut at its edge to continue. The game stops such a picture 4 pixels short of its own right edge (and drops one wholly inside those 4 pixels), which in a wide view would leave a slit; the margin layer fills columns 316 to 319 where the centre has no object pixel, and that is the only allowed difference, counted.
- **Straddling copies** appear in the margin only, cut hard at the old edge, as the game makes no centre half. Counted; stage 2 may revisit.
- **Menus and two-player with W on** are the original between black margins in a wide buffer, so the picture does not change size between a menu and a race.
- **Panels are black** (0x000000).
- **A line with no known edge whose mark falls inside a margin** gets grass continued through the margin; such lines are counted (`no_edge_lines`).
- **W resizes a windowed window** to keep the picture the same size (the design's "at the same size"); fullscreen and maximised windows are left alone.

## What the review changed

- **Ground model (critical).** The first version assumed one edge-line mark per plane; every ground row has one on each side of the block. The model above is rewritten from `planes.txt`; the gate after Task 2 has its outcomes decided in advance.
- **Measurements (critical).** The checks are now library functions tested in C against deliberately wrong pictures, expect grass from the road's own edges instead of trusting plane columns, report counts that must be above zero, and cover sky margins and untracked frames. Python tests are limited to what the fixture can show.
- **Seams.** Mirrored pictures cut at the right end at 315 or 316; margin pictures are the narrow ones "uncut" (not a `dx` nudge), two-part records keep the narrow picture's lines, and the seam fill closes the last 4 pixels.
- **The margin set's rules** now rest on a recorded outcome per record; margins have no tile budget; copies are ruled on.
- **Which copy of the block:** from the unwrapped shift, or the copy that fits the game's clamps; the continuation formulas are corrected.
- **Sky rows** cannot be mistaken for ground: the block must be opaque right across, and the gate checks the recordings for rows that pass anyway.
- **The tool draws margin objects** on the recordings (`WIDE_OBJECTS`), and the constants live in one place (`rr_wide_blocks`).
- **The window** keeps the picture's size, restores wide after the settings it needs, and `project.godot` is left alone.
- **Speed** is timed on the console, not the tool. **Order:** the road's `beyond` comes before the ground continuation; three large tasks are split.
