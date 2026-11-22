/* The race scene between two of the game's updates.
 *
 * The game redraws the race about 11 times a second and the picture stands
 * still in between (analysis/race-picture.md, 3.4). This describes the scene
 * part-way from one update to the next, or a little past the newest, so a
 * picture can be drawn on every video frame. Nothing here writes to the
 * game: it reads work RAM images and video memory.
 *
 * Design: docs/superpowers/specs/2026-11-12-interpolation-design.md.
 */
#ifndef RR_BETWEEN_H
#define RR_BETWEEN_H

#include <stddef.h>
#include <stdint.h>

#include "rr_objects.h"
#include "rr_road.h"
#include "rr_vdp.h"

enum { RR_BETWEEN_RECORDS = 96 }; /* draw records looked at; more makes the image "not covered" */

/* One draw record (22 bytes at 0xFFCEB8 + 22 * slot) with what identifies it. */
typedef struct {
    int16_t x, foot, hidden, offset;
    uint16_t size, picture, flags, roadside, second, segment;
    uint64_t key; /* 0: cannot be followed from one update to the next */
} RrBetweenRecord;

typedef struct {
    int ok, count;
    RrBetweenRecord record[RR_BETWEEN_RECORDS];
} RrBetweenRecords;

/* Read and identify the records of a work RAM image taken at
 * RR_OBJECTS_NOTE_PC. A vehicle record (its long in the table at 0xFF06D6
 * equals a listed vehicle's place, +4, less the camera's place, 0xFF0A0A) is
 * keyed by the vehicle's address; a roadside record (distance or roadside
 * byte not zero) by its segment, side, picture and roadside byte. A key that
 * occurs twice in the image is 0 in both. ok = 0 if the count (0xFFDC42) is
 * above RR_BETWEEN_RECORDS or the vehicle list (0xFF04F8, longs, ended by a
 * word 0xFFFF) has no end within 32 entries. */
void rr_between_read(const uint8_t *ram, RrBetweenRecords *out);

/* The objects at t (16.16) between the images a and b (both taken at
 * RR_OBJECTS_NOTE_PC). The base image is a below 0x10000 and b from there
 * on. Each base record whose key is also in the other image is taken
 * part-way from its a values to its b values (past 0x10000 the motion is
 * carried on): x, foot, hidden line and offset in a straight line, size
 * through 1 / size, which is how distance moves it. The whole parts go into
 * a copy of the base image (`scratch`, 0x10000 bytes), which rr_objects_draw
 * draws (`work`, 0x10000 bytes, is its); the fractions of x and foot become
 * the pictures' fx and fy. From 0x8000 up to 0x10000 the picture, second
 * picture and mirror bit are b's. A record that moved more than
 * RR_BETWEEN_JUMP_X pixels or RR_BETWEEN_JUMP_FOOT lines, or to more than
 * twice or less than half its size, is not taken part-way, and neither are
 * records without a match: they are as the base has them, with one
 * exception. An object of a that b no longer has (it has left the view) but
 * that the image before a has too (`ram_before`, or NULL for none) goes on
 * as it was going from that image to a; `carry` (16.16) is how long the
 * update from a to b lasted as a share of the one from `ram_before` to a.
 * Without this the nearest objects would stand still for their last update.
 * Returns out->ok, which is 0 also when a sprite entry of the result has no
 * full picture. */
enum { RR_BETWEEN_JUMP_X = 64, RR_BETWEEN_JUMP_FOOT = 32 };
int rr_between_objects(const uint8_t *ram_before, int32_t carry, const uint8_t *ram_a,
                       const uint8_t *ram_b, const uint8_t *rom, size_t rom_size, int32_t t,
                       RrObjectSet *out, uint8_t *scratch, uint8_t *work);
/* The same, also giving where each record slot stands between pixels: fx
 * and fy (RR_BETWEEN_RECORDS each, 65,536ths of a pixel; 0 for a record not
 * taken part-way), for the pictures of records that `out` does not show. */
int rr_between_objects_fine(const uint8_t *ram_before, int32_t carry, const uint8_t *ram_a,
                            const uint8_t *ram_b, const uint8_t *rom, size_t rom_size, int32_t t,
                            RrObjectSet *out, uint8_t *scratch, uint8_t *work, uint16_t *fx, uint16_t *fy);

/* The sprite table to draw with: `set`'s entries (0 up to set->sprites),
 * then every entry of video memory's sprite chain from on_screen->sprites on
 * (the centre-line dashes and whatever else the game added after its
 * objects), in chain order, each linked to the next. `table` is 640 bytes;
 * fine->table is set to it, fine->pictures to set's, and fine->picture and
 * dx are set's for the first part and -1 for the rest. Returns 0 if the two
 * parts do not fit in 80 entries. */
int rr_between_sprite_table(const RrObjectSet *set, const RrObjectSet *on_screen, const RrVdp *v,
                            uint8_t *table, RrFineSprites *fine);

/* ---- which moment to draw, and the scene for it ---- */

enum { RR_BETWEEN_OFF, RR_BETWEEN_PREDICT, RR_BETWEEN_BLEND, RR_BETWEEN_LATE };
enum { RR_BETWEEN_UPDATES = 6, RR_BETWEEN_LENGTHS = 16, RR_BETWEEN_SHIFT_BYTES = RR_SCREEN_H * 4 };
enum { RR_BETWEEN_GAP = 30 }; /* video frames: an update that lasted longer was a stop, not motion */
enum { RR_BETWEEN_NO_PAIR, RR_BETWEEN_ROAD, RR_BETWEEN_OBJECTS, RR_BETWEEN_TABLE, RR_BETWEEN_REASONS };

/* One of the game's updates as it showed on screen. */
typedef struct {
    int good;                 /* 0: it could not be described (no road inputs) */
    int dashes;               /* its centre-line dashes are proven (rr_dashes.h) */
    uint64_t id;              /* the object tracker's age stamp of its set */
    uint64_t stamp;           /* the video frame it first showed in */
    int road_top;             /* the first line its road reached (RR_SCREEN_H: none) */
    RrRoadInputs inputs;
    uint8_t ring[RR_ROAD_RING];
    uint8_t object_ram[0x10000];
    uint8_t shifts[RR_BETWEEN_SHIFT_BYTES]; /* video memory's sideways-shift table when it showed */
} RrBetweenUpdate;

typedef struct {
    int mode;
    RrBetweenUpdate update[RR_BETWEEN_UPDATES]; /* oldest first */
    int count;
    uint8_t length[RR_BETWEEN_LENGTHS];         /* video frames each recent update lasted, newest last */
    int lengths;
    int32_t delay;            /* how far behind the present the picture is drawn, 16.16 frames; -1: not set */
    /* The scene rr_between_frame made. */
    RrRoadShifts shifts;      /* what to draw with: `have` says which lines */
    uint8_t road_have[RR_SCREEN_H]; /* the lines of it that the road reaches */
    RrRoadShifts expect;      /* row 0 of every line: the shift video memory held when the scene was made */
    RrObjectSet set;
    uint8_t table[RR_SPRITES_MAX * 8];
    RrStrips strips;          /* the centre-line dashes, when both updates' are proven */
    RrFineSprites fine;       /* fine.strips is then `strips` and fine.skip marks the game's dash sprites */
    int32_t t;                /* where between `older` and `newer` it is */
    int older, newer;
    uint8_t scratch[0x10000], work[0x10000];
    uint64_t frames, between_frames, guessed_frames; /* asked for; made; of those, past the newest update */
    uint64_t fell_back[RR_BETWEEN_REASONS];
    uint64_t returned;        /* frames on which an earlier update was on screen again */
    /* For a wider picture. `margin` is set by the owner (screen pixels on
     * either side; 0: none, and nothing below is made). The scene then has
     * strips cut at the wide picture's sides, and the objects of the margins
     * (rr_objects_margin of the scene's own objects) in margin_set,
     * margin_table and margin_fine, each picture with its place between
     * pixels. margin_ok is 0 if they could not be made; the scene is made
     * all the same, and the frame is counted in margin_fell_back. */
    int margin;
    RrObjectSet margin_set;
    uint8_t margin_table[RR_SPRITES_MAX * 8];
    RrFineSprites margin_fine;
    int margin_ok;
    uint64_t margin_fell_back;
    uint16_t fx[RR_BETWEEN_RECORDS], fy[RR_BETWEEN_RECORDS]; /* each record slot's place between pixels */
} RrBetween;

void rr_between_forget(RrBetween *b);             /* forgets the updates kept: no pair until two new ones show */
void rr_between_reset(RrBetween *b);              /* forgets the updates and the counts; the mode is kept */
void rr_between_set_mode(RrBetween *b, int mode); /* a mode outside the four is ignored; a change forgets the updates */

/* The delay the mode asks for, 16.16 frames: 0 (predict), the mean of the kept
 * lengths (blend) or the largest of them (late blend). */
int32_t rr_between_target_delay(const RrBetween *b);

/* Call before a frame's first line, after the road and object trackers have
 * chosen for it. If the set the object tracker chose is newer than the
 * newest update kept, it is kept as a new one stamped `frame` (good only if
 * the road chosen has inputs), and the one before gets its length. A set
 * older than the newest kept (video memory showing an earlier update again)
 * is counted and otherwise ignored. If no set or no road was found on screen
 * the updates are forgotten: there is then an update nobody described
 * between what was kept and whatever comes next. So are they if the newest
 * lasted more than RR_BETWEEN_GAP frames (a pause, a menu). The delay
 * moves towards its target by at most a quarter of a frame per call, so the
 * moment drawn never steps back. */
void rr_between_note(RrBetween *b, uint64_t frame, const RrRoadTracker *road,
                     const RrObjectTracker *objects, const RrVdp *v);

/* The pair of updates and the place between them for `frame`: the moment
 * frame - delay on the straight path through the stamps. Past the newest
 * stamp it is the two newest with t above 0x10000, but never above 0x20000
 * (the picture then holds); before the oldest, the two oldest with t = 0.
 * Returns 0 if there are not two updates or either of the pair is not good. */
int rr_between_clock(const RrBetween *b, uint64_t frame, int *older, int *newer, int32_t *t);

/* The scene at t between the kept updates `older` and `newer` (indices into
 * b->update), in b->shifts, b->road_have, b->expect, b->set, b->table and
 * b->fine; `on_screen` is the object set video memory holds. Returns -1 if
 * made, else the reason it could not be (RR_BETWEEN_ROAD, _OBJECTS or
 * _TABLE). Counts nothing. Lines the in-between road does not reach take
 * their sideways shift part-way between the two updates' tables (the short
 * way round the 1,024-pixel plane; a difference above RR_BETWEEN_JUMP_X is
 * not slid).
 *
 * `screen`, if not NULL, is the road tracker with the road on screen chosen.
 * The horizon on screen is the game's and is not moved by this scene, so
 * where the scene's road and the road on screen do not start on the same
 * line the lines between are drawn as the screen has them: a line of hills
 * on screen (from the highest road top of the two updates and the scene
 * down to the last line of the road on screen)
 * with the shift video memory holds now, a line of ground on screen that the
 * scene's road does not reach with the shifts of the road on screen. Such
 * lines are not in b->road_have. b->shifts.beyond_a/b (how far an edge lies
 * beyond the game's limits) go with the shifts: the scene's road's on its
 * own lines, the road on screen's on the lines taken from it, 0 on every
 * other line. With NULL the scene is made as if its own
 * horizon were on screen (for checking a scene against an update that is
 * not the one on screen). */
int rr_between_scene(RrBetween *b, int older, int newer, int32_t t, const RrObjectSet *on_screen,
                     const RrVdp *v, const RrRoadTracker *screen, const uint8_t *rom, size_t rom_size,
                     int scale);

/* Make the scene for `frame` in b->shifts, b->expect and b->fine. Returns 1
 * if it is to be drawn; 0, counting why, if the frame is to be drawn as it
 * would be without this. */
int rr_between_frame(RrBetween *b, uint64_t frame, const RrRoadTracker *road,
                     const RrObjectTracker *objects, const RrVdp *v, const uint8_t *rom,
                     size_t rom_size, int scale);

/* For checking: whether the scene just made shows this road on every row of
 * every road line; whether these objects are that set (every sprite entry,
 * the link aside, and every picture); whether the scene's sprite table is
 * video memory's own chain, entry for entry. */
int rr_between_same_road(const RrBetween *b, const RrRoadShifts *road);
int rr_between_same_objects(const RrObjectSet *made, const RrObjectSet *set);
int rr_between_same_table(const RrBetween *b, const RrVdp *v);

#endif
