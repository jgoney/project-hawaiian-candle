# High-Resolution Objects (Stage 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** In the enlarged picture, draw every rider, car, tree and sign straight from its picture in the ROM at the output resolution, instead of enlarging the game's already-scaled 320×224 sprite.

**Architecture:** The game's object drawing (`0xA4EE`) is repeated in C (a port of the verified Python model in `src/rr2port/objects.py`), which gives, for each sprite the game stages, the picture it shows and how that picture is placed, scaled and cut. The video chip model keeps drawing the sprite list exactly as before (order, priority, per-line limits), but a sprite that has such a description takes each output pixel from the ROM picture instead of from its tiles. A description is only used while the sprite entries and tiles in video memory are byte for byte what the C model predicted; otherwise the sprite is the plain enlargement.

**Tech Stack:** C11 (`port/`), Python 3 + pytest (`tests/port/`), GDScript (Godot 4.7, `godot/`).

**Spec:** `docs/superpowers/specs/2026-11-11-draw-data-contract.md` (section 3 "Objects", "Objects stay pixel art, but placed and scaled finely", decision 1(c)). Evidence: `analysis/race-picture.md` sections 2, 3.3, 4.4.

## Global Constraints

- Nothing derived from the ROM is committed: the ROM, generated C, RAM images, pictures, logs stay under the ignored `generated/` and `godot/bin/`. Test fixtures are synthetic.
- Nothing is called "the same" without a measurement. Statements in `analysis/` are labelled **verified** or **hypothesis**.
- The game logic is untouched: a replay with every new option on must still print `RESULT match`.
- With fine objects off, every picture is what it was before this plan (existing tests stay as they are and pass).
- Do not rebuild `godot/bin/librr2.dylib` while a Godot window may be using it; say so before the final rebuild.
- Commit only as the tasks say. Integration is the owner's decision.
- Out of scope, by decision: placing objects between pixels (their positions stay the game's whole pixels; that comes with interpolation, which makes positions fractional anyway), upscaled replacement art (stage 2), the centre-line dashes, two-player races.

## Review Focus

1. **A RAM image the model does not cover** (title screens never reach `0xA4EE`, but a two-player race does, and the buffer pointers at `0xFF27A6`/`0xFF27CE` may hold anything): the model must never read or write out of bounds, and the picture must be the plain enlargement. Test: Task 1 `test_wild_pointers_and_counts_do_not_escape`.
2. **A picture address outside the ROM, or a picture whose rows run past its end:** no crash; that sprite is drawn plain. Test: Task 1 `test_picture_outside_rom_is_plain`.
3. **The sprite list or tile buffer running out** (80 sprites, 0x150 tiles): the object the game drops must also lose its descriptions. Test: Task 1 `test_dropped_record_leaves_no_picture`.
4. **CPU speed other than 100%** (the owner plays at 200%): the set on screen is found by content, not by timing, and video memory changing part-way down a frame turns fine sprites off for the rest of it. Tests: Task 2 `test_genesis_drops_fine_sprites_when_video_memory_changes`, Task 3 `test_tracker_picks_the_set_in_video_memory`.
5. **Reset, scale change and toggling during a race:** F5 forgets noted sets; a new scale needs nothing recomputed; switching off returns the plain enlargement on the next frame. Tests: Task 4 console tests.

---

### Task 1: The game's object drawing in C

**Files:**
- Create: `port/render/rr_objects.h`, `port/render/rr_objects.c`
- Create: `port/tests/test_objects.c`, `port/tools/rr_objects_draw_tool.c`, `tests/port/test_objects_c.py`
- Modify: `port/hw/rr_vdp.h` (the two types below only), `port/Makefile` (`RENDER_SRC`/`RENDER_HDR`, `test_objects` in `test`, target `objects-draw`), `port/tools/rr_race_replay.c` (`--objects-check`), `tests/port/test_race_replay.py`

**Interfaces:**
- Consumes: the Python reference `rr2port.objects._Drawer` (every routine has its 68000 address in a comment); addresses in its module constants.
- Produces, in `port/hw/rr_vdp.h`:

```c
/* A picture some sprites show, finer than their tiles. */
typedef struct {
    const uint8_t *rows;    /* `height` rows of `stride` bytes; two pixels to a byte, high four bits first; colour 0 is see-through */
    uint16_t width, height, stride; /* in picture pixels and bytes */
    uint32_t size;          /* enlargement on screen; 0x10000 is the picture's natural size */
    int16_t x, y;           /* screen pixel of the picture's first shown column (its right-hand end when mirrored: one past it) and first shown row */
    uint8_t mirrored;
    uint16_t first_column;  /* screen pixels of the enlarged picture cut off before x */
    uint16_t first_row;     /* picture rows skipped above y */
    uint16_t columns, rows_shown; /* screen pixels shown from x, lines shown from y; beyond them is see-through */
} RrFinePicture;
enum { RR_SPRITES_MAX = 80 };
typedef struct {
    const RrFinePicture *pictures;
    int16_t picture[RR_SPRITES_MAX]; /* per sprite table entry: index into pictures, or -1 for none */
    int16_t dx[RR_SPRITES_MAX];      /* how far sideways this entry's copy of the picture stands */
} RrFineSprites;
```

- Produces, in `port/render/rr_objects.h`:

```c
enum { RR_OBJECT_TILES = 0x150, RR_OBJECTS_NOTE_PC = 0xA4EE, RR_OBJECTS_DONE_PC = 0xA5C6 };
typedef struct {
    int ok;                       /* 0: nothing below is set */
    int first_sprite, sprites;    /* the objects' sprite-list entries are [first_sprite, sprites) */
    int first_tile, tiles;        /* their tiles in the buffer are [first_tile, tiles) */
    uint16_t tile_base;           /* 0xFFCE86: the video-memory tile number of buffer tile 0 */
    int pictures;
    RrFinePicture picture[RR_SPRITES_MAX];
    RrFineSprites fine;           /* fine.pictures points at `picture` */
    uint8_t list[RR_SPRITES_MAX * 8];        /* the sprite list as the game stages it */
    uint8_t tile_bytes[RR_OBJECT_TILES * 32];/* the picture buffer as the game stages it */
    unsigned records, dropped;
} RrObjectSet;
/* `ram` is work RAM (0x10000 bytes) when the main loop is at RR_OBJECTS_NOTE_PC.
 * `work` (0x10000 bytes) receives RAM as the game's drawing leaves it. Returns out->ok. */
int rr_objects_draw(const uint8_t *ram, const uint8_t *rom, size_t rom_size, RrObjectSet *out, uint8_t *work);
```

How a piece becomes an `RrFinePicture` (names from `_Drawer._draw`, values after it has moved `self.x`/`self.y`): `rows` = ROM at `data + 4`; `width`, `height` = the bytes at `data`, `data + 1`; `stride` as `_column_steps` returns; `size` = `self.size`; `y` = `self.y`; `first_row` = `self.skip_rows`; `rows_shown` = `last_row + 1` after the hidden-line cut; `columns` = `2 * produced`; `first_column` = twice the number of output bytes `_column_steps` dropped at the start (negative clip); not mirrored: `x` = `self.x`; mirrored: `x` = `self.x + 2 * produced`. Each sprite entry `_sprites` adds gets that picture and `dx` 0; each entry `_copies` adds gets its source entry's picture and that entry's `dx` plus the copy's offset. A record that is dropped takes its pictures and entries with it. A picture whose rows do not lie wholly inside the ROM leaves its entries at -1.

`rr-objects-draw ROM RAM OUT` (the tool): reads both files, calls `rr_objects_draw`, writes `work` to `OUT`, prints `ok N`, `sprites FIRST END`, `tiles FIRST END`, one `picture I OFFSET WIDTH HEIGHT STRIDE SIZE X Y MIRRORED FIRST_COLUMN FIRST_ROW COLUMNS ROWS` per picture (`OFFSET` = `rows` minus the ROM start) and one `entry I PICTURE DX` per object entry; exit 0, or 2 with a message on a file error.

`--objects-check` (replay tool): at `RR_OBJECTS_NOTE_PC` run the model; at `RR_OBJECTS_DONE_PC` compare `work` with RAM over the sprite list (80 × 8 bytes at the pointer in `0xFF27A6`), the picture buffer (0x150 × 32 bytes at the pointer in `0xFF27CE`) and the words at `0xFFDC30` and `0xFFDC34`. Prints `OBJECTS passes=N identical=M records=R dropped=D pictures=P first_difference=PASS|none`. Refused with `--resync`.

- [ ] **Step 1: Write the failing C tests** in `port/tests/test_objects.c`, with a synthetic ROM and RAM built as `tests/port/test_objects.py` builds them (`rom_with`, `race_ram`, `plain_picture`, `anchored_picture`):
  - `test_one_record_stages_the_sprites_python_stages`: picture 16×16, x 100, foot 120, natural size → one entry at (92, 104), 2×2 cells, tile `FIRST_TILE`, high priority; `tile_bytes` equal the picture's rows laid out down columns; picture 0 = {width 16, height 16, size 0x10000, x 92, y 104, columns 16, rows_shown 16, first_column 0, first_row 0, not mirrored}; `fine.picture[0] == 0`, `fine.dx[0] == 0`.
  - `test_mirrored_record_measures_from_its_right_end`: flags bit 15 → `mirrored`, `x == 108`.
  - `test_copies_share_the_picture_with_a_sideways_step`: flags `0x1200`, offset 40 → entries 1 and 2 have picture 0 and dx 40 and 80.
  - `test_two_part_record_makes_two_pictures`, `test_left_clip_sets_first_column`, `test_hidden_line_cuts_rows_shown`, `test_rows_above_the_view_set_first_row` (inputs as the Python tests of the same cases; expected values from running the Python model on the same input).
  - `test_dropped_record_leaves_no_picture`: `0xFFDC34` preset to 79 and a 64×64 picture → `sprites == 79`, `pictures == 0`, `dropped == 1`.
  - `test_picture_outside_rom_is_plain`: picture table entry pointing 8 bytes before the ROM's end → `ok`, entries -1, no out-of-bounds read (run under `-fsanitize=address,undefined` in Step 2).
  - `test_wild_pointers_and_counts_do_not_escape`: pointers `0xFFFFFFFF`, order list without a terminator, `0xFFDC34` = 0xFFFF, every record's picture index 0x3FFF → returns without touching memory outside `work` and the set.
  - `test_natural_size_fine_picture_is_the_plain_enlargement`: left for Task 2 (it needs the renderer); declare it there.
- [ ] **Step 2: Run them to see them fail.** Run: `make -C port test CFLAGS="-std=c11 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined"`. Expected: build fails, `rr_objects.h` not found.
- [ ] **Step 3: Implement `rr_objects_draw`** in `port/render/rr_objects.c` as a routine-for-routine port of `_Drawer` (`run`, `_record`, `_anchor`, `_edges`, `_centre`, `_draw`, `_sprites`, `_copy`, `_copies`, `_column_steps`, `_row_steps`), working on `work` (a copy of `ram`). Every ROM read goes through one bounds-checked helper that returns 0 past the end; every RAM access is masked to 16 bits; step arrays are fixed (1,032 column steps, 1,032 row steps) and a picture that would overflow them is dropped as out of room.
- [ ] **Step 4: Run the C tests.** Same command as Step 2. Expected: `all objects tests passed`, and the other suites pass.
- [ ] **Step 5: Write the differential test** `tests/port/test_objects_c.py`: build the tool with the harness's `compiler()` from `rr_objects_draw_tool.c` + `rr_objects.c`; `test_c_drawing_equals_python_on_random_scenes`: for seeds 0..149, 1 to 12 records with random picture sizes (2..70 wide, 2..90 high), sizes 0x0400..0xFFFF, x −40..360, foot 0..170, hidden 40..0x98, mirror, anchor, second picture, copies with offsets, irregular spacing; assert the tool's `OUT` equals `draw_objects_detail(...).ram` over the list, the buffer and the two counters, and that `sprites END` equals the Python sprite count. `test_tool_reports_a_missing_file`: exit 2.
- [ ] **Step 6: Run it.** Run: `uv run pytest tests/port/test_objects_c.py -q`. Expected: PASS (fix the C, not the Python, on a difference: the Python is the measured one).
- [ ] **Step 7: Add `--objects-check`** to `rr_race_replay.c` with tests in `tests/port/test_race_replay.py`: `test_objects_check_prints_counts` (synthetic race never reaches the PC: `OBJECTS passes=0 identical=0`), `test_objects_check_is_refused_with_resync`. Run: `uv run pytest tests/port/test_race_replay.py -q`. Expected: PASS.
- [ ] **Step 8: Measure on the recorded race** (owner's ROM; output stays under `generated/`):

```sh
make -C port race-replay
B=generated/port/build/rr-race-replay; H=generated/port/handoffs/full-throttle
$B --rom "Road Rash II (USA, Europe) (RR206).gen" --handoffs $H.bin --timesteps $H.timesteps --pads $H.pads --objects-check
```
Expected: `RESULT match` and `identical` equal to `passes`. If not, find the cause before going on (the Python model is identical on 2,174 of 2,174).
- [ ] **Step 9: Commit.** `git add port tests docs && git commit -m "Repeat the game's object drawing in C"`

### Task 2: Sprites drawn from a finer picture

**Files:**
- Modify: `port/hw/rr_vdp.h`, `port/hw/rr_vdp.c`, `port/hw/rr_vdp_render.c`, `port/hw/rr_genesis.h`, `port/hw/rr_genesis.c`
- Test: `port/tests/test_vdp.c`, `port/tests/test_objects.c`

**Interfaces:**
- Consumes: `RrFinePicture`, `RrFineSprites` (Task 1).
- Produces:

```c
/* rr_vdp.h */
uint64_t vram_writes;   /* new field of RrVdp: counts every change to video memory (data port, DMA, fill, copy) */
/* As rr_vdp_render_line_scaled, and a sprite table entry i with fine->picture[i] >= 0
 * takes its pixels from that picture. fine may be NULL. */
void rr_vdp_render_line_fine(const RrVdp *v, int line, int scale, const int32_t *shift_a,
                             const int32_t *shift_b, const RrFineSprites *fine, uint32_t *out);
/* rr_genesis.h, beside the hires fields */
const RrFineSprites *hires_sprites;  /* set by hires_begin; NULL for none */
uint64_t hires_sprites_stamp;        /* vdp.vram_writes when hires_sprites was chosen */
uint64_t hires_sprites_stale_lines;  /* lines drawn plain because video memory had changed since */
```

`rr_vdp_render_line_scaled` becomes a call with `fine` NULL. The sampling rule, for output column `c` (0 ≤ c < 320·S) and output row `q` (= line·S + r), sprite entry `i`, picture `p`, all in integers:

```
edge = (p.x + dx[i]) * S
u = p.mirrored ? edge - 1 - c : c - edge          drawn only if 0 <= u < p.columns * S
w = q - p.y * S                                   drawn only if 0 <= w < p.rows_shown * S
sx = ((2 * (u + p.first_column * S) + 1) << 16) / (2 * S * p.size)      (64-bit)
sy = p.first_row + ((2 * w + 1) << 16) / (2 * S * p.size)
colour = (sx < p.width && sy < p.height) ? nibble sx of row sy : 0
```

The entry still covers only its own rectangle, takes its colour set and priority from its own attribute word, and obeys the same order and per-line limits (counted per screen pixel, as now); a see-through fine pixel lets the next sprite or the planes show. Entries without a picture are their tiles, each pixel repeated. `p.size == 0` is treated as no picture.

The genesis line loop passes `hires_sprites` when `vdp.vram_writes == hires_sprites_stamp`, else NULL and counts a stale line.

- [ ] **Step 1: Write the failing tests** in `port/tests/test_vdp.c` (a 4×4 picture with 16 different pixels, tiles holding its scaled form, one 1×1-cell sprite at screen (16, 8), scale 3 unless said):
  - `test_fine_null_is_render_line_scaled`: `fine` NULL equals `rr_vdp_render_line_scaled` on lines with and without sprites, with and without shifts.
  - `test_fine_picture_at_natural_size_repeats_each_pixel`: size 0x10000 → picture pixel (sx, sy) fills output columns 48+3sx..50+3sx on rows 24+3sy..26+3sy.
  - `test_fine_picture_at_half_size_shows_every_source_pixel`: size 0x8000, scale 2, columns 2, rows_shown 2 → the 4×4 picture appears whole in output columns 32..35, rows 16..19.
  - `test_fine_picture_at_double_size_gives_each_pixel_equal_room`: size 0x20000, columns 8 → each picture pixel is 6 output pixels wide.
  - `test_fine_picture_mirrored_runs_right_to_left`, `test_fine_first_column_and_first_row_skip_into_the_picture`, `test_fine_columns_and_rows_shown_end_the_picture`, `test_fine_dx_moves_a_copy`.
  - `test_fine_sprite_keeps_order_priority_and_colour_set`: two overlapping entries, the first in link order wins where it is opaque and lets the second show where it is see-through; a high-priority plane pixel covers a low-priority fine sprite.
  - `test_entry_without_picture_is_its_tiles`, `test_fine_size_zero_is_plain`.
  - `test_vram_writes_counts_every_kind_of_write`: data port, DMA from memory, fill, copy each raise `vram_writes`; CRAM and VSRAM writes do not.
  - `test_genesis_drops_fine_sprites_when_video_memory_changes` (in `test_console.c` if it needs the machine): with `hires_sprites` set and the stamp one behind, the frame's lines are the plain enlargement and `hires_sprites_stale_lines` is 224.
  - In `port/tests/test_objects.c`: `test_natural_size_fine_picture_is_the_plain_enlargement`: draw records (plain, mirrored, with copies, anchored two-part) at natural size with `rr_objects_draw`, copy `list` and `tile_bytes` into a video chip as the game's block copies would (list at the sprite table, tiles from `tile_base`), and assert `rr_vdp_render_line_fine` with the set's `fine` equals it with NULL on every line, at scales 2 and 3. (This ties the model's geometry to the renderer's rule. `test_objects` then links the hardware sources.)
- [ ] **Step 2: Run to see them fail.** Run: `make -C port test`. Expected: build fails, `rr_vdp_render_line_fine` undeclared.
- [ ] **Step 3: Implement** the counter in `rr_vdp.c`, `rr_vdp_render_line_fine` in `rr_vdp_render.c` (a per-output-row sprite layer used only on lines where some sprite on the line has a picture; other lines keep today's paths), and the genesis fields.
- [ ] **Step 4: Run all suites.** Run: `make -C port test && uv run pytest -q`. Expected: all pass.
- [ ] **Step 5: Commit.** `git commit -am "Draw sprites from a finer picture in the enlarged frame"`

### Task 3: Which object set is on screen, and the measurement

**Files:**
- Modify: `port/render/rr_objects.h`, `port/render/rr_objects.c`, `port/tools/rr_race_replay.c`
- Test: `port/tests/test_objects.c`, `tests/port/test_race_replay.py`
- Modify: `analysis/race-picture.md` (new section 7), `analysis/memory-map.md` if an address is new

**Interfaces:**
- Consumes: `rr_objects_draw`, `RrObjectSet`, `rr_vdp_render_line_fine`, genesis `hires_sprites*`.
- Produces:

```c
enum { RR_OBJECT_RECENT = 4 };
typedef struct {
    RrObjectSet recent[RR_OBJECT_RECENT];
    int count, newest, chosen;       /* chosen: index in recent, or -1 */
    uint8_t work[0x10000];
    uint64_t noted, frames, fine_frames; /* frames: choose calls with a set noted */
} RrObjectTracker;
void rr_object_tracker_reset(RrObjectTracker *t);
/* Call when the main loop reaches RR_OBJECTS_NOTE_PC. */
void rr_object_tracker_note(RrObjectTracker *t, const uint8_t *ram, const uint8_t *rom, size_t rom_size);
/* Call before a frame's first line. The newest noted set whose sprite entries
 * (all bytes but the link) and tiles are what video memory holds, or NULL. */
const RrFineSprites *rr_object_tracker_choose(RrObjectTracker *t, const RrVdp *v);
```

A set matches when it is `ok`, has at least one object entry, and for every entry in `[first_sprite, sprites)` bytes 0, 1, 2, 4, 5, 6, 7 at `sprite table + 8 i` equal `list`, and video memory from `(tile_base + first_tile) * 32` equals `tile_bytes` for the object tiles. The sprite table address is register 5's.

Replay tool: `--fine-objects` (needs `--scaled-check S`; `--crisp-points` gains the value `none`, meaning no road shifts). With it the tracker is noted at `RR_OBJECTS_NOTE_PC` and chosen in `hires_begin`; the existing `off_road_identical` and `sampled_back_identical` checks skip screen pixels inside the rectangle of a sprite entry that has a picture (so they still prove that nothing outside the objects changed). Prints `FINE_OBJECTS frames=N set_frames=K fine_frames=F stale_lines=L entries=E fine_entries=G covered_pixels=P same_at_centre=Q`: `K` frames with a set noted, `F` frames drawn with one, `E` sprite entries linked on those frames and `G` those with a picture, `P` screen pixels inside fine entries' rectangles and `Q` those whose centre output pixel has the original pixel's colour.

- [ ] **Step 1: Write the failing tests.** `port/tests/test_objects.c`: `test_tracker_picks_the_set_in_video_memory` (two sets noted with different tile bases; video memory holds the older → it is chosen; then the newer's bytes → the newer), `test_tracker_ignores_the_link_byte`, `test_tracker_returns_null_when_a_tile_byte_differs`, `test_tracker_returns_null_with_no_objects`, `test_tracker_reset_forgets`. `tests/port/test_race_replay.py`: `test_fine_objects_needs_scaled_check`, `test_fine_objects_prints_counts_and_does_not_change_the_race`, `test_crisp_points_none_is_accepted`.
- [ ] **Step 2: Run to see them fail.** `make -C port test; uv run pytest tests/port/test_race_replay.py -q`. Expected: FAIL (undeclared tracker; unknown option).
- [ ] **Step 3: Implement** the tracker and the tool option.
- [ ] **Step 4: Run all suites.** `make -C port test && uv run pytest -q`. Expected: all pass.
- [ ] **Step 5: Measure on the recorded race**, scale 3, three runs: `--crisp-points none --fine-objects`; `--crisp-points fine --fine-objects`; `--crisp-points fine` (unchanged from before this plan). Write frames 4000 and two others with `--scaled-at` with and without `--fine-objects` under `generated/port/draw/` and look at them. Expected: `RESULT match` in all three; `off_road_identical` and `sampled_back_identical` as before this plan; `stale_lines=0`; `fine_frames` close to the race's frames. Anything else is investigated before it is written down.
- [ ] **Step 6: Write `analysis/race-picture.md` section 7** with the commands, the numbers, what is verified (the C model equals the game; nothing outside object rectangles changes; which frames are fine) and what is not (how the fine picture looks is by eye; `same_at_centre` is reported, not a pass mark, and why it is below 100%: the game scales sideways in pixel pairs and its scaler has a different phase).
- [ ] **Step 7: Commit.** `git commit -am "Track the object set on screen and measure fine objects on the race"`

### Task 4: Console, Godot window, documents

**Files:**
- Modify: `port/host/rr_console.h`, `port/host/rr_console.c`, `port/host/rr_godot.c`, `godot/scripts/console.gd`, `godot/tests/test_console.gd`, `godot/tests/test_console_scene.gd`, `port/tests/test_console.c`, `godot/README.md`, `README.md`, `docs/superpowers/specs/2026-11-11-draw-data-contract.md`

**Interfaces:**
- Consumes: `RrObjectTracker` and its functions.
- Produces:

```c
void rr_console_set_fine_objects(RrConsole *c, int on);   /* on by default; acts while the crisp road is on and the scale is above 1 */
int rr_console_fine_objects(const RrConsole *c);
uint64_t rr_console_fine_object_frames(const RrConsole *c);
void rr_console_note_objects(RrConsole *c);               /* test hook: as if the main loop reached RR_OBJECTS_NOTE_PC */
```
Godot methods `set_fine_objects(bool)`, `get_fine_objects() -> bool`; `console.gd` `toggle_fine_objects()` on the O key, showing "Objects from the full pictures" or "Objects enlarged as the original draws them".

- [ ] **Step 1: Write the failing tests.** `port/tests/test_console.c` (the test ROM gains the sprite-shape table and one picture): `test_fine_objects_are_on_by_default_and_can_be_switched`, `test_console_draws_a_noted_set_fine_and_off_draws_it_plain` (stage a set by hand in RAM and video memory, `rr_console_note_objects`, run a frame: `rr_console_fine_object_frames` rises and the scaled frame differs from the enlargement inside the sprite at half size; switched off: equal), `test_reset_forgets_noted_objects`, `test_fine_objects_need_the_crisp_picture`. `godot/tests/test_console.gd` and `test_console_scene.gd`: the O key toggles and shows the message; nothing happens without a console.
- [ ] **Step 2: Run to see them fail.** `make -C port test`. Expected: FAIL, functions undeclared. (Godot tests run in Step 4, after the library is rebuilt.)
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Say that the library is about to be rebuilt, rebuild it, run everything.** `scripts/build-godot-extension.sh && make -C port test && uv run pytest -q && uv run ruff check`. Expected: all pass.
- [ ] **Step 5: Final measurement** (Task 3's first two runs again on the committed code, plus `run_ms_per_frame` at scale 3 with and without fine objects) and update section 7 if a number moved.
- [ ] **Step 6: Documents.** Keys in `README.md` and `godot/README.md`; "State after high-resolution objects" in the contract: what is done, what the owner should look at (far objects, near objects, the O comparison), what is left (placement between pixels, upscaled art, dashes, interpolation).
- [ ] **Step 7: Commit.** `git commit -am "Fine objects in the console and the Godot window"`
