# Fine Road Points Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the far-edge wobble of the crisp road by placing the 32 road points with fractions of a pixel instead of on whole pixels, while still never drawing a road that is not the one on screen.

**Architecture:** The game projects each road point as world position × scale ÷ 65,536 and throws the remainder away. The port repeats that projection in C from the same inputs (camera, track bytes), noted at the moment the game projects, and keeps the remainder. Proof that it is the same projection: with the remainder dropped it must equal the game's own 32 points, checked for every pass. The crisp road's per-row shifts are then taken from the fine points. Because the fine edge no longer has the game's whole-pixel value on row 0, the "sampled back" equality of the previous plan cannot hold on road lines; it is replaced by two measurements: equality everywhere off the road lines, and how far the fine edge sits from the game's whole-pixel edge. The whole-pixel path stays as a selectable mode and as the fallback.

**Tech Stack:** C11 (`port/`), Python 3 with pytest and ruff (`tests/port`), GDScript only if a test needs it, the `full-throttle` recording under `generated/port/handoffs/`.

**Spec:** `docs/superpowers/specs/2026-11-11-draw-data-contract.md`, "Road" form 2a and "State after the first renderer step" (first item of "Left for the next plans"). Evidence and arithmetic: `analysis/race-picture.md` sections 3.1, 4.1, 5; `src/rr2port/scene.py` (`project_road`, checked equal to the game on 2,175 of 2,175 clean images) is the reference to port. **Not in this plan:** using the camera's full 16-bit fraction for distance (the game uses its top 8 bits; the finer value moves the road between passes and belongs with interpolation), curve smoothing between points, the leaning grass speckles, objects, dashes.

## Global Constraints

- Nothing derived from the ROM is committed; pictures, dumps and RAM images stay under ignored `generated/` (and `godot/bin/`). Test fixtures are synthetic.
- Every claim in `analysis/` is labelled **verified** (with the measurement) or **hypothesis**, and says what the measurement does not cover.
- Nothing may change what the game computes: with the new options on, `full-throttle` must still end in `RESULT match` (2,176 of 2,176).
- The reference is the port at 100% CPU speed. No claim about a real console or other speeds.
- Never a wrong road: fine points are used only for a road whose whole-pixel shift table equals the one in video memory, and only on a line whose table entry is unchanged when the line is drawn (both checks exist; they must keep working on the whole-pixel values).
- The default picture stays the original 320×224; the crisp road stays behind the R key.
- Do not rebuild `godot/bin/librr2.dylib` while a Godot window may be using it; say so in the conversation before the rebuild in Task 3.
- Commit only where a task's commit step says. Work on branch `fine-road`, created from `main`.
- Writing for the owner is in plain terms.
- Known facts: the one-player projection is entered at `0x14A86` (which calls `0x14B44`); it reads the camera segment `0xFF0A0A` and fraction `0xFF0A0C`, sideways position `0xFF0A02`, camera height `0xFF0A08`, count `0xFF1CA0`, the track ring `0xFFD828`, view top/bottom `0xFFF2B0`/`0xFFF2B2`, horizon reference `0xFFF2BC`, centre `0xFFF2BE`, and the perspective table at ROM `0x27C84` (16,384 words). The road points are turned into the picture from `0x14566` on (`RR_ROAD_NOTE_PC`).

## Review Focus

1. **The fine points belong to a different road than the one on screen.** Inputs are noted at projection time and the points a pass or two later. Expected: a set of inputs is attached to a noted road only if projecting it, remainder dropped, gives exactly that road's 32 points; otherwise that road is drawn from its whole-pixel points as before. Test in Task 2 (`test_inputs_that_project_to_other_points_are_not_used`).
2. **Hills.** A nearer crest hides farther road; the game handles it with a "clipped line". Expected: fine rows hidden by a crest get the nearer segment's edge, as the whole-pixel path does, and a line with no road still has no shift. Test in Task 2 (`test_a_crest_hides_the_road_behind_it`).
3. **The line guard.** The per-line check in `rr_genesis.c` compares row 0's whole part with the scroll table; fine shifts differ from the table by design. Expected: the guard compares the *whole-pixel* value for that line, kept beside the fine shifts, so a table rewritten mid-frame still makes the line plain. Test in Task 3 (the existing `test_a_line_whose_table_entry_changed_after_the_choice_is_drawn_plainly`, run in fine mode too).
4. **Negative and clamped values.** The game rounds products toward zero, clamps a line above the view to its top, and limits the edges (−0x160..0x140, 0..0x180). Expected: dropping the remainder reproduces the game for negative world values, and a clamped edge stays clamped on every row. Tests in Task 1 (`test_dropping_the_remainder_gives_the_whole_points`) and Task 2 (`test_clamped_edge_stays_clamped_on_every_row_fine`).
5. **A straight road must be straight.** The point of the change. Expected: for a flat, straight track, every row's edge lies on one line to within one output pixel; with whole points it does not. Test in Task 2 (`test_a_straight_road_has_straight_edges`).

---

### Task 1: The projection in C, with the remainder kept

**Files:**
- Modify: `port/render/rr_road.h`, `port/render/rr_road.c`
- Test: `port/tests/test_road.c`

**Interfaces:**
- Produces:
  ```c
  typedef struct {
      int count;                                   /* road points, at most RR_ROAD_POINTS */
      uint16_t fraction;                           /* camera's place within its segment */
      int16_t sideways, height;                    /* player's sideways position, camera height */
      int16_t top, bottom, reference, centre_x;    /* view lines, horizon reference, screen centre */
      int8_t curve[RR_ROAD_POINTS + 1], hill[RR_ROAD_POINTS + 1];  /* the track from the camera on */
  } RrRoadInputs;
  int rr_road_read_inputs(const uint8_t *ram, RrRoadInputs *out);   /* 0 if not a one-player race */

  typedef struct {
      int count;
      int32_t line[RR_ROAD_POINTS], clipped_line[RR_ROAD_POINTS];   /* 16.16 */
      int32_t left[RR_ROAD_POINTS], right[RR_ROAD_POINTS];          /* 16.16, relative to the centre */
      int16_t centre_x, view_bottom;
  } RrFineRoad;
  /* The game's projection (0x14B44) with the remainders of its multiplications kept. `rom`
   * must hold the perspective table; returns 0 if it is too short. */
  int rr_road_project(const RrRoadInputs *in, const uint8_t *rom, size_t rom_size, RrFineRoad *out);
  /* The fine road with every remainder dropped as the game drops it: what the game's own
   * arrays hold. horizon_line is filled as rr_road_read fills it. */
  void rr_road_whole(const RrFineRoad *fine, RrRoad *out);
  ```
- Rule for the remainder: where the game computes `v × scale >> 16` on the magnitude and restores the sign, the fine value is sign × (|v| × scale) as 16.16; the game's value is that with the low 16 bits of the magnitude dropped. A line clamped to the view top has no remainder. `clipped_line` is the running minimum of the fine lines under the game's condition (distance at least one segment), starting at the view bottom.

- [ ] **Step 1: Write the failing tests**, synthetic inputs and a synthetic perspective table (scale = 0x10000 × 64 ÷ (distance + 64), say; any smooth table):
  - `test_dropping_the_remainder_gives_the_whole_points`: for a track with a bend, a rise, a negative sideways position and a camera fraction of 0x4000, `rr_road_whole(rr_road_project(...))` equals values worked independently in the test by the rule of `scene.py`'s `project_road` (write that rule out in the test as plain integer arithmetic for three points; do not call the code under test).
  - `test_the_remainder_is_less_than_one_pixel_toward_zero`: every fine value differs from its whole value by less than 0x10000 and has the same sign.
  - `test_a_line_above_the_view_is_clamped_without_a_remainder`.
  - `test_inputs_are_refused_outside_a_race_and_with_a_short_rom`.
- [ ] **Step 2: Run** `make -C port test`. Expected: build fails, `RrRoadInputs` undeclared.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `make -C port test`. Expected: `all road tests passed`, other suites pass.
- [ ] **Step 5: Commit**: "Project the road in C with the remainders kept".

### Task 2: Shifts from the fine points, and which inputs belong to which road

**Files:**
- Modify: `port/render/rr_road.h`, `port/render/rr_road.c`
- Test: `port/tests/test_road.c`

**Interfaces:**
- Consumes: Task 1's types.
- Produces:
  ```c
  /* As rr_road_shifts, from fine points: for each row, the edge is taken on the straight run
   * between the two points whose fine lines it lies between. out->have and row 0's *whole*
   * values are not taken from here: see RrRoadTracker. */
  void rr_road_shifts_fine(const RrFineRoad *road, int horizon_line, int scale, RrRoadShifts *out);
  enum { RR_ROAD_WHOLE, RR_ROAD_FINE };
  ```
  `RrRoadTracker` gains: `int mode` (default `RR_ROAD_FINE`); the last four `RrRoadInputs` noted; `RrRoadShifts whole` (the game's whole-pixel shifts for the chosen road, which the caller's line guard uses) beside `shifts` (what is drawn); counters `fine_frames` and `whole_frames` (their sum is `crisp_frames`).
  ```c
  enum { RR_ROAD_INPUTS_PC = 0x14A86 };                     /* the game is about to project the road */
  void rr_road_tracker_note_inputs(RrRoadTracker *t, const uint8_t *ram);
  ```
  `rr_road_tracker_note` (at `RR_ROAD_NOTE_PC`) attaches to the road it notes the newest noted inputs whose projection, remainder dropped, equals that road's points exactly, or none. `rr_road_tracker_choose` picks the road by the whole-pixel match as now; if that road has inputs and the mode is fine, `shifts` come from `rr_road_shifts_fine` with `have` copied from `whole`; otherwise `shifts` equal `whole`.

- [ ] **Step 1: Write the failing tests**:
  - `test_a_straight_road_has_straight_edges`: flat straight track, camera off-centre, scale 4. Fit a line through the first and last row of the left edge between two far points; every row between is within one output pixel (0x4000) of it with fine points. The same measure with `rr_road_shifts` on the whole points exceeds it on at least one row (this is the wobble; if it does not, choose inputs that show it).
  - `test_a_crest_hides_the_road_behind_it`: a rise then a fall so that a far point's line is below a nearer one's; rows between take the nearer run and no row uses the hidden point.
  - `test_clamped_edge_stays_clamped_on_every_row_fine`.
  - `test_fine_and_whole_edges_differ_by_less_than_two_pixels`: on row 0 of every line with a shift, for the bend-and-rise track of Task 1.
  - `test_inputs_that_project_to_other_points_are_not_used`: note inputs, change the sideways position in RAM without re-projecting, note the road: the tracker's choice is the whole-pixel shifts and `whole_frames` advances, not `fine_frames`.
  - `test_matching_inputs_give_fine_shifts_and_the_whole_table_for_the_guard`: `fine_frames` advances; `whole` row 0 equals the video table; `shifts` differ from `whole` somewhere.
  - `test_whole_mode_draws_the_whole_points`: mode `RR_ROAD_WHOLE` gives `shifts` identical to `whole`.
- [ ] **Step 2: Run** `make -C port test`. Expected: build fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `make -C port test`. Expected: all suites pass.
- [ ] **Step 5: Commit**: "Take the crisp road's shifts from fine road points".

### Task 3: Wire it in, measure, and look

**Files:**
- Modify: `port/hw/rr_genesis.h`, `port/hw/rr_genesis.c` (the line guard reads a separate array of expected whole values), `port/host/rr_console.h`, `port/host/rr_console.c`, `port/tools/rr_race_replay.c`, `port/host/rr_godot.c` only if a method is added
- Test: `port/tests/test_console.c`, `tests/port/test_race_replay.py`
- Modify: `analysis/race-picture.md` (new section 6), `docs/superpowers/specs/2026-11-11-draw-data-contract.md` ("State after…"), `godot/README.md`

**Interfaces:**
- `RrGenesis` gains `const int32_t *hires_expect_a, *hires_expect_b;` (per line, row 0 index as the shift arrays): the whole-pixel shifts the line guard compares with the table. When NULL the guard compares the drawn shifts as now.
- `rr_console.h`: `void rr_console_set_fine_road(RrConsole *c, int on); int rr_console_fine_road(const RrConsole *c);` (default on; kept across a reset) and `uint64_t rr_console_fine_frames(const RrConsole *c);`. The console notes inputs at `RR_ROAD_INPUTS_PC` in the same step hook.
- Tool: `--crisp-points whole|fine` (default fine) with `--scaled-check`. The `SCALED` line becomes `SCALED scale=S points=fine|whole frames=N off_road_identical=M sampled_back_identical=B crisp_frames=K fine_frames=F race_frames=R edge_lines=L within_1px=W max_edge_difference=D run_ms_per_frame=T`, where *off road identical* counts frames in which every line without a shift is the plain enlargement; *sampled back identical* is the previous plan's check (expected to be all frames only in whole mode); *edge lines* counts (line, plane) pairs drawn with a fine shift, *within 1px* those whose row-0 fine shift is less than one pixel from the whole-pixel value, and *max edge difference* the largest such difference in 1/65536 pixels.

- [ ] **Step 1: Write the failing tests.** `test_console.c`: run the mid-frame table-change test in both modes; `test_fine_road_is_on_by_default_and_counts_frames` (the race console with inputs noted: `rr_console_fine_frames` advances; with `rr_console_set_fine_road(c, 0)` it does not and the picture equals the previous plan's). `test_race_replay.py`: `test_scaled_check_reports_points_mode_and_edge_measures` (synthetic fixture: fields present, `points=fine`, zero edge lines) and `test_crisp_points_needs_scaled_check_and_a_known_value`.
- [ ] **Step 2: Run** `make -C port test; uv run pytest tests/port/test_race_replay.py -q`. Expected: fail (functions and option missing).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `uv run pytest -q && uv run ruff check . && make -C port test`. Expected: all pass.
- [ ] **Step 5: Measure** on `full-throttle` at scale 4, both modes. Record in section 6 of `analysis/race-picture.md`, **verified**: `RESULT match` in both; in whole mode the previous plan's numbers unchanged (12,843 sampled back, 11,823 crisp); in fine mode off-road identical frames (target: all), fine frames out of crisp frames (target: at least 99%; if lower, find out why from a sample and record it, moving the input note point if that is the cause, as a ruling), the share of edge lines within one pixel and the largest difference.
  **Stop rule:** if the largest difference is 2 pixels or more on more than a handful of lines, do not explain it away: find the lines, compare with `scene.py`'s per-line arithmetic, and report what the cause is (the game's reciprocal-table rounding on long runs is the expected one) with the numbers.
- [ ] **Step 6: Look.** Write frames 1518, 4000 and 10261 in both modes with `--scaled-at`, make side-by-side crops of the far road, and describe in section 6 what changed and what did not (the leaning speckles are not expected to change). Tell the owner where the pictures are.
- [ ] **Step 7: Say in the conversation that the Godot library is about to be rebuilt**, check no Godot process is running, run `scripts/build-godot-extension.sh`, then the Godot headless tests through `uv run pytest tests/port/test_godot_extension.py -q`. Expected: pass.
- [ ] **Step 8: Update** the contract's "State after…" section (far wobble: done or what remains) and `godot/README.md` if the R key's description changes.
- [ ] **Step 9: Commit**: "Draw the crisp road from fine road points".
