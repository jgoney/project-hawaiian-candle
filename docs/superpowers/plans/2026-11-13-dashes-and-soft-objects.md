# Centre-line Dashes and Softer Far Objects Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the centre-line dashes move with the in-between road instead of stepping at the game's rate, and offer a softer drawing of shrunken (far) objects on a key.

**Architecture:** The game describes the centre line per screen line (a start x and a width), blanks the lines of every other track segment, and packs the rest into sprites. The port repeats the per-line description from its own road points and checks it against the game's memory; it then draws the dashes as strips of the enlarged picture directly from the fine (and in-between) road, and leaves the game's dash sprites out of the sprite list. Softer far objects: where a full picture is drawn smaller than its natural size, each output pixel takes the average of the picture pixels it covers instead of one of them.

**Tech Stack:** C11 (`port/`), Python 3 with pytest via `uv`, Godot 4 GDExtension.

**Spec:** `docs/superpowers/specs/2026-11-12-interpolation-design.md`, stage 3 (the dashes); the owner's note of 2026-11-11 that far objects are "almost too crisp" (softening: "a separate small item" in that design's non-goals).

## Global Constraints

- The game is untouched: `rr-race-replay` ends in `RESULT match` with every new option on. New code only reads.
- With the new things off, every frame is byte for byte what the current build draws. The dashes' new drawing is on only where the fine road is drawn; the softening is off unless switched on.
- Never a wrong picture: a frame whose dashes cannot be described (the port's per-line description does not equal the game's memory for the update on screen) keeps the game's dash sprites.
- Nothing derived from the ROM is committed; test data is synthetic.
- Every claim in `analysis/` is marked **verified** or **hypothesis**; nothing is called "the same" without a measurement.
- Do not rebuild `godot/bin/librr2.dylib` while a Godot window may be using it; say so to the owner before the final rebuild.
- Work on branch `dashes`. Commit as the tasks say. Merging and pushing are the owner's decision.
- Tests: `make -C port test`, `uv run pytest -q`; both green at the end of every task. After editing a C source, `touch` it before `make` if the build was less than a second ago (a stale binary passed for a failing test once).

## What was read from the game's code (hypotheses until Task 1 measures them)

- `0x14790`–`0x1481A` builds four per-line arrays from per-point arrays with `0x153EA` (the routine section 3.1 describes): left edge `0xFF27DE`, right edge `0xFF2D1E`, and a third, `0xFF2B5E`, from the per-point array at `0xFF2078`.
- `0x14822` (the loop that makes the per-line shifts) also writes, for each road line: **width** (`0xFF309E`) = max(1, (3 × (right − left) ÷ 2) >> 6), computed as the code does it (`half = (right − left) >> 1` arithmetic; `(right_plus_centre − left_plus_centre + half) >> 6` logical, 16-bit); **start x** (`0xFF2EDE`) = the third array's value for the line; a start of 0x140 or more gives start 0 and width 0; a negative start is added to the width and set to 0, and a width that goes negative becomes 0. Lines above the road get 0 and 0.
- `0x12768` walks the bands between neighbouring road points and, for the bands of every other track segment (by bit 0 of the camera's segment number plus the band's index), sets the width to 0. It also picks one of two 16-entry road-surface patterns (`0x12668`, `0x12728`) for the band; how that pattern reaches the screen is **not traced** and not part of this plan.
- `0x13F20` then packs the non-zero lines into sprites of colour 12 in colour set 3 (first read as 13; the game's pattern table at `0x13EF4` and Task 3's measurement say 12), low priority (attribute `0x6000`), eight lines at a time, consuming the arrays as it goes. Its entries follow the objects' in the sprite list.

## Review Focus

1. **A hill crest hides part of a dash.** Expected: the strip stops where the road does (clipped lines), as the game's does.
2. **The centre line leaves the screen on a sharp bend.** Expected: no strip, no wrap-around to the other side.
3. **More sprites after the objects than the dashes** (something else the game adds). Expected: only entries proven to be dashes are left out; anything else is still drawn.
4. **The softening switched on with objects at natural size or larger.** Expected: exactly today's picture for those objects.
5. **A shrunken object against the sky or another object.** Expected: its edge pixels blend only with the picture's own colours (no dark or bright fringe), and a pixel less than half covered shows what is behind.

---

### Task 1: The game's per-line dash description, repeated and measured

**Files:**
- Create: `port/render/rr_dashes.h`, `port/render/rr_dashes.c`, `port/tests/test_dashes.c`
- Modify: `port/Makefile`, `port/render/rr_road.h`, `port/render/rr_road.c` (the road model gains the centre line), `port/tools/rr_race_replay.c`, `tests/port/test_race_replay.py`

**Interfaces:**
- Produces, in `RrRoad`: `int16_t centre[RR_ROAD_POINTS];` and in `RrFineRoad`: `int32_t centre[RR_ROAD_POINTS];` (the per-point centre-line x the game interpolates for the dashes, relative to the screen centre like the edges; read from RAM by `rr_road_read` and produced by `rr_road_project`). Which RAM array and which projected quantity it is, is settled by Step 1's measurement.
- Produces:

```c
typedef struct {
    int ok;
    int16_t start[RR_SCREEN_H], width[RR_SCREEN_H]; /* per screen line; width 0: no dash */
} RrDashLines;

enum { RR_DASHES_PC = 0x13F20 }; /* the game is about to pack the dashes into sprites */

/* The per-line centre-line description the game holds at RR_DASHES_PC, made
 * from the road points (whole pixels, as the game has them) and the camera's
 * segment number: 0x14822's start and width, then 0x12768's blanking of every
 * other segment's band. ok = 0 if the road is not usable. */
void rr_dashes_lines(const RrRoad *road, uint32_t place, const uint8_t *rom, size_t rom_size,
                     RrDashLines *out);
/* The same two arrays as they stand in work RAM (0xFF2EDE, 0xFF309E). */
void rr_dashes_read(const uint8_t *ram, RrDashLines *out);
```

- Produces: replay option `--dashes-check`, printing `DASHES passes=N identical=M first_difference=K|none sprites_after_objects=A dash_sprites=D`.

- [ ] **Step 1: Measure before modelling.** With `--ram-at-pc 13F20:0:2176:generated/port/draw/before-dashes.bin` take the images, and with a throwaway script (scratchpad) confirm or correct each formula in "What was read" against the arrays in every image: the width formula; that the start is the interpolation of the per-point array at `0xFF2078` (and what that array equals in terms of the projection: centre, or centre minus half the width); the blanking rule's parity. Write the confirmed formulas into the header comment of `rr_dashes.h`. Expected: every non-zero line of at least 99% of images explained; the rest classified (as section 4.1 did for images caught mid-projection).
- [ ] **Step 2: Write the failing tests** in `test_dashes.c` (synthetic road as in `test_road.c`):

```c
static void test_width_is_three_halves_of_the_road_over_64(void);   /* road 640 wide on a line: width 15; 20 wide: 1 (never 0 on a road line of an "on" segment) */
static void test_start_follows_the_centre_line(void);               /* centre at x on two points: every line between them starts where 0x153EA's run puts it */
static void test_every_other_segment_is_blank(void);                /* camera in segment 4 and in segment 5: the on and off bands swap */
static void test_a_start_off_the_right_is_no_dash(void);            /* start >= 0x140: start 0, width 0 */
static void test_a_start_off_the_left_is_cut(void);                 /* start -3, width 5: start 0, width 2; start -9, width 5: width 0 */
static void test_lines_above_the_road_have_no_dash(void);
static void test_a_crest_hides_the_dash_behind_it(void);            /* clipped lines: no line gets a dash from a point hidden behind a nearer one */
static void test_read_returns_the_arrays_in_ram(void);
static void test_wild_road_is_refused(void);                        /* count 0, count 33, lines out of order by 30000: ok = 0 or all widths 0; no write outside the struct */
```

- [ ] **Step 3:** `make -C port test` → FAIL (no such file).
- [ ] **Step 4:** Implement `rr_dashes.c` and the centre line in the road model; existing road tests still pass (the tracker's `memcmp` of whole roads now includes the centre: `rr_road_read` and `rr_road_project` must agree on it, which is itself a check).
- [ ] **Step 5:** `make -C port test` → all suites pass.
- [ ] **Step 6:** Add `--dashes-check` (failing pytest first: option prints the line; refused with `--resync`). At `RR_DASHES_PC` it compares `rr_dashes_lines` (from the road points and place in RAM) with `rr_dashes_read`; at the objects' done point it notes how many sprites the game then adds and how many of them have attribute `0x6000 + tile`.
- [ ] **Step 7:** Run on `full-throttle`. Expected: `RESULT match`; `identical` equal to `passes` or every exception classified; `dash_sprites` equal to `sprites_after_objects` (if not, the difference is a finding: what else is there).
- [ ] **Step 8:** Commit: `git add port tests && git commit -m "The game's per-line dash description, repeated in C and measured"`.

---

### Task 2: Draw the dashes as strips of the enlarged picture

**Files:**
- Modify: `port/hw/rr_vdp.h`, `port/hw/rr_vdp_render.c`, `port/render/rr_dashes.h/.c`, `port/render/rr_between.h/.c`
- Test: `port/tests/test_vdp.c`, `port/tests/test_dashes.c`, `port/tests/test_between.c`

**Interfaces:**
- Produces, in `rr_vdp.h`:

```c
/* Strips drawn with the sprites: on output row q (0 .. RR_SCREEN_H * scale - 1)
 * output columns [from[q], to[q]) show `colour` (a palette entry 0..63) at low
 * priority, behind every sprite in the table; from >= to is no strip. */
typedef struct {
    int scale;
    uint8_t colour;
    int32_t from[RR_SCREEN_H * RR_SCALE_MAX], to[RR_SCREEN_H * RR_SCALE_MAX];
} RrStrips;
```
  and in `RrFineSprites`: `const RrStrips *strips;` (NULL: none) and `uint8_t skip[RR_SPRITES_MAX];` (1: this table entry is not drawn; it still counts in the per-line limits, so that leaving the dash sprites out changes nothing else).
- Produces, in `rr_dashes.h`:

```c
/* The dashes of a fine road as strips at `scale`: for each row of each road
 * line of an "on" segment, from the centre line's start to start + width,
 * both evaluated on that row from the fine points (the width from the fine
 * edges), cut at the screen's sides and by the same clipped lines as the
 * game's. */
void rr_dashes_strips(const RrFineRoad *road, uint32_t place, int scale, RrStrips *out);
/* Which entries of a sprite table are the game's dashes: entries from `first`
 * on whose attribute word is 0x6000 + a tile at or after `tile_base`. */
void rr_dashes_mark(const uint8_t *table, int first, uint16_t tile_base, uint8_t *skip);
```
- `RrBetween` gains `RrStrips strips;` filled by `rr_between_scene` from the in-between road and place, with `fine.strips` pointing at it and `fine.skip` marking the dash entries of its table.

- [ ] **Step 1: Failing tests.**
  - `test_vdp.c`: `test_strips_draw_behind_sprites_and_under_high_priority_planes` (a strip under a sprite pixel shows the sprite; under a high-priority plane pixel shows the plane; on the backdrop shows the strip's colour); `test_a_skipped_entry_is_not_drawn_but_still_counts` (21 sprites on a line with the first skipped: the 21st is still dropped); `test_no_strips_and_no_skips_is_todays_picture` (all 224 lines equal with the new fields zero).
  - `test_dashes.c`: `test_strips_at_scale_1_are_the_whole_pixel_dashes` (on a road whose points have no fractions, `rr_dashes_strips` at scale 1 gives from = start, to = start + width for every line of `rr_dashes_lines`); `test_strips_move_by_rows` (on a slope, row 1 of a line starts between line's and next line's start); `test_strips_are_cut_at_the_screen_edges`; `test_mark_takes_only_dash_entries` (an entry after the objects with another attribute is not marked).
  - `test_between.c`: `test_dashes_move_between_updates` (two updates half a segment apart: at t = 0x8000 the first strip row of the nearest dash lies between where it is at t = 0 and t = 0x10000); `test_the_scene_at_an_update_has_that_updates_dashes` (at t = 0x10000 the strips equal `rr_dashes_strips` of the tracker's fine road).
- [ ] **Step 2:** `make -C port test` → FAIL to compile.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** `make -C port test` → pass; `uv run pytest -q` → pass.
- [ ] **Step 5:** Commit: `git commit -am "Dashes drawn as strips from the fine road"`.

---

### Task 3: Dashes in the console and the replay tool; measured

**Files:**
- Modify: `port/host/rr_console.c`, `port/tools/rr_race_replay.c`, `port/tests/test_console.c`, `tests/port/test_race_replay.py`, `analysis/race-picture.md` (section 9), `godot/README.md`, `README.md`

**Interfaces:**
- Consumes: Task 1 and 2. No new public console function and no new key: the strips are used on every frame the fine road is drawn and the frame's dash description is proven (`rr_dashes_lines` of the road chosen equals a description noted at `RR_DASHES_PC`; the console notes it there as it notes the objects), both with in-between pictures and without. Otherwise the game's dash sprites are drawn as today.
- Replay: with `--scaled-check S --fine-objects` (and optionally `--inbetween`), add to the output `FINE_DASHES frames=N strip_frames=M sprite_frames=K`, and with `--inbetween-check` add `dash_still` / `dash_moving` to `INBETWEEN_MOTION` (the first strip row of the nearest dash).

- [ ] **Step 1: Failing tests.** `test_console.c`: `test_dashes_are_strips_when_the_fine_road_is_drawn` (a synthetic race with one dash band and its sprite in video memory: the dash's edge on row 1 of a sloping line is not where row 0's is); `test_dashes_stay_sprites_when_the_description_is_not_proven`; `test_dashes_slide_with_in_between_pictures` (predict: the dash's first row is lower on the frame after an update than on the frame of it). Python: the `FINE_DASHES` line appears with zero strip frames on the synthetic program, and the race is unchanged.
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement. **Step 4:** all tests pass.
- [ ] **Step 5:** Measure on `full-throttle`, scale 3, blend and predict at 100% and blend at 200%: `RESULT match`; strip frames against race frames; dash still/moving against today's. Dump frames 4000–4007 in blend, make the contact sheet, and look: dashes slide, sit on the road's centre, none doubled (a strip and a sprite of the same dash).
- [ ] **Step 6:** Write section 9 of `analysis/race-picture.md` (what the dashes are, Task 1's and this task's measurements, the road-surface pattern as **not traced**, limits), a summary line, and the README lines.
- [ ] **Step 7:** Commit: `git add port tests analysis README.md godot/README.md && git commit -m "Dashes follow the fine and in-between road"`.

---

### Task 4: Softer far objects, on a key

**Files:**
- Modify: `port/hw/rr_vdp.h`, `port/hw/rr_vdp_render.c`, `port/host/rr_console.h/.c`, `port/host/rr_godot.c`, `godot/scripts/console.gd`
- Test: `port/tests/test_vdp.c`, `port/tests/test_console.c`, `godot/tests/test_console.gd`, `godot/tests/test_console_scene.gd`
- Modify: `godot/README.md`, `analysis/race-picture.md` (a paragraph in section 7)

**Interfaces:**
- Produces, in `RrFineSprites`: `uint8_t soft;` — 0: as today. 1: where a picture's `size` is below 0x10000, an output pixel's colour is the average (in RGB) of the picture pixels its square covers, counting only pixels that are not see-through; if fewer than half of the covered pixels are see-through-free (coverage under one half), the output pixel is see-through. Pictures at 0x10000 or more are drawn exactly as today.
- Produces: `void rr_console_set_soft_objects(RrConsole *c, int on); int rr_console_soft_objects(const RrConsole *c);` (default off); Godot `set_soft_objects(on: bool)`, `get_soft_objects() -> bool`; key **B** toggles, with the messages "Far objects softened" / "Far objects sharp".

- [ ] **Step 1: Failing tests.** `test_vdp.c`: `test_soft_half_size_picture_averages_four_pixels` (a 4 × 4 picture of two colours in a checker at size 0x8000, scale 1: every output pixel is the mean of the two colours' RGB); `test_soft_leaves_natural_and_larger_pictures_alone` (sizes 0x10000 and 0x20000: all lines equal with soft 0 and 1); `test_soft_edge_under_half_covered_is_see_through` and `..._over_half_takes_only_the_pictures_colours` (a picture with a see-through column: the output pixel that covers one opaque and three see-through pixels shows the backdrop; one that covers three opaque and one see-through is the mean of the three); `test_soft_keeps_order_and_priority` (a soft pixel behind a high-priority plane pixel shows the plane). `test_console.c`: off by default; on changes the object's lines in the synthetic race with a half-size object; kept across reset. Godot tests: round-trip, key B messages.
- [ ] **Step 2:** Run → FAIL to compile. **Step 3:** Implement (the sprite layer gains an RGB value per output pixel beside its palette index; the line's final colour takes it where the sprite layer wins). **Step 4:** all tests pass.
- [ ] **Step 5:** Measure on `full-throttle` with the softening on: `RESULT match`; `off_road_identical` unchanged (nothing outside the object sprites changes); time per frame. Dump frame 4000 sharp and soft, crop the far trees, look.
- [ ] **Step 6:** Tell the owner, check no Godot is running (`ps`), rebuild the library, run the Godot tests. Update `godot/README.md` (key B) and the analysis paragraph (what it does, that the averaged colours are not in the game's palette, by eye only).
- [ ] **Step 7:** Commit: `git add port godot analysis && git commit -m "Softer far objects on a key"`.

---

## After the last task

One whole-branch review by a fresh reviewer on the most capable model, one fix pass, the final message with "Rulings I made" and "Deferred minors", then the finishing menu. The owner then looks: dashes sliding in blend at 100% and 200%, and B for the softened far objects.
