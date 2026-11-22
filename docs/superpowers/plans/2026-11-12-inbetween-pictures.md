# In-between Pictures Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Draw a new picture of the race on every video frame, part-way between (or just past) the game's own updates, in three modes (predict, blend, late blend), without touching the game.

**Architecture:** Each time a new update appears on screen it is recorded with the frame number it appeared in ("stamp"): what the road was projected from, the work RAM the objects were drawn from, and the sideways-shift table in video memory. For a video frame, a clock reads the straight path through the stamped updates at "now minus a delay" and gives a pair of updates A, B and a place `t` between them (above 1: past B). The scene at `t` is made by (1) projecting the road again from a camera part-way between A's and B's, (2) taking matched draw records part-way and running the port's own copy of the game's object drawing (`rr_objects_draw`) on them, (3) taking the sky and hill sideways shifts part-way. The existing enlarged renderer draws it; two small additions let it place pictures between pixels and take its sprite list from the scene instead of video memory.

**Tech Stack:** C11 (`port/`), Python 3 with pytest via `uv` (tests and measurement glue), Godot 4 GDExtension (`godot/`).

**Spec:** `docs/superpowers/specs/2026-11-12-interpolation-design.md` (stages 1 and 2; stage 3, the centre-line dashes, is not in this plan).

## Global Constraints

- The game is untouched: CPU, memory and video chip model run exactly as now; `rr-race-replay` ends in `RESULT match` with every new option on. In-between code only reads.
- With in-between pictures off, every frame is byte for byte what the current build draws.
- Never a wrong picture for the sake of smoothness: if the two updates cannot be described (no verified road inputs, no object set in video memory, two-player, menu), the frame is drawn as today.
- Nothing derived from the ROM is committed (ROM, generated C, traces, captures, pictures, dumps, logs, RAM images stay under `generated/`, `godot/bin/`). Test data is synthetic.
- Every claim in `analysis/` is marked **verified** or **hypothesis**; nothing is called "the same" without a measurement.
- Do not rebuild `godot/bin/librr2.dylib` while a Godot window may be using it; say so to the owner before the final rebuild (Task 7).
- Work on branch `inbetween`. Commit as the tasks say. Merging and pushing are the owner's decision.
- Tests: `make -C port test` (C suites), `uv run pytest -q` (everything). Both green at the end of every task.
- Plain words in user-facing text and in `analysis/`: "update" for a pass of the game's main loop, "in-between picture", "predict", "blend", "late blend".

## Rulings made while planning (to be confirmed by the owner with the plan)

- **Roadside objects are taken part-way between their two draw records**, not re-placed by the roadside formulas for the in-between camera as the spec's "Approach" item 2 says. Why: the ends are then the game's own records by construction, the code is a fraction of the size, and one mechanism serves roadside objects and vehicles. Size goes through distance (1/size is taken part-way), which follows perspective. What it costs if wrong: objects could drift against the road edge by a fraction of a pixel between updates; Task 6's leave-one-out measurement (below) would show it, and the formulas remain available as a follow-up. Task 0 amends the spec to say this.
- **Leave-one-out measurement** added to the spec's list: the scene made from updates k−1 and k+1 at the moment of update k is compared with the game's real update k. It is the only measurement of in-between pictures against something the game actually drew.
- **An in-between sprite entry with no full picture** (the drawing could not describe it; 42 of 395,559 entries in the recorded race) makes that frame fall back to today's picture. Counted.

## Review Focus

1. **The game is paused or the bike is stopped, in predict.** Expected: the picture holds still; it does not run on or drift. (Test in Task 5: `test_clock_holds_when_no_update_arrives`, `test_no_motion_gives_no_motion`.)
2. **A crash or restart moves the camera a long way in one update.** Expected: the new update is shown at once, nothing sweeps across the screen. (Tests in Task 3 and Task 5: `test_camera_jump_is_not_slid`, `test_far_moved_object_snaps`.)
3. **An update that cannot be described arrives between two good ones** (object set not in video memory, road without inputs). Expected: frames fall back to today's picture until two good updates in a row exist again; no pair spans the gap. (Task 5: `test_pair_never_spans_a_broken_update`.)
4. **Switching mode, the crisp road, fine objects or CPU speed mid-race.** Expected: no stale pair is used; at worst a few frames of today's picture. (Task 7: `test_switching_modes_mid_race_never_uses_stale_updates`.)
5. **Wild memory contents** (menus, two-player, garbage vehicle list with no end mark, record count above the table). Expected: no read outside the buffers; the frame falls back. (Task 3: `test_wild_ram_is_refused`; run under the sanitizer build in Task 6.)

---

## File Structure

| File | Responsibility |
|---|---|
| `port/hw/rr_vdp.h`, `rr_vdp_render.c` | Renderer: fraction-of-a-pixel placement of fine pictures; sprite list taken from a supplied table |
| `port/render/rr_road.h`, `rr_road.c` | Road inputs gain the camera's place; the tracker keeps each recent road's inputs and RAM; `rr_road_inputs_between` |
| `port/render/rr_between.h`, `rr_between.c` (new) | Matching draw records, records part-way, the in-between object set and sprite table, the update history, the clock, the scene for a frame |
| `port/tests/test_between.c` (new) | Unit tests for the above, synthetic data |
| `port/tools/rr_race_replay.c` | `--inbetween MODE`, `--inbetween-check`; measurements |
| `port/host/rr_console.h/.c`, `rr_godot.c`, `godot/scripts/console.gd` | Mode setting, key I, messages |
| `analysis/race-picture.md` | Section 8 with the measurements |

---

### Task 0: Housekeeping

**Files:**
- Modify: `analysis/race-picture.md` (already repaired in the working tree: duplicate sections 6 and 7 removed)
- Modify: `docs/superpowers/specs/2026-11-12-interpolation-design.md`
- Modify: `docs/wishlist.md`

- [ ] **Step 1:** `git checkout -b inbetween`.
- [ ] **Step 2:** Check the repaired analysis file: `git diff 5710894 -- analysis/race-picture.md | grep -c '^[-+][^-+]'`. Expected: `25` (18 added, 7 removed: only the review's rewording), and `grep -c '^## ' analysis/race-picture.md` gives `8`.
- [ ] **Step 3:** In the spec, replace "Approach" item 2's last two sentences so it says matched roadside objects are taken part-way between their two draw records (size through distance), marked "(builder's ruling in the plan)"; add the leave-one-out line to "Measurements to report"; fix "removes the second copy" to "removes the duplicate" in the note on the analysis file.
- [ ] **Step 4:** In `docs/wishlist.md`, under "Opponents" in "Ideas not yet discussed", extend the line to: "riders with grudges and rivalries that carry across races, for every rider (the original has this for one rider only: `analysis` side note of 2026-11-12, partly verified)".
- [ ] **Step 5:** `uv run pytest -q` → all pass. Commit: `git add analysis/race-picture.md docs/ && git commit -m "Design for in-between pictures; repair duplicated analysis sections"`.

---

### Task 1: The renderer places pictures between pixels and takes a supplied sprite list

**Files:**
- Modify: `port/hw/rr_vdp.h`, `port/hw/rr_vdp_render.c`
- Test: `port/tests/test_vdp.c`

**Interfaces:**
- Produces, in `RrFinePicture`: `uint16_t fx, fy;` — how far right and down of (`x`, `y`) the picture really stands, in 65,536ths of a screen pixel. Zero (the value every existing caller leaves, since they zero the struct) means today's behaviour exactly.
- Produces, in `RrFineSprites`: `const uint8_t *table;` — if not NULL, 640 bytes (80 entries of 8) used in place of the sprite table in video memory, for the whole walk (links, positions, sizes, tiles of entries without a picture). NULL means video memory, as today.

Sampling with a fraction (replaces the rule in the header comment; `S` is the scale, `FX = floor(fx * S / 65536)`, `FY` likewise, in output pixels):

```
u = mirrored ? (x + dx) * S + FX - 1 - c : c - (x + dx) * S - FX
w = q - y * S - FY
```

An entry whose picture has `fx` or `fy` non-zero covers its own rectangle grown by one screen pixel to the right and one down (so the moved picture is not cut at the entry's edge; neighbouring entries of one picture sample the same pixels, so the overlap is harmless). The per-line limits count the entry's own width, as today.

- [ ] **Step 1: Write the failing tests** in `port/tests/test_vdp.c`, in the style of the existing fine tests (same helpers):

```c
/* A fine picture with fx = 0x8000 at scale 4 is the same picture two output columns to the right. */
static void test_fraction_moves_a_fine_picture_by_output_pixels(void);
/*   render line L with fx = 0 into a[], with fx = 0x8000 into b[];
     for every c in 2..1279: b[c] == a[c - 2] wherever a[c - 2] is a sprite pixel,
     and b[] shows the plane wherever a[c - 2] shows the plane. Same for fy = 0x4000
     against the row one output row up. */
/* A mirrored picture moves right too, not left. */
static void test_fraction_moves_a_mirrored_picture_right(void);
/* With fx = 0 and fy = 0 the output is byte for byte today's (compare against
   a copy of the picture struct made before the fields existed: all 224 lines). */
static void test_zero_fraction_changes_nothing(void);
/* A picture exactly as wide as its sprite (16 pixels in a 16-wide entry) with fx = 0x8000
   still shows its last column: output columns (x + 16) * 4 .. (x + 16) * 4 + 1 are sprite pixels. */
static void test_fraction_is_not_cut_at_the_entry_edge(void);
/* A supplied table is used instead of video memory: video memory's table holds one sprite at
   x = 10, the supplied table one at x = 200; only the one at 200 is drawn. With table = NULL
   only the one at 10. Links are followed in the supplied table (two entries, second linked). */
static void test_supplied_sprite_table_replaces_video_memory(void);
```

- [ ] **Step 2:** `make -C port test` → FAIL to compile (`fx`, `table` not members).
- [ ] **Step 3:** Add the fields and implement in `rr_vdp_render.c`: every read of the sprite table in the fine path goes through one helper that picks `fine->table` or video memory; the three sprite walks (`line_has_fine_sprite`, the limits walk, `render_sprites_fine`) use it. Update the header comment with the rule above.
- [ ] **Step 4:** `make -C port test` → all five suites pass. `uv run pytest -q` → all pass.
- [ ] **Step 5:** Measure nothing changed on the race: run the replay with `--scaled-check 3 --crisp-points fine --fine-objects` and compare its `SCALED` and `FINE_OBJECTS` lines with the figures in `analysis/race-picture.md` section 7 (same_at_centre 64,240,400; stale_lines 0). Expected: identical.
- [ ] **Step 6:** Commit: `git commit -am "Fine pictures between pixels; sprite list from a supplied table"`.

---

### Task 2: The road from a camera between two updates

**Files:**
- Modify: `port/render/rr_road.h`, `port/render/rr_road.c`
- Test: `port/tests/test_road.c`

**Interfaces:**
- Produces, in `RrRoadInputs`: `uint32_t place;` — the long at `0xFF0A0A` (segment in the high word, fraction in the low word; `fraction` stays).
- Produces, in `RrRoadTracker`: `RrRoadInputs recent_inputs[RR_ROAD_RECENT];` (the inputs attached to each recent road, valid where `has_fine`) and `uint8_t inputs_ram[RR_ROAD_RECENT][0x10000];` kept beside `inputs[]` (work RAM as it stood when those inputs were noted), with `uint8_t recent_ram_index[RR_ROAD_RECENT]` saying which `inputs_ram` belongs to each recent road.
- Produces: `const RrRoadInputs *rr_road_tracker_chosen_inputs(const RrRoadTracker *t, const uint8_t **ram);` — the chosen road's inputs and that RAM, or NULL if the chosen road has none.
- Produces:

```c
/* What the game would project from with its camera part-way from a's to b's.
 * t is 16.16: 0 is a, 0x10000 is b, above is past b. The place, sideways
 * position and height are taken part-way (rounded to nearest); the view
 * lines, reference line and screen centre are taken part-way and rounded to
 * nearest whole; the track from the camera on is read from `ram_b` (work RAM
 * noted with b) at the in-between segment. Returns 0, leaving `out` alone,
 * if a and b differ in `count`, or the place moves back, or by more than
 * RR_ROAD_JUMP (4 segments) from a to b: that is a jump, not motion. */
int rr_road_inputs_between(const RrRoadInputs *a, const RrRoadInputs *b, const uint8_t *ram_b,
                           int32_t t, RrRoadInputs *out);
enum { RR_ROAD_JUMP = 4 << 16 };
```

- [ ] **Step 1: Write the failing tests** in `test_road.c` (use its existing synthetic RAM builder):

```c
static void test_inputs_between_at_zero_is_a(void);      /* same ring in both: out equals a field for field */
static void test_inputs_between_at_one_is_b(void);       /* t = 0x10000: out equals b field for field */
static void test_inputs_between_halfway(void);
/*   a.place = 0x00050000, b.place = 0x00058000, sideways 10 -> 30, height 100 -> 104:
     at t = 0x8000: place 0x00054000, fraction 0x4000, sideways 20, height 102, curve/hill = segment 5's window */
static void test_inputs_between_crosses_a_segment(void);
/*   a.place = 0x0005C000, b.place = 0x00064000, t = 0x8000: place 0x00060000 and
     out.curve[0] is the ring's entry for segment 6 */
static void test_inputs_between_past_b(void);            /* t = 0x18000 continues: place = b + (b - a) / 2 */
static void test_camera_jump_is_not_slid(void);          /* b.place - a.place = 5 segments, and b before a: both return 0 */
static void test_projecting_between_is_between(void);
/*   straight flat road, a and b half a segment apart: every fine point's line at t = 0x8000
     lies between its lines at t = 0 and t = 0x10000 (inclusive) */
static void test_tracker_keeps_the_chosen_roads_inputs(void);
/*   note inputs, note road, choose (video memory set to match): rr_road_tracker_chosen_inputs
     returns inputs with the noted place and a RAM whose bytes equal the RAM at the inputs note */
```

- [ ] **Step 2:** `make -C port test` → FAIL to compile.
- [ ] **Step 3:** Implement. `rr_road_inputs_between` copies `ram_b` nowhere: it reads the ring with the same indexing `rr_road_read_inputs` uses (factor that loop into a static helper taking the place), then fills the scalars.
- [ ] **Step 4:** `make -C port test` → pass. `uv run pytest -q` → pass.
- [ ] **Step 5:** Commit: `git commit -am "Road inputs between two updates; tracker keeps each road's inputs"`.

---

### Task 3: The objects between two updates

**Files:**
- Create: `port/render/rr_between.h`, `port/render/rr_between.c`
- Create: `port/tests/test_between.c`
- Modify: `port/Makefile` (add `rr_between.c` to `RENDER_SRC`; `test_between` to `test`)

**Interfaces:**
- Consumes: `rr_objects_draw`, `RrObjectSet` (`rr_objects.h`); Task 1's `fx`, `fy`, `table`.
- Produces:

```c
enum { RR_BETWEEN_RECORDS = 96 };   /* draw records looked at; more makes the image "not covered" */

/* One draw record (22 bytes at 0xFFCEB8 + 22 * slot) with what identifies it. */
typedef struct {
    int16_t x, foot, hidden, offset;
    uint16_t size, picture, flags, roadside, second, segment;
    uint32_t key;    /* 0: cannot be followed */
} RrBetweenRecord;

typedef struct {
    int ok, count;
    RrBetweenRecord record[RR_BETWEEN_RECORDS];
} RrBetweenRecords;

/* Read and identify the records of a work RAM image taken at RR_OBJECTS_NOTE_PC.
 * Keys: a vehicle record (its long in the table at 0xFF06D6 equals a listed
 * vehicle's place, +4, minus the camera's place, 0xFF0A0A) gets
 * 0x80000000 | the vehicle's address; a roadside record (distance or roadside
 * byte not zero) gets a key made of segment, side bit, picture and roadside
 * byte; a key that occurs twice in the image is set to 0 in both. ok = 0 if
 * the count (0xFFDC42) is above RR_BETWEEN_RECORDS or the vehicle list
 * (0xFF04F8, longs) has no end mark within 32 entries. */
void rr_between_read(const uint8_t *ram, RrBetweenRecords *out);

/* The objects at t (16.16) between the images a and b (both at RR_OBJECTS_NOTE_PC).
 * The base image is a below 0x10000 and b from there on. Each base record
 * whose key is also in the other image is taken part-way: x, foot, hidden
 * line and offset linearly, size through 1/size; the whole parts go into a
 * copy of the base RAM (`scratch`, 0x10000 bytes), which rr_objects_draw then
 * draws; the fractions of x and foot go into the pictures' fx, fy. Past
 * 0x10000 the motion from a to b is carried on. The picture, second picture
 * and mirror bit are the base's below 0x8000 and from 0x10000 on, b's
 * between. A matched record that moved more than RR_BETWEEN_JUMP_X (64)
 * pixels, RR_BETWEEN_JUMP_FOOT (32) lines or to more than twice or less than
 * half the size is not taken part-way (it snaps with its update). Unmatched
 * records are drawn as the base has them. Returns out->ok; 0 also when any
 * sprite entry of the result has no full picture. */
int rr_between_objects(const uint8_t *ram_a, const uint8_t *ram_b, const uint8_t *rom,
                       size_t rom_size, int32_t t, RrObjectSet *out, uint8_t *scratch,
                       uint8_t *work);
enum { RR_BETWEEN_JUMP_X = 64, RR_BETWEEN_JUMP_FOOT = 32 };

/* The sprite table to draw with: `set`'s entries first, then every entry of
 * video memory's sprite chain that is not one of `on_screen`'s object entries
 * (the centre-line dashes and anything else the game added), in chain order,
 * links renumbered. `table` is 640 bytes; fine->table is set to it and
 * fine->picture / dx are filled for the first part and -1 for the rest.
 * Returns 0 if the two parts do not fit in 80 entries. */
int rr_between_sprite_table(const RrObjectSet *set, const RrObjectSet *on_screen, const RrVdp *v,
                            uint8_t *table, RrFineSprites *fine);
```

- [ ] **Step 1: Write the failing tests** in `test_between.c`. Build images with the helpers `test_objects.c` uses for a ROM with one picture and a RAM with records (copy the two small builders; add `put_vehicle(ram, index, address, place)` and `put_camera(ram, place)`):

```c
static void test_roadside_record_is_keyed_by_segment_side_picture(void);   /* same record in two images: equal non-zero keys; other side: different key */
static void test_vehicle_record_is_keyed_by_its_vehicle(void);             /* slot differs between images, key equal; key & 0x80000000 */
static void test_duplicate_keys_are_not_followed(void);                    /* two identical roadside records: both keys 0 */
static void test_wild_ram_is_refused(void);                                /* count 0xFFFF; list of 0x00FF0A62 repeated with no end: ok == 0, no crash */
static void test_objects_at_zero_are_a(void);
/*   rr_between_objects(a, b, t = 0) gives a set whose list, pictures and tile_bytes equal
     rr_objects_draw(a)'s, and every fx, fy is 0 */
static void test_objects_at_one_are_b(void);                               /* likewise for t = 0x10000 and b */
static void test_matched_record_is_halfway(void);
/*   one roadside object, x 100 -> 105, foot 150 -> 151, size 0x2000 -> 0x4000 at t = 0x8000:
     picture x corresponds to record x 102 with fx = 0x8000; fy = 0x8000 on foot 150;
     size is 0x2AAA or 0x2AAB (1/size halfway), not 0x3000 */
static void test_motion_is_carried_past_b(void);                           /* t = 0x18000: record x 107 with fx 0x8000 */
static void test_unmatched_records_snap(void);
/*   a has objects P and Q, b has P and R: at t = 0x8000 the set has P (halfway) and Q as in a,
     no R; at t = 0x10000 it has P and R as in b */
static void test_far_moved_object_snaps(void);                             /* x 100 -> 170: at t = 0x8000 it stands at 100, fx 0 */
static void test_pose_changes_at_the_halfway_point(void);
/*   vehicle with picture 1 in a, 2 in b: t = 0x7FFF shows picture 1's rows, t = 0x8000 picture 2's */
static void test_sprite_table_puts_the_games_other_sprites_after(void);
/*   on-screen set of 2 entries in video memory followed by 3 linked dash entries; in-between set of
     3 entries: table has 6 entries, links 1,2,3,4,5,0; entries 3..5 equal the dash entries apart
     from the link byte; fine->picture is >= 0 for 0..2 and -1 for 3..5 */
static void test_sprite_table_refuses_more_than_80(void);
```

- [ ] **Step 2:** `make -C port test` → FAIL (no such file / functions).
- [ ] **Step 3:** Implement `rr_between.c`. All RAM reads masked to the image; the vehicle address read from the list is used only as a masked offset.
- [ ] **Step 4:** `make -C port test` → six suites pass. `uv run pytest -q` → pass.
- [ ] **Step 5:** Commit: `git add port && git commit -m "Objects between two updates: matching, part-way records, sprite table"`.

---

### Task 4: A bounded look at the records that cannot be followed

About 7% of draw records are neither roadside nor tied to a vehicle (pictures 501/502 with flags 0x6100/0xE100 in 60% of updates, and a mixed remainder). This task spends at most one sitting on them.

**Files:**
- Modify (only if the rule below qualifies): `port/render/rr_between.c`, `port/tests/test_between.c`
- Modify: `analysis/race-picture.md` (a paragraph for section 8, written in Task 6)

- [ ] **Step 1:** With a throwaway script over `generated/port/draw/before-draw.bin` (scratchpad, not committed), for records with no key, test the candidate key "picture and the high byte of the flags, when that pair occurs once in both updates": report the share of such records it matches to the update before, and the error of carrying the last motion forward (median, 90th, 99th percentile in x).
- [ ] **Step 2:** Decision rule: adopt the key (as `0x40000000 | picture << 8 | flags >> 8`) if it matches at least 80% of keyless records and its 90th-percentile carry-forward error in x is at most 8 pixels. Otherwise leave them snapping.
- [ ] **Step 3 (only if adopted):** Failing test `test_other_record_is_keyed_by_picture_and_flags` (same record in two images gets equal keys with bit 30 set; two such records in one image get 0), run to see it fail, implement in `rr_between_read`, run to pass.
- [ ] **Step 4:** Note the figures and the decision for Task 6's write-up (ledger line). Also look at where the code that makes the 501/502 records is (`--ram-writes` on their slots for a few updates) and record the routine's address; do not port it.
- [ ] **Step 5:** `make -C port test && uv run pytest -q` → pass. Commit if anything changed: `git commit -am "Follow other draw records by picture and flags"`.

---

### Task 5: Update history, the clock, and the scene for a frame

**Files:**
- Modify: `port/render/rr_between.h`, `port/render/rr_between.c`
- Modify: `port/render/rr_objects.h`, `port/render/rr_objects.c`
- Test: `port/tests/test_between.c`, `port/tests/test_objects.c`

**Interfaces:**
- Produces, in `RrObjectTracker`: `uint8_t ram[RR_OBJECT_RECENT][0x10000];` (work RAM as each kept set was noted, filled by `rr_object_tracker_note`) and `const uint8_t *rr_object_tracker_chosen_ram(const RrObjectTracker *t);` (the chosen set's, or NULL).
- Consumes: Task 2's `rr_road_inputs_between`, `rr_road_project`, `rr_road_shifts`, `rr_road_shifts_fine`; Task 3's functions.
- Produces:

```c
enum { RR_BETWEEN_OFF, RR_BETWEEN_PREDICT, RR_BETWEEN_BLEND, RR_BETWEEN_LATE };
enum { RR_BETWEEN_UPDATES = 6, RR_BETWEEN_LENGTHS = 16 };

typedef struct {
    int good;                      /* 0: this update could not be described */
    uint64_t stamp;                /* the video frame it first showed in */
    RrRoadInputs inputs;
    uint8_t road_ram[0x10000];     /* work RAM noted with the inputs */
    uint8_t object_ram[0x10000];   /* work RAM at RR_OBJECTS_NOTE_PC */
    uint8_t shifts[896];           /* video memory's sideways-shift table when it showed */
} RrBetweenUpdate;

typedef struct {
    int mode;
    RrBetweenUpdate update[RR_BETWEEN_UPDATES];   /* a ring; newest at `newest` */
    int count, newest;
    uint8_t length[RR_BETWEEN_LENGTHS];           /* frames each recent update lasted */
    int lengths;
    /* the scene made by rr_between_frame */
    RrRoadShifts whole, shifts;    /* as RrRoadTracker's: what to check against, what to draw with */
    RrObjectSet set;
    uint8_t table[640];
    RrFineSprites fine;
    uint8_t scratch[0x10000], work[0x10000];
    uint64_t frames, between_frames, guessed_frames;   /* race frames asked for; drawn in-between; of those, past the newest update */
    uint64_t fell_back[4];         /* reasons: RR_BETWEEN_NO_PAIR, _ROAD, _OBJECTS, _TABLE */
} RrBetween;
enum { RR_BETWEEN_NO_PAIR, RR_BETWEEN_ROAD, RR_BETWEEN_OBJECTS, RR_BETWEEN_TABLE };

void rr_between_reset(RrBetween *b);            /* the mode is kept */
void rr_between_set_mode(RrBetween *b, int mode);   /* also forgets the updates */

/* Call before a frame's first line, after the road and object trackers have
 * chosen. If what they chose is not the newest update kept, it is added with
 * stamp `frame` (good only if the road has inputs and a set was chosen). */
void rr_between_note(RrBetween *b, uint64_t frame, const RrRoadTracker *road,
                     const RrObjectTracker *objects, const RrVdp *v);

/* The pair and place for `frame`: the delay is 0 (predict), the mean of the
 * kept lengths (blend) or their largest (late blend), in 16.16 frames; the
 * pair is the two neighbouring good updates around frame - delay, or the two
 * newest when it is past the newest, with t above 0x10000 but never above
 * 0x20000 (the picture then holds). Returns 0 if there is no such pair of
 * good neighbours. */
int rr_between_clock(const RrBetween *b, uint64_t frame, int *older, int *newer, int32_t *t);

/* Make the scene for `frame` in b->shifts / b->whole / b->fine. Returns 1 if
 * it is to be drawn; 0 (and counts why) if the frame is to be drawn as today.
 * `whole` is always the road tracker's (what video memory holds). Lines the
 * road does not reach take their sideways shift part-way between the two
 * updates' tables (shortest way round the 1024-pixel plane; not past a
 * difference of 64 pixels, which snaps). */
int rr_between_frame(RrBetween *b, uint64_t frame, const RrRoadTracker *road,
                     const RrObjectTracker *objects, const RrVdp *v, const uint8_t *rom,
                     size_t rom_size, int scale);
```

- [ ] **Step 1: Write the failing tests** (clock tests need no pictures: fill `update[].good/stamp` and `length[]` directly):

```c
static void test_tracker_keeps_the_ram_each_set_was_noted_from(void);   /* in test_objects.c: note image 1, note image 2, choose with video memory holding set 1: chosen_ram equals image 1 byte for byte */
static void test_predict_at_the_stamp_is_the_newest_update(void);   /* stamps 10, 15; frame 15: pair (0,1), t == 0x10000 */
static void test_predict_carries_on_evenly(void);                    /* frames 16, 17, 18: t rises by 0x10000 / 5 each frame */
static void test_blend_runs_one_mean_update_behind(void);
/*   stamps 10, 15, 20, lengths all 5; frame 22: frame - delay = 17 -> pair (10,15), t = 0x10000 * 2 / 5 */
static void test_blend_guesses_when_an_update_runs_long(void);       /* same, frame 27: pair (15,20), t = 0x10000 * 7 / 5 */
static void test_late_blend_uses_the_longest_length(void);           /* lengths 4,5,8: delay 8 */
static void test_clock_holds_when_no_update_arrives(void);           /* predict, frame = newest stamp + 100: t == 0x20000 */
static void test_clock_never_steps_back(void);
/*   stamps arriving at 10, 15, 19, 26 while frames run 15..40 in each mode: the position
     (stamp of older + t * gap) never decreases from one frame to the next */
static void test_pair_never_spans_a_broken_update(void);             /* good, bad, good: rr_between_clock returns 0 until two good in a row */
static void test_note_adds_an_update_once(void);                     /* same chosen road and set on three frames: count rises by 1; stamp is the first frame; length recorded when the next arrives */
static void test_set_mode_forgets_updates(void);
static void test_no_motion_gives_no_motion(void);
/*   two identical updates: rr_between_frame at any t in predict gives shifts and table equal to those at t = 0 */
static void test_frame_at_an_update_is_that_updates_scene(void);
/*   synthetic race image pair through the trackers: at frame == stamp in predict, b->shifts equals the
     road tracker's shifts and b->table's object entries equal the chosen set's list (links aside) */
static void test_sky_shift_goes_the_short_way_round(void);           /* table values 1020 -> 4 on a sky line: halfway is 0 (mod 1024), not 512 */
static void test_frame_falls_back_and_counts_why(void);              /* no pair; road jump; objects not ok; table over 80 */
```

- [ ] **Step 2:** `make -C port test` → FAIL to compile.
- [ ] **Step 3:** Implement. `rr_between_frame`: clock → `rr_road_inputs_between` (pair's inputs, newer's `road_ram`) → `rr_road_project` → `rr_road_shifts` on the whole points → `rr_road_shifts_fine`; `rr_between_objects` (pair's `object_ram`) → `rr_between_sprite_table` against the object tracker's chosen set; sky lines. The road tracker's `whole` is copied to `b->whole` so the per-line check against video memory is the same as today's.
- [ ] **Step 4:** `make -C port test` → pass. `uv run pytest -q` → pass.
- [ ] **Step 5:** Commit: `git commit -am "Update history, clock and the in-between scene"`.

---

### Task 6: The replay tool: draw and measure on the recorded race

**Files:**
- Modify: `port/tools/rr_race_replay.c`
- Test: `tests/port/test_race_replay.py`
- Modify: `analysis/race-picture.md` (new section 8), `README.md` (one line on the options)

**Interfaces:**
- Consumes: `RrBetween` and its functions; the tool's existing `--scaled-check S`, `--fine-objects`, `--scaled-at`, `--cpu-speed`.
- Produces: options `--inbetween predict|blend|late` (needs `--scaled-check S`, `--crisp-points fine`, `--fine-objects`) and `--inbetween-check` (needs `--inbetween`). With `--inbetween`, each frame is drawn from `rr_between_frame`'s scene when it returns 1. Output lines:

```
INBETWEEN mode=<m> frames=<race frames> between=<n> guessed=<n> no_pair=<n> road=<n> objects=<n> table=<n> stale_lines=<n> ms_per_frame=<x.x>
INBETWEEN_ENDS updates=<n> road_at_0=<n> road_at_1=<n> objects_at_0=<n> objects_at_1=<n> first_difference=<update or none>
INBETWEEN_SKIP_ONE triples=<n> edge_median=<px> edge_p90=<px> edge_p99=<px> object_median=<px> object_p90=<px> object_p99=<px>
INBETWEEN_MOTION edge_still_frames=<n> edge_moving_frames=<n> edge_largest_step=<px> object_still=<n> object_moving=<n>
INBETWEEN_GUESS updates=<n> edge_median=<px> edge_p90=<px> edge_p99=<px> object_median=<px> object_p90=<px> object_p99=<px>
```

Definitions (all in screen pixels of 320, fractions allowed):
- `INBETWEEN_ENDS`: for each pair of neighbouring good updates, the scene at t = 0 has the same `shifts` (every row of every road line) as the scene the current renderer uses for the older update, and the same object entries and pictures (list bytes apart from links, every `RrFinePicture` field); likewise t = 0x10000 and the newer.
- `INBETWEEN_SKIP_ONE`: for good updates k−1, k, k+1, the scene from (k−1, k+1) at t = (stamp k − stamp k−1) / (stamp k+1 − stamp k−1), compared with update k: left road edge on lines 120, 150, 180, 210 (row 0), and x of every object matched in all three.
- `INBETWEEN_MOTION`: over drawn in-between frames, the left edge on line 180 and the x of the nearest matched roadside object: a frame is "still" if it equals the frame before to within 1/16 pixel, counted only between updates that differ.
- `INBETWEEN_GUESS`: when an update arrives, how far the scene drawn for the frame before was from it (same samples as skip-one).

- [ ] **Step 1: Write the failing tests** in `tests/port/test_race_replay.py` (the file's synthetic-ROM harness):

```python
def test_inbetween_needs_fine_road_and_objects(...):   # --inbetween blend without --fine-objects: exit 2, message names what is missing
def test_inbetween_rejects_unknown_mode(...):           # "--inbetween sideways": exit 2, "predict, blend or late"
def test_inbetween_check_needs_inbetween(...):
def test_inbetween_keeps_the_replay_matching(...):      # synthetic race: RESULT match, and an INBETWEEN line with the five reasons present
def test_inbetween_off_draws_todays_frames(...):        # without the option, --scaled-at dumps are byte-identical to those of a run made before this task (hash recorded in the test from the pre-change binary)
```

- [ ] **Step 2:** `uv run pytest tests/port/test_race_replay.py -q` → the new tests FAIL.
- [ ] **Step 3:** Implement the options and the five measurements.
- [ ] **Step 4:** `uv run pytest -q` → pass; `make -C port test` → pass.
- [ ] **Step 5:** Sanitizer run: build the tool with `-fsanitize=address,undefined` (the `build-asan` directory the objects test uses) and run the recorded race once with `--inbetween predict --inbetween-check`. Expected: no sanitizer report, `RESULT match`.
- [ ] **Step 6:** Measure on `full-throttle`, scale 3, for each mode at 100%, and for predict and blend at 200% (`--cpu-speed 200 --speed-from-handoff 0 --inputs-by-frame`, as in `analysis/smoothness-experiment.md`). Expected: `RESULT match` (100% runs); `between` close to the 11,8xx fine-object frames; `INBETWEEN_ENDS` equal counts or every exception explained by class; `ms_per_frame` under 16.6. Anything else is a finding to debug (superpowers:systematic-debugging) before writing it up.
- [ ] **Step 7:** Dump a run of consecutive frames with `--scaled-at` in blend around frames 4000–4012 and 8278–8290, make a contact sheet under `generated/port/draw/`, and look at it: road, roadside objects and riders move a little every frame; nothing doubles or tears.
- [ ] **Step 8:** Write `analysis/race-picture.md` section 8, "In-between pictures (verified as stated below)": what it is, the five measurements as tables for each mode and speed, Task 4's findings on the unfollowed records, what was looked at by eye, and "Not done, and limits" (centre-line dashes snap; up/down shifts of sky and hills snap; poses snap; crash and police behaviour **unverified**, no recording; two-player falls back; one recorded race). Add the summary line at the top of the file and the option line in `README.md`.
- [ ] **Step 9:** Commit: `git add port tests analysis README.md && git commit -m "In-between pictures in the replay tool, measured on the race"`.

---

### Task 7: The console and the Godot window

**Files:**
- Modify: `port/host/rr_console.h`, `port/host/rr_console.c`, `port/host/rr_godot.c`, `godot/scripts/console.gd`
- Test: `port/tests/test_console.c`, `godot/tests/test_console.gd`, `godot/tests/test_console_scene.gd`
- Modify: `godot/README.md`, `README.md`, `docs/superpowers/specs/2026-11-11-draw-data-contract.md` (a "State after in-between pictures" paragraph)

**Interfaces:**
- Consumes: `RrBetween`; `choose_road` in `rr_console.c`.
- Produces:

```c
void rr_console_set_between(RrConsole *c, int mode);   /* RR_BETWEEN_OFF .. RR_BETWEEN_LATE; other values are ignored */
int rr_console_between(const RrConsole *c);
uint64_t rr_console_between_frames(const RrConsole *c);  /* frames drawn in-between since power on */
```

Godot methods `set_between(mode: int)`, `get_between() -> int`. Key **I** cycles off → blend → predict → late blend → off, with the messages "In-between pictures: off", "In-between pictures: blend (about one update late)", "In-between pictures: predict (nothing late, may guess)", "In-between pictures: late blend (never guesses)". The mode acts only while the crisp road and fine objects are on; the message then adds " — needs R and O on" when they are not. Default when the window opens: blend.

In `choose_road`, after the two trackers choose: `rr_between_note`, then if the mode is not off and `rr_between_frame` returns 1, the frame's shifts, `have`, and sprites come from the `RrBetween`; the expectations (`hires_expect_a/b`) stay the road tracker's `whole`. `rr_console_set_between`, power on, and turning the crisp road or fine objects off or on all call `rr_between_set_mode` / `rr_between_reset`, so no pair survives a change. A change of CPU speed needs nothing: stamps are video frames.

- [ ] **Step 1: Write the failing tests.** In `test_console.c` (its synthetic race ROM already draws an object):

```c
static void test_between_is_off_by_default_in_the_library(void);       /* rr_console_between() == RR_BETWEEN_OFF after open; frames byte-equal to a second console never given the call */
static void test_between_draws_in_between_frames(void);                /* mode predict, run until two updates: rr_console_between_frames() > 0 */
static void test_between_off_frames_are_todays(void);                  /* same run with mode off: every frame equals the crisp + fine objects frame */
static void test_switching_modes_mid_race_never_uses_stale_updates(void);
/*   predict for N frames, off for 3, blend: the first blend frames are today's picture until two new
     updates have been noted (between_frames does not rise for them); same for toggling fine objects */
static void test_between_ignores_unknown_modes(void);
```

In `godot/tests/test_console.gd`: `set_between(2)` then `get_between() == 2`; out-of-range ignored. In `test_console_scene.gd`: pressing I four times walks the four messages and returns to off; the scene starts in blend.

- [ ] **Step 2:** `make -C port test` → FAIL to compile.
- [ ] **Step 3:** Implement the console part; `make -C port test` → pass.
- [ ] **Step 4:** Implement the Godot binding and key. **Before rebuilding `godot/bin/librr2.dylib`, tell the owner and check no Godot window is running** (`pgrep -fl Godot`); then `scripts/build-godot-extension.sh` and the Godot tests (`uv run pytest tests/port/test_godot_extension.py -q` and the headless scene tests as the existing ones are run).
- [ ] **Step 5:** `uv run pytest -q` → all pass.
- [ ] **Step 6:** Update `godot/README.md` (key table), `README.md`, and the contract's state paragraph.
- [ ] **Step 7:** Commit: `git add port godot docs README.md && git commit -m "In-between pictures in the console and the Godot window"`.

---

## After the last task

One whole-branch review by a fresh reviewer on the most capable model (executing-plans), one fix pass, then the final message with "Rulings I made" and "Deferred minors", and the finishing menu. The owner then plays: R, O on, I to compare blend, predict and late blend, at 100% and 200%.
