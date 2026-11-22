# Objects Traced and a Crisp Road Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** (a) Find out exactly how a draw record becomes sprites on screen (picture format, scaling, placement, cutting into sprites) and prove it by predicting what the game stages; (b) show the race in the Godot window at a higher resolution with road edges drawn as smooth slopes instead of staircases, the original grass pattern kept, and the original objects laid over.

**Architecture:** Part (a) is research with the same proof method as the road: a Python reader repeats the game's arithmetic from a work-RAM image and the ROM, and a checker compares the result with what the game staged, for equality. It needs cleaner RAM images than the handoff ones, so the replay tool learns to take an image when the main loop reaches a chosen instruction. Part (b) does not replace the video-chip model; it generalises its line renderer: each original line is drawn as S sub-lines, and on the lines that show the road, planes A and B are given a fractional sideways shift per sub-line, computed from the game's 32 road points. Everything else (sky, objects, dashboard) is the original, enlarged. Because of that construction the enlarged picture sampled at every S-th pixel must be the original picture, which is measured on every frame of the race.

**Tech Stack:** C11 (`port/`), Python 3 with pytest and ruff (`src/rr2port`, `tests/port`), GDScript (Godot 4.7.1), the `full-throttle` recording under `generated/port/handoffs/`.

**Spec:** `docs/superpowers/specs/2026-11-11-draw-data-contract.md` (decisions 1a, 1b and 2), with `analysis/race-picture.md` as its evidence and `docs/superpowers/specs/2026-11-10-native-port-design.md` Stage 4 behind it. **Not in this plan:** high-resolution objects (1c), interpolation (1d), re-projecting the road from the camera with fractions (contract form 2a; the 32 projected points, form 2b, are used here and 2a comes with interpolation, which needs it anyway), the sky strips, the dashboard, sound.

## Global Constraints

- Nothing derived from the ROM is committed: pictures, extracted art, dumps, logs and RAM images stay under ignored `generated/` (and `godot/bin/`). Test fixtures are synthetic.
- Every claim in `analysis/` is labelled **verified** (with the measurement) or **hypothesis**. "Verified" for part (a) means the reading predicts values that were then checked on passes it was not derived from.
- No instrument or renderer may change what the game computes. With the new options switched on, the `full-throttle` replay must still end in `RESULT match` (2,176 of 2,176).
- The reference is the port at 100% CPU speed. No claim about a real console, and none about other CPU speeds.
- Surface look (decision 2): the grass keeps its original pattern at original pixel size; only edges become crisp. No flat fill.
- The default picture stays the original 320×224. The crisp road is switched on by the user.
- Do not rebuild `godot/bin/librr2.dylib` while a Godot window may be using it; say so in the conversation before the rebuild in Task 6.
- Commit only where a task's commit step says. Work on branch `crisp-road`, created from `renderer-plan`.
- Writing for the owner is in plain terms; explain video-chip and reverse-engineering words on first use.
- Known starting facts are in `analysis/race-picture.md` and `analysis/memory-map.md` ("Race picture"). In particular: draw records `0xFFCEB8` (22 bytes), order list `0xFF08D6`, picture pointer table ROM `0x56A0A`, object drawing `0xA4EE`/`0xA5C8`, scaler `0xA08E`/`0xA40A` onward, vehicles `0x15672` (list `0xFF04F8`), roadside `0x16E8C` (list pointer `0xFFD352`), centre dashes `0x13F20`; staged sprite pictures `0xFF4742`/`0xFF7A0A`, staged shift table and sprite list `0xFF3A40`/`0xFF40C2` (sprite list at +0x400), buffer selector `0xFF279A`; road chain `0x14566` → `0x153EA` → `0x14822`; road arrays `0xFF1E20`, `0xFFD2B8`, `0xFF1E84`, `0xFF1F4C`; reciprocal table ROM `0x15472`.

## Review Focus

1. **The crisp road is shown for the wrong pass.** The road on screen is usually one pass behind the road arrays in RAM, and arrays can be caught half-written. Expected: the renderer uses road points only when the whole-line table they predict equals the shift table actually in video memory; otherwise that frame is the plain enlargement. Never a wrong road. Test in Task 5 (`test_falls_back_when_the_table_in_video_memory_differs`).
2. **Not a one-player race.** Menus, two-player split screen, end-of-race screens. Expected: plain enlargement, no override. Test in Task 5 (`test_menu_frame_is_a_plain_enlargement`).
3. **Road edges at their limits.** An edge clamped at the screen side, a line the road does not reach (a hill crest), the last line of the view. Expected: the sampled-back picture still equals the original. Test in Task 4 (`test_clamped_and_missing_lines_sample_back_to_the_original`).
4. **Objects at their limits.** A mirrored picture, a two-part object (second picture), one cut off by the "hidden below" line or the screen edge, each colour set. Expected: the checker's sample contains each case and reports them separately. Test in Task 3 (`test_mirrored_and_clipped_records`), and the measure step counts each class.
5. **The picture size changes while running.** Switching scale in the Godot window. Expected: the texture is resized, no stale or torn frame, and scale 1 is byte-for-byte the old path. Test in Task 6 (`test_scale_change_resizes_the_picture`).

---

## Part (a): the objects

### Task 1: RAM images taken at a chosen instruction

Handoff images are taken by the frame interrupt wherever the main loop happens to be, which is why 8 images and 115 tables stayed unexplained. An image taken when the main loop reaches a named instruction is always finished work.

**Files:**
- Modify: `port/tools/rr_race_replay.c` (and its header comment)
- Test: `tests/port/test_race_replay.py`
- Modify: `analysis/race-picture.md` (section 4.2)

**Interfaces:**
- Produces (tool): `--ram-at-pc PC:A:B:FILE` (PC hex). From handoff A up to handoff B, each time the instruction at PC is about to run, append one image to FILE in the layout `rr2port.handoffs.iter_handoffs` reads, numbered by the handoff count at that moment. Uses the existing `step_hook`. May be given several times. Refused with `--resync`.

- [ ] **Step 1: Write the failing test** `test_ram_at_pc_takes_an_image_each_time_the_instruction_runs`: on the synthetic fixture (use `code=` to place a known instruction that runs once per pass after writing a RAM cell), assert the file holds one image per pass in range and each image already contains that pass's cell value; and `test_ram_at_pc_is_refused_with_resync`.
- [ ] **Step 2: Run** `uv run pytest tests/port/test_race_replay.py -q`. Expected: FAIL, unknown option.
- [ ] **Step 3: Implement** the option.
- [ ] **Step 4: Run** `uv run pytest -q && uv run ruff check . && make -C port test`. Expected: all pass.
- [ ] **Step 5: Measure.** Take images over the whole race at the instruction where `0x14822` returns to its caller (find it in the translated C). Run `rr2-scene-check` on that file. Record the new SEGMENTS and SHIFT_TABLES counts in section 4.2 beside the old ones, **verified**. Expected direction: unexplained tables fall well below 115; whatever the number is, report it. Also confirm the replay with the option on still prints `RESULT match`.
- [ ] **Step 6: Commit**: "Take RAM images at a chosen instruction and re-measure the road tables".

### Task 2: Trace the object path

Research. Its product is section 3.2/3.3 of `analysis/race-picture.md` rewritten from "read from the code" to statements Task 3 can test.

**Files:**
- Modify: `analysis/race-picture.md`, `analysis/memory-map.md`

- [ ] **Step 1: Take clean images**: `--ram-at-pc` at the instruction after the main loop's call to `0xA4EE` returns (all pictures drawn, sprite list complete), whole race, to `generated/port/draw/after-draw.bin`. Also `--ram-writes` on the staged sprite list and sprite-picture buffer for passes 500–502 and 66–68.
- [ ] **Step 2: Research, in this order.** Method: the translated C in `generated/port/`, `rr2-ram-writes`, Ghidra, and small throwaway scripts in the scratchpad. Write each answer down as verified or hypothesis.
  1. **Picture data.** The header (what the three bytes really are, and any more), then how rows of pixels are stored (packed, run-length, column lists?), where transparency and the 16 colours come from, how the colour set (flags bits 12–13) and mirror are applied.
  2. **The scaler** (`0xA08E`, `0xA40A` onward): how the size at +4 picks source pixels for each output pixel (step, rounding), and how the output is written as 8×8 tiles into the staged picture buffer.
  3. **Placement and cutting.** From record x, foot line, size and header to the top-left corner; the "further sideways adjustment" seen in section 4.4; how a picture wider or taller than 32 pixels is cut into sprites; sprite order and link; what "hidden below" (+6) removes; how the second picture (+0x12) is attached.
  4. **Where records come from.** Roadside (`0x16E8C`): confirm the formula in 3.2 against the clean images. Vehicles (`0x15672`): from a vehicle object (list `0xFF04F8`) to its record: which road point, sideways position, size, picture choice.
  5. **Centre-line dashes** (`0x13F20`): which data they are drawn from and whether they are pictures like the rest or generated. Lowest priority.
  **Stop rule:** items 1–3 are the purpose of this task and are worked until Task 3 can be written. For items 4 (vehicles) and 5, after two attempts that do not produce a testable rule, write down what is known as a hypothesis and go on.
- [ ] **Step 3: Rewrite sections 3.2 and 3.3** and add the new addresses to `analysis/memory-map.md`. Nothing is called verified yet; that is Task 3.
- [ ] **Step 4: Commit**: "Trace how a draw record becomes sprites".

### Task 3: The proof for objects, and the art on disk

**Files:**
- Create: `src/rr2port/objects.py`, `src/rr2port/objects_check.py`; add `rr2-object-check` and `rr2-extract-art` to `pyproject.toml` and `src/rr2port/cli.py`
- Test: `tests/port/test_objects.py`, `tests/port/test_objects_check.py`
- Modify: `analysis/race-picture.md` (section 4.4), `docs/superpowers/specs/2026-11-11-draw-data-contract.md` (section 3)

**Interfaces:**
- Consumes: `rr2port.scene.read_scene`, `SceneObject`, `RoadTables`; `rr2port.handoffs.iter_handoffs`; the formats from Task 2.
- Produces:
  ```python
  @dataclass(frozen=True)
  class Picture:
      index: int; width: int; height: int; anchor_x: int
      rows: tuple[bytes, ...]          # colour numbers 0..15, 0 = see-through
  def read_picture(rom: bytes, index: int) -> Picture
  def scale_picture(picture: Picture, size: int) -> tuple[bytes, ...]   # size as the record holds it; 0x4000 = natural
  @dataclass(frozen=True)
  class StagedSprite:
      x: int; y: int; cells_wide: int; cells_high: int
      tile: int; palette: int; priority: bool; mirrored: bool; link: int
  def draw_objects(ram: bytes, rom: bytes) -> tuple[list[StagedSprite], bytes]
      # the sprite list and the picture-buffer bytes the game stages for this image's draw records
  def roadside_records(ram: bytes, tables: RoadTables) -> list[SceneObject]
  def vehicle_records(ram: bytes, tables: RoadTables) -> list[SceneObject]   # only if Task 2 item 4 produced a rule
  ```
  Field names may change to fit Task 2 (record the change as a ruling); the function names are fixed.
- Produces (CLI): `rr2-object-check --rom ROM --images FILE [--every N]` printing `SPRITE_LISTS identical=a of b`, `SPRITE_PICTURES identical=c of d`, `ROADSIDE_RECORDS identical=e of f`, `VEHICLE_RECORDS identical=g of h` (or `not attempted`), and `CASES mirrored=… two_part=… clipped=… colour_sets=…` (how many checked records were of each kind). `rr2-extract-art --rom ROM --video-state FILE --out DIR`: every picture in the table as a PNG at natural size, in the colours of that video state, plus one contact sheet; refuses an `--out` that is not under `generated/`.

- [ ] **Step 1: Write the failing tests** on synthetic ROM and RAM built by helpers in the test file (a 4×3 picture in the format Task 2 found): `test_read_picture_decodes_rows_and_header`, `test_scale_picture_at_natural_half_and_double_size`, `test_staged_sprites_for_one_record` (position, cells, tile numbering from the buffer's first tile), `test_mirrored_and_clipped_records`, `test_wide_picture_is_cut_into_sprites`, `test_check_reports_counts_and_cases`, `test_extract_art_refuses_a_directory_outside_generated`.
- [ ] **Step 2: Run** `uv run pytest tests/port -q`. Expected: FAIL, module missing.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `uv run pytest -q && uv run ruff check .`. Expected: pass.
- [ ] **Step 5: Measure** with `rr2-object-check` on `after-draw.bin`, every pass of the race.
  **Target:** sprite lists and sprite pictures identical on every pass; roadside records identical.
  **Stop rule:** when the remaining differences are confined to one named element and two attempts have not closed it, stop and report the counts with the unexplained part as a hypothesis. An honest partial number is acceptable; an unmeasured "it matches" is not.
- [ ] **Step 6: Extract the art** to `generated/port/art/` with a race frame's colours and look at the contact sheet (a wrong format shows at a glance). Tell the owner where the sheet is.
- [ ] **Step 7: Write up.** Replace section 4.4 of `analysis/race-picture.md` with the result; update the Summary's lists; in the contract, change section 3's heading and the "Not yet settled" paragraph to say what is now measured and what is not.
- [ ] **Step 8: Commit** code, tests and documents: "Predict the staged sprites from the draw records and extract the object art".

## Part (b): the crisp road

### Task 4: A line renderer that draws sub-lines with fractional shifts

**Files:**
- Modify: `port/hw/rr_vdp.h`, `port/hw/rr_vdp_render.c`
- Test: `port/tests/test_vdp.c`

**Interfaces:**
- Produces:
  ```c
  enum { RR_SCALE_MAX = 8 };
  /* Draw display line `line` as `scale` rows of 320*scale pixels (0x00RRGGBB) into out.
   * shift_a / shift_b: NULL, or `scale` values, one per row, giving that plane's sideways
   * shift for the row in 16.16 fixed point (the same sense as the hscroll table value;
   * the integer part replaces the table's value). With both NULL every row is the
   * original line with each pixel repeated. */
  void rr_vdp_render_line_scaled(const RrVdp *v, int line, int scale,
                                 const int32_t *shift_a, const int32_t *shift_b, uint32_t *out);
  ```
- The sampling rule (it is what makes the sampled-back picture exact): for output column X on a row with shift h, the plane is read at the column the original would read for screen x = floor((X − floor(h × scale ÷ 65536)) ÷ scale) with shift 0, using floor division. Vertically every row of a line reads the same sheet row as the original line. Window, sprites and backdrop are not shifted. Layer order and priorities are the existing renderer's; restructure it so the original and the scaled renderer share one compositing path.

- [ ] **Step 1: Write the failing tests**:
  - `test_scale_1_without_shifts_is_the_original_line`: on the synthetic frame of `test_source_colour_equals_the_picture`, all 224 lines equal `rr_vdp_render_line`.
  - `test_scaled_without_shifts_repeats_each_pixel`: scale 3.
  - `test_integer_shifts_sample_back_to_the_original`: scale 4, shifts equal to the line's own hscroll values (as 16.16); every 4th pixel of row 0 equals the original line, for all lines.
  - `test_fractional_shift_moves_an_edge_one_output_pixel_per_row`: plane A has a vertical colour boundary; scale 4; shifts h, h+¼, h+½, h+¾; the boundary's output column increases by exactly 1 per row, and plane B, a sprite and the window on the same line do not move.
  - `test_clamped_and_missing_lines_sample_back_to_the_original`: shift_a given and shift_b NULL on the same line; a shift at the 10-bit wrap.
- [ ] **Step 2: Run** `make -C port test`. Expected: build fails, `rr_vdp_render_line_scaled` undeclared.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `make -C port test`. Expected: `all vdp tests passed`, other suites pass.
- [ ] **Step 5: Commit**: "Render a line as sub-lines with fractional plane shifts".

### Task 5: Road shifts from the game's road points, checked against video memory

**Files:**
- Create: `port/render/rr_road.h`, `port/render/rr_road.c`, `port/tests/test_road.c`
- Modify: `port/Makefile` (new directory in the library and test builds), `port/hw/rr_genesis.h`, `port/hw/rr_genesis.c`, `port/host/rr_console.h`, `port/host/rr_console.c`, `port/tools/rr_race_replay.c`
- Test: `port/tests/test_road.c`, `port/tests/test_console.c`, `tests/port/test_race_replay.py`
- Modify: `analysis/race-picture.md` (new section 5)

**Interfaces:**
- Consumes: `rr_vdp_render_line_scaled`; the arithmetic of `src/rr2port/scene.py` (`read_scene`'s road fields, `_interpolate`, `line_shifts`, `predict_line_tables`), which is the reference to port.
- Produces (C):
  ```c
  enum { RR_ROAD_POINTS = 32 };
  typedef struct {
      int count;
      int16_t line[RR_ROAD_POINTS], clipped_line[RR_ROAD_POINTS];
      int16_t left[RR_ROAD_POINTS], right[RR_ROAD_POINTS];
      int16_t centre_x, view_bottom, horizon_line;
  } RrRoad;
  int rr_road_read(const uint8_t *ram, RrRoad *out);   /* 0 if the RAM is not a one-player race */
  typedef struct {
      int scale;
      uint8_t have[RR_SCREEN_H];                        /* 1 where the road reaches this line */
      int32_t a[RR_SCREEN_H * RR_SCALE_MAX], b[RR_SCREEN_H * RR_SCALE_MAX];  /* row r of line l at [l*scale + r] */
  } RrRoadShifts;
  void rr_road_shifts(const RrRoad *road, const uint8_t *rom, int scale, RrRoadShifts *out);
  /* 1 if, on every line with have[], row 0's whole part equals the shift table in video memory. */
  int rr_road_matches_video(const RrRoadShifts *s, const RrVdp *v);
  ```
  Row 0 of each line is the game's own value for that line (so `rr_road_matches_video` is a check of the reading itself); rows 1..scale−1 continue along the same straight run between the two road points, in 16.16, with the game's clamps applied to the fractional value.
- Produces (`RrGenesis`): `uint32_t *hires; int hires_scale; const RrRoadShifts *hires_shifts;` and `void (*hires_begin)(RrGenesis *g, void *ctx); void *hires_ctx;` called just before display line 0 is drawn. When `hires` is set, each display line is also drawn with `rr_vdp_render_line_scaled`, passing shifts on lines with `have[]` when `hires_shifts` is not NULL.
- Produces (`rr_console.h`):
  ```c
  void rr_console_set_scale(RrConsole *c, int scale);        /* 1..RR_SCALE_MAX, clamped; 1 = off */
  int rr_console_scale(const RrConsole *c);
  void rr_console_set_crisp_road(RrConsole *c, int on);
  const uint32_t *rr_console_scaled_frame(const RrConsole *c);  /* 320*scale x 224*scale; the plain frame at scale 1 */
  uint64_t rr_console_crisp_frames(const RrConsole *c);      /* frames drawn with road shifts since reset */
  ```
  The console takes a snapshot of the road arrays each time the main loop reaches `0x14566` (step hook, installed only while crisp road is on), keeps the last four, and in `hires_begin` uses the newest one whose shifts match video memory; if none does, no shifts for that frame.
- Produces (tool): `--scaled-check S` runs the whole replay with scale S and crisp road on and prints `SCALED scale=S frames=N sampled_back_identical=M crisp_frames=K race_frames=R ms_per_frame=T` (sampled back: every S-th pixel of every S-th row against the original frame; race frames: frames in which the race callback is installed and it is a one-player race). `--scaled-at F:FILE` writes that frame's enlarged picture as a PPM.

- [ ] **Step 1: Write the failing C tests** in `test_road.c` (synthetic RAM, a small reciprocal table in a synthetic ROM image): `test_row_0_equals_the_integer_table` (three road points on a slope; compare with values worked by hand from `scene.py`'s rule), `test_rows_continue_the_slope` (scale 4: rows of a line step by a quarter of the line-to-line step), `test_clamped_edge_stays_clamped_on_every_row`, `test_line_beyond_the_last_point_has_no_shift`, `test_not_a_race_is_refused` (callback slot not `0xE038`; two-player word set). In `test_console.c`: `test_falls_back_when_the_table_in_video_memory_differs` (road arrays present, video memory's table changed on one line: `crisp_frames` does not advance and the scaled frame is the plain enlargement), `test_menu_frame_is_a_plain_enlargement`, `test_scale_1_is_the_plain_frame`.
- [ ] **Step 2: Run** `make -C port test`. Expected: build fails, `rr_road.h` missing.
- [ ] **Step 3: Implement** `rr_road.c`, the genesis fields, the console functions.
- [ ] **Step 4: Run** `make -C port test`. Expected: all suites pass, including the new `all road tests passed`.
- [ ] **Step 5: Write the failing replay tests**: `test_scaled_check_reports_counts` and `test_scaled_check_does_not_change_the_race` (same report lines and exit status with and without `--scaled-check 4`) on the synthetic fixture.
- [ ] **Step 6: Run** `uv run pytest tests/port/test_race_replay.py -q`. Expected: FAIL, unknown option. **Implement** the two options. **Run** `uv run pytest -q && uv run ruff check . && make -C port test`. Expected: pass.
- [ ] **Step 7: Measure** on `full-throttle` with `--scaled-check 4`. Record in a new section 5 of `analysis/race-picture.md`, **verified**: `RESULT match`; frames sampled back identical (target: all); crisp frames out of race frames (the rest fell back; say how many and, from a sample of them, why); milliseconds per frame. Write three `--scaled-at` pictures (frames 1518, 4000, 10261) and look at them beside the originals: road edges and edge lines are straight slopes, the grass pattern is unchanged in size, objects and dashboard are unchanged.
  **If fewer than 90% of race frames are crisp:** the snapshot point is wrong. Use `--ram-writes` on the road arrays to find where they are last written before `0x14822` reads them, move the snapshot, and measure again; record the ruling.
- [ ] **Step 8: Commit**: "Draw the road with fractional edges from the game's road points".

### Task 6: The crisp road in the Godot window

**Files:**
- Modify: `port/host/rr_godot.c`, `godot/scripts/console.gd`, `godot/README.md`
- Test: `godot/tests/test_console.gd`, `godot/tests/test_console_scene.gd`, `tests/port/test_godot_extension.py`

**Interfaces:**
- Consumes: the `rr_console.h` functions of Task 5.
- Produces (`RoadRashConsole`): `set_scale(int)`, `get_scale() -> int`, `set_crisp_road(bool)`, `get_frame_width() -> int`, `get_frame_height() -> int`; `get_frame()` returns the scaled frame (RGBA8, width × height × 4 bytes).
- Produces (`console.gd`): key **R** switches between the original picture (scale 1) and the crisp road (scale 4, crisp road on), showing "Crisp road on (4×)" or "Original picture" for `speed_message_seconds`; `set_crisp(on: bool)` for tests. The picture and texture are re-created when the size reported by the console changes. Starts as the original picture.

- [ ] **Step 1: Write the failing tests.** `test_console_scene.gd`: `test_scale_change_resizes_the_picture` (a stand-in console whose `get_frame_width/height` change after `set_scale`; after one physics tick `picture().get_size()` is 1280×896, and back to 320×224 after switching off) and `test_r_key_toggles_the_crisp_road`. `test_console.gd`: with the real class, `set_scale(4)` then a frame gives 1280×896×4 bytes. `test_godot_extension.py`: the new methods are registered.
- [ ] **Step 2: Run** the three Godot headless tests and `uv run pytest tests/port/test_godot_extension.py -q`. Expected: FAIL (methods missing). The real-class test needs the library: it fails until Step 4.
- [ ] **Step 3: Implement** `rr_godot.c` and `console.gd`.
- [ ] **Step 4: Say in the conversation that the Godot library is about to be rebuilt** (any open game window should be closed), then run `scripts/build-godot-extension.sh`.
- [ ] **Step 5: Run** the three Godot headless tests, `uv run pytest -q && uv run ruff check . && make -C port test`. Expected: all pass.
- [ ] **Step 6: Update `godot/README.md`** (the R key, what it changes and what it does not).
- [ ] **Step 7: Commit**: "Show the crisp road in the Godot window".

### Task 7: Findings

**Files:**
- Modify: `analysis/race-picture.md` (Summary), `docs/superpowers/specs/2026-11-11-draw-data-contract.md` (a short "State after the first renderer step" section), `README.md`

- [ ] **Step 1: Update the Summary** of `analysis/race-picture.md`: what moved from "read" or "not confirmed" to verified, what is still open, how to repeat each new measurement.
- [ ] **Step 2: In the contract**, record what the first renderer step uses (form 2b road points, straight runs between points as the original draws them) and what is left for the next plans: curve smoothing between points as a look option, form 2a with interpolation, high-resolution objects from the extracted art, the dashes.
- [ ] **Step 3: Run** `uv run pytest -q && uv run ruff check . && make -C port test`, the three Godot headless tests, and the full `full-throttle` replay with `--scaled-check 4`. Expected: all pass; `RESULT match`.
- [ ] **Step 4: Commit**: "Write up the object trace and the crisp road".
