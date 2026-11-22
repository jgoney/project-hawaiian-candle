# Draw Path and Scene Description Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Find out how the race picture is made, from the game's own description of the scene (road, horizon, riders, cars, signs, with their distances) down to the video chip, prove the understanding by predicting the picture from that description, and write the draw-data contract that the high-resolution renderer and interpolation will be built on.

**Architecture:** Three measuring instruments are added to the existing replay tool, all off by default and none allowed to change what the game computes: a per-pixel "which layer drew this" map, a log of every video-chip write with the code address that made it, and a log of work-RAM writes to a chosen range. They are used to walk backwards from the picture to the RAM tables the game stages for the video chip, and from those to the scene data and the projection arithmetic. A Python scene reader then takes a work-RAM image alone and predicts what the game sends to the video chip; the prediction is compared with what the game actually sent, and finally as a picture.

**Tech Stack:** C11 (`port/`), Python 3 with pytest and ruff (`src/rr2port`, `tests/port`), the `full-throttle` recording under `generated/port/handoffs/`, MAME 0.289 only if a cross-check is needed.

**Spec:** `docs/superpowers/specs/2026-11-10-native-port-design.md`, Stage 4, first two bullets (reverse engineer what the game decides to draw; write the draw-data contract for the owner's approval before renderer work begins). The renderer itself, art extraction from the ROM, upscaling and interpolation are **not** in this plan.

## Global Constraints

- Nothing derived from the ROM is committed: pictures, video-memory dumps, logs, RAM images and scene dumps stay under ignored `generated/`. Test fixtures are synthetic.
- Every claim in `analysis/` is labelled **verified** (with the measurement) or **hypothesis**.
- The instruments must not change behaviour. With every one of them switched on, the `full-throttle` replay must still end in `RESULT match` with nothing excluded (2,176 of 2,176).
- The reference is the port at 100% CPU speed, which is measured equal to MAME 0.289 for this race (`analysis/race-conformance.md`). No claim is made about a real console.
- Do not rebuild `godot/bin/librr2.dylib` while a Godot window may be using it; say so before any rebuild. This plan should not need one.
- Commit only where a task's commit step says. Work on branch `draw-path`.
- Writing for the owner is in plain terms: explain video-chip and reverse-engineering words on first use.
- Known starting facts (from `analysis/`): race loop handoff `0xDF48`; frame interrupt handler `0x11B8` with callback slot `0xFF0300` (race callback `0xE038`); line interrupt handler `0x14BE`, which reads per-line command data through pointers near `0xFF2796`/`0xFF27DA`; the pass routine `0xEEB8` waits on the word through `0xFF27B2`; player vehicle record at `0xFF0A62`; mode word `0xFF0304`. In a profile of race passes, code at `0xA000`–`0xABFF` takes about 36% of CPU time and the line interrupt handler about 10%; what `0xA000`–`0xABFF` does is unknown. `0x16CE`/`0x179E` moving the road every frame is a hypothesis.

## Review Focus

1. **An instrument changes the game.** Expected: `RESULT match` with all logs on. Test in Task 2 (`test_logs_do_not_change_the_race`).
2. **The picture changes part-way down the screen.** The line interrupt handler rewrites video registers during the frame, so a source map or state taken once per frame is wrong for some lines. Expected: the source map and any per-line state are captured line by line as the picture is drawn. Test in Task 1 (`test_source_map_follows_a_mid_frame_change`).
3. **Sprite corner cases in the source map**: a sprite partly off the left or top edge, two overlapping sprites, a sprite whose colour is transparent over a plane. Expected: the map names the layer whose colour is actually shown. Test in Task 1 (`test_source_colour_equals_the_picture`, run over a frame containing all three).
4. **A pass's scene is not shown in the frame it was computed in.** Passes take 3 to 9 frames and the picture is handed over through the frame interrupt. Expected: the scene reader states which frames show which pass, and the comparison in Task 4 uses that pairing, measured, not assumed. Test in Task 4 (`test_pairing_of_pass_and_frame_is_measured`).
5. **Not a one-player race.** Menus, two-player split screen and the end-of-race screens use the same memory differently. Expected: the scene reader refuses (raises `NotARaceScene`) rather than returning nonsense. Test in Task 4 (`test_scene_reader_refuses_a_menu_image`).

---

### Task 1: The race picture, and which layer drew each pixel

The video chip builds the picture from layers: a backdrop colour, two scrolling tile planes (A and B), a fixed "window" plane, and up to 80 sprites. Knowing which layer each pixel came from tells us at once how the road, hills, riders and dashboard are drawn.

**Files:**
- Modify: `port/hw/rr_vdp.h`, `port/hw/rr_vdp_render.c`, `port/hw/rr_genesis.h`, `port/hw/rr_genesis.c`, `port/tools/rr_race_replay.c`
- Create: `src/rr2port/picture_report.py`; add `rr2-picture-report` to `pyproject.toml` and `src/rr2port/cli.py`
- Test: `port/tests/test_vdp.c`, `tests/port/test_picture_report.py`, `tests/port/test_race_replay.py`

**Interfaces:**
- Produces (C):
  ```c
  enum { RR_SRC_BACKDROP, RR_SRC_PLANE_B, RR_SRC_PLANE_A, RR_SRC_WINDOW, RR_SRC_SPRITE };
  typedef struct {
      uint8_t layer;      /* RR_SRC_* */
      uint8_t colour;     /* 0..63, index into CRAM */
      uint16_t detail;    /* sprite: index in the sprite table; plane/window: the name-table entry */
  } RrPixelSource;
  void rr_vdp_render_line_sources(const RrVdp *v, int line, RrPixelSource *out /* 320 */);
  ```
  `RrGenesis` gains `RrPixelSource *sources;` (NULL = off; when set, 320×224 entries filled line by line at the same moment as `frame`) and `RrVdpLineState *line_state;` (NULL = off; 224 entries, each the 24 registers plus the two horizontal scroll values and both planes' first vertical scroll value in force when that line was drawn).
- Produces (tool): `--picture-at F:FILE` (PPM of frame F), `--sources-at F:FILE` (224×320 records of 4 bytes: layer, colour, detail big-endian), `--line-state-at F:FILE` (224 text lines), `--video-state-at F:FILE` (registers, VRAM, CRAM, VSRAM as at the end of frame F, in the layout `rr-vdp-render` already reads). Each may be given several times. `F` counts frames from power-on as the existing report does.
- Produces (Python): `read_sources(path) -> list[list[PixelSource]]`; `layer_bands(sources) -> list[Band]` (runs of lines with the same set of layers); `sprites_on_screen(video_state) -> list[Sprite]` (index, x, y, width, height in cells, tile, palette, priority, flips, link order); CLI `rr2-picture-report --sources F --line-state F --video-state F` printing bands, sprites, and every register that differs between lines.

- [ ] **Step 1: Write the failing C tests** in `test_vdp.c`:
  - `test_source_colour_equals_the_picture`: build a synthetic frame with plane B filled, plane A partly transparent, a window strip, a sprite half off the left edge, two overlapping sprites, and a sprite with a transparent pixel; for all 320×224 pixels assert `rr_vdp_colour(cram[src.colour]) == pixel` from `rr_vdp_render_line`, and assert the expected `layer` at six named points (one per case).
  - `test_source_map_follows_a_mid_frame_change`: run a frame through `rr_genesis_run_frame` with `sources` and `line_state` set, on a tiny synthetic program whose line interrupt changes the backdrop register at line 100; assert `sources` colour above and below differ and `line_state[99].reg[7] != line_state[100].reg[7]`.
- [ ] **Step 2: Run** `make -C port test`. Expected: build fails, `rr_vdp_render_line_sources` undeclared.
- [ ] **Step 3: Implement** `rr_vdp_render_line_sources` by making the existing line renderer compute sources and derive colours from them (one code path, so the two cannot disagree); fill `sources` and `line_state` in `rr_genesis.c` where line 413 renders.
- [ ] **Step 4: Run** `make -C port test`. Expected: `all vdp tests passed`, the other two suites pass.
- [ ] **Step 5: Write the failing Python tests**: `test_layer_bands_groups_lines`, `test_sprites_on_screen_follows_the_link_order`, `test_sprite_table_base_comes_from_register_5` on synthetic data; in `test_race_replay.py`, `test_picture_and_sources_are_written_for_the_asked_frame` on the existing synthetic fixture.
- [ ] **Step 6: Run** `uv run pytest tests/port -q`. Expected: the new tests fail (module missing, option unknown).
- [ ] **Step 7: Implement** the tool options and `picture_report.py`.
- [ ] **Step 8: Run** `uv run pytest -q && uv run ruff check . && make -C port test`. Expected: all pass.
- [ ] **Step 9: Measure.** Pick 12 frames of `full-throttle` (the frames in which passes 3, 20, 70, 300, 500, 510, 900, 1300, 1700, 2000, 2100 and 2170 start, taken from the report), write all four files for each under `generated/port/draw/`, run `rr2-picture-report`, look at the PPMs. Write section 1 of `analysis/race-picture.md`: which layer draws the sky, hills, road surface, lane markings, verges, roadside objects, riders, the dashboard and the mirrors; which registers change during a frame, at which lines, to what; how many sprites are in use. All **verified** for those frames.
- [ ] **Step 10: Commit** `port/`, `src/`, `tests/`, `pyproject.toml`, `analysis/race-picture.md`: "Add a per-pixel layer map and describe how the race picture is layered".

### Task 2: Who writes the video chip

**Files:**
- Modify: `port/hw/rr_genesis.h`, `port/hw/rr_genesis.c`, `port/tools/rr_race_replay.c`
- Create: `src/rr2port/video_report.py`; add `rr2-video-report`
- Test: `port/tests/test_vdp.c`, `tests/port/test_video_report.py`, `tests/port/test_race_replay.py`

**Interfaces:**
- Produces (C):
  ```c
  enum { RR_VIDEO_REGISTER, RR_VIDEO_VRAM, RR_VIDEO_CRAM, RR_VIDEO_VSRAM, RR_VIDEO_DMA, RR_VIDEO_FILL };
  typedef struct {
      uint64_t frame; int line;      /* where the beam was */
      uint8_t kind;                  /* RR_VIDEO_* */
      uint32_t target;               /* register number, or address in that memory */
      uint32_t value;                /* value written; for DMA and fill, the length in words */
      uint32_t source;               /* DMA only: 68000 address the data came from */
  } RrVideoEvent;
  /* In RrGenesis: */ void (*video_log)(void *ctx, const RrVideoEvent *e); void *video_log_ctx;
  ```
- Produces (tool): `--video-log A:B:FILE`: from handoff A up to handoff B, one text line per event: `handoff frame line pc kind target value source`, where `pc` is the address of the instruction making the access (the tool already tracks it for `--profile`).
- Produces (Python): `read_video_log(path) -> list[VideoEvent]`; `summarise(events, registers) -> list[Row]` grouping by (kind, named target region, code address rounded down to the enclosing routine start if the code report knows it, else to 0x100), where target regions are named from the registers: plane A table, plane B table, window table, sprite table, horizontal scroll table, "tile patterns" otherwise. CLI `rr2-video-report LOG --video-state FILE`.

- [ ] **Step 1: Write the failing tests.** C: `test_video_log_reports_register_data_and_dma_writes` (three writes through the bus produce three events with the right kind, target, value, and for the DMA the RAM source and length). Python: `test_summary_names_the_sprite_and_scroll_tables`, `test_summary_groups_by_writer`. Replay: `test_logs_do_not_change_the_race` (synthetic fixture run with `--video-log`, `--sources-at`, `--line-state-at` gives the same report lines and exit code as without).
- [ ] **Step 2: Run** `make -C port test; uv run pytest tests/port -q`. Expected: fail (field and option missing).
- [ ] **Step 3: Implement** the hook at the two write sites in `rr_genesis.c` (lines 215–217) and where DMA and fill are carried out, then the option and the report.
- [ ] **Step 4: Run** all suites and ruff. Expected: pass.
- [ ] **Step 5: Run the real check of Review Focus 1:** the full `full-throttle` replay with `--video-log 500:520:…`, one `--sources-at` and one `--line-state-at`. Expected: `RESULT match`, TIMESTEP same=2176/2176.
- [ ] **Step 6: Measure.** Log passes 500–520 and 66–80. Add section 2 to `analysis/race-picture.md`: a table of every writer (code address) with what it writes and when (in the frame interrupt, in the line interrupt, in the main loop); the work-RAM addresses that DMA copies from, with their sizes — these are the staging buffers (sprite list, scroll table, road table, tile data); what the line interrupt handler writes on each of its lines.
- [ ] **Step 7: Commit**: "Log video-chip writes by the code that makes them".

### Task 3: From the staging buffers back to the scene

This is the research task. Its instrument is small; its product is understanding, written down with evidence.

**Files:**
- Modify: `port/core/rr_machine.h` and the work-RAM write path it declares, `port/tools/rr_race_replay.c`
- Create: `src/rr2port/ram_writes.py`; add `rr2-ram-writes`
- Modify: `analysis/race-picture.md`, `analysis/memory-map.md`
- Test: `port/tests/test_core.c`, `tests/port/test_ram_writes.py`

**Interfaces:**
- Consumes: staging buffer addresses from Task 2.
- Produces (tool): `--ram-writes A:B:START:END:FILE`: from handoff A to B, every work-RAM write that touches `START..END` (hex), as `handoff frame pc address size value`. Off by default; when off the write path costs one predictable branch at most.
- Produces (Python): `writers(path) -> dict[pc, Range]` and CLI `rr2-ram-writes FILE` listing each writing instruction with the addresses it wrote and how often.

- [ ] **Step 1: Write the failing tests**: C `test_ram_write_watch_reports_only_the_range` (byte, word and long writes, one straddling the range start); Python `test_writers_groups_by_instruction`.
- [ ] **Step 2: Run** them. Expected: fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** all suites, ruff, and the full `full-throttle` replay with a watch on. Expected: pass; `RESULT match`.
- [ ] **Step 5: Research, in this order, each answer written into `analysis/race-picture.md` section 3 as verified or hypothesis:**
  1. **Sprites.** Which routine fills the sprite staging buffer; where it reads each object from (the object records: address, size, count); which fields are world position along the road, sideways offset, and height; how distance from the player becomes screen x, screen y and a choice of picture size (the projection); how the picture (tile number, size, flip, palette) is chosen from object type, animation frame and distance; how draw order is decided.
  2. **Road.** What `0xA000`–`0xABFF` computes. Which routine fills the road and scroll staging tables; what one table entry means on screen (per line: sideways shift, width, colour or stripe phase); how curves and hills enter (the track data it reads and where the track position is kept); where the horizon line comes from; how the background hills scroll.
  3. **Frame-rate behaviour.** Whether `0x16CE`/`0x179E` (or anything else in the frame callback `0xE038`) moves the picture between passes, and what it moves. This decides what interpolation has to replace.
  4. **Hand-over.** The exact sequence by which a finished pass's tables reach the screen (the word through `0xFF27B2`, the frame interrupt, the line interrupt), and which frames show which pass. From that, state why a faster CPU can show a half-updated picture (the owner's screenshots in `analysis/smoothness-experiment.md`), as verified if reproduced with `--cpu-speed 800 --picture-at`, otherwise hypothesis.
  5. **Dashboard, mirrors, text.** Enough to say which layer and which tables, and that they are separable from the 3D view. No deeper.
  Evidence standard for "verified": the statement predicts values that were then checked against logged data on at least two passes it was not derived from. Method: `rr2-ram-writes` to find writers, the translated C in `generated/port/` and Ghidra annotations to read them, `--track` and `--ram-at` to test predictions.
- [ ] **Step 6: Update `analysis/memory-map.md`** with the addresses now understood.
- [ ] **Step 7: Commit**: "Trace the race picture from the video chip back to the scene data".

### Task 4: The proof: predict the picture from the scene

**Files:**
- Create: `src/rr2port/scene.py`, `src/rr2port/scene_check.py`; add `rr2-scene-check`
- Test: `tests/port/test_scene.py`, `tests/port/test_scene_check.py`

**Interfaces:**
- Consumes: the layouts and formulas from Task 3; RAM images from `--ram-at` or `generated/port/handoffs/full-throttle.bin`; video state, line state and pictures from Task 1's options.
- Produces:
  ```python
  class NotARaceScene(Exception): ...
  @dataclass(frozen=True)
  class SceneObject:   # one rider, car, sign, tree...
      kind: int; along: int; across: int; height: int      # world position, game units
      distance: int                                        # from the camera, game units
      screen_x: int; screen_y: int; art: int; width: int; height_px: int
      flip: bool; palette: int; order: int
  @dataclass(frozen=True)
  class RoadLine:      # one screen line of road
      line: int; distance: int; centre_x: int; half_width: int; stripe: int
  @dataclass(frozen=True)
  class Scene:
      horizon_line: int; background_scroll: tuple[int, int]
      road: tuple[RoadLine, ...]; objects: tuple[SceneObject, ...]
  def read_scene(ram: bytes) -> Scene
  def predict_sprite_table(scene: Scene) -> list[tuple[int, int, int, int]]   # y, size+link, tile+flags, x
  def predict_line_tables(scene: Scene) -> list[tuple[int, ...]]              # per line, what the game stages
  ```
  Field names and units may change to fit what Task 3 finds (record the change as a ruling); the three functions and the exception are fixed. `art` may be the game's own tile number: decoding the ROM's compressed art is not in this plan.
- Produces (CLI): `rr2-scene-check --ram FILE --video-state FILE --line-state FILE [--picture FILE]` printing `SPRITES identical=a/b`, `LINES identical=c/d`, and with `--picture`, `PIXELS identical=e/f` for the 3D view (the lines above the dashboard), where the picture is redrawn from the predicted tables plus the tile art in the video state.

- [ ] **Step 1: Write the failing tests** on synthetic RAM built by a helper in the test file: `test_read_scene_finds_objects_and_road`, `test_scene_reader_refuses_a_menu_image` (mode word not a one-player race → `NotARaceScene`), `test_predicted_sprite_entry_for_a_known_object`, `test_predicted_line_table_for_a_straight_flat_road`, and in `test_scene_check.py` `test_pairing_of_pass_and_frame_is_measured` (the checker is given frames either side of the right one and reports the best pairing, failing if none matches) and `test_check_reports_counts`.
- [ ] **Step 2: Run** `uv run pytest tests/port -q`. Expected: fail (module missing).
- [ ] **Step 3: Implement** `scene.py` and `scene_check.py`.
- [ ] **Step 4: Run** all suites and ruff. Expected: pass.
- [ ] **Step 5: Measure** on 60 passes of `full-throttle`: every 40th pass from 40 to 2160, plus passes 2–7 (the start line, flag waver). Record in `analysis/race-picture.md` section 4 the three counts per pass and the totals.
  **Target:** sprites, line tables and 3D-view pixels identical on all 60.
  **Stop rule:** when the remaining differences are confined to one named element (for example "roadside objects beyond the horizon" or "the crash animation") and two attempts at it have not closed it, stop, report the counts and what is unexplained as a hypothesis, and go on to Task 5. An honest "97%, and here is what is missing" is an acceptable outcome of this plan; an unmeasured "it matches" is not.
- [ ] **Step 6: Commit**: "Read the scene from work RAM and check it against the picture".

### Task 5: The draw-data contract and the findings

**Files:**
- Create: `docs/superpowers/specs/2026-11-11-draw-data-contract.md`
- Modify: `analysis/race-picture.md` (summary at the top), `README.md` (one line pointing at both), `analysis/smoothness-experiment.md` (replace the artifact hypothesis with what Task 3 found)

- [ ] **Step 1: Write the contract**, for the owner to approve before any renderer work. In plain terms, with a labelled diagram of one frame: what the core will hand the renderer each pass (road geometry and perspective, horizon and camera, each object's position, distance, picture, size, order and palette, the dashboard and mirrors as their own layer), in what units, what is measured and what is still hypothesis, and what moves between passes in the original. Then three short sections: **what this makes possible for crisp edges** (which edges become real geometry, what stays pixel art, and what distance information is available per object and per road line), **what interpolation needs** (which values to blend between passes, and which cannot be blended, such as animation frames), **open questions for the owner**.
- [ ] **Step 2: Write the summary** at the top of `analysis/race-picture.md`: result, what is and is not shown (one race, one track, no crash or police sequences unless they occurred), how to repeat every measurement.
- [ ] **Step 3: Run** `uv run pytest -q && uv run ruff check . && make -C port test` and the full `full-throttle` replay. Expected: all pass; `RESULT match`.
- [ ] **Step 4: Commit**: "Write the draw-data contract and the race picture findings".
