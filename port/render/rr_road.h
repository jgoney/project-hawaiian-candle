/* The road for a finer picture: the game's own projected road points, turned
 * into a sideways shift of each grass plane for every row of an enlarged
 * picture.
 *
 * The game projects 32 points of the road ahead (screen line, left and right
 * edge), draws straight runs between neighbouring points, one position per
 * screen line, and shifts plane A (left verge) and plane B (right verge) on
 * each line so their edges land there (analysis/race-picture.md, sections 3.1
 * and 4). Here the same straight runs are evaluated `scale` times per line,
 * so the edge moves a fraction of a pixel per row instead of a whole pixel
 * every few lines. On row 0 of every line the whole part of the shift is
 * exactly the game's value (the game drops the fraction): that is what lets
 * the result be checked against the shift table in video memory.
 *
 * The arithmetic follows src/rr2port/scene.py, which is checked against the
 * game's own tables.
 */
#ifndef RR_ROAD_H
#define RR_ROAD_H

#include <stddef.h>
#include <stdint.h>

#include "rr_vdp.h"

enum { RR_ROAD_POINTS = 32, RR_ROAD_RECENT = 4 };

typedef struct {
    int count;
    int16_t line[RR_ROAD_POINTS], clipped_line[RR_ROAD_POINTS];
    int16_t left[RR_ROAD_POINTS], right[RR_ROAD_POINTS];
    int16_t centre[RR_ROAD_POINTS]; /* the road's centre at each point: a screen x */
    int16_t centre_x, view_bottom, horizon_line;
} RrRoad;

/* Read the road points from work RAM (65,536 bytes). Returns 0, leaving `out`
 * alone, if the RAM is not a one-player race. */
int rr_road_read(const uint8_t *ram, RrRoad *out);

/* What the game projects the road from, as it stands when the projection starts. */
typedef struct {
    int count;                                /* road points, at most RR_ROAD_POINTS */
    uint32_t place;                           /* the camera's place on the track (0xFF0A0A): segment
                                                 number in the high word, `fraction` in the low */
    uint16_t fraction;                        /* the camera's place within its track segment */
    int16_t sideways, height;                 /* the player's sideways position, the camera's height */
    int16_t top, bottom, reference, centre_x; /* view lines, horizon reference line, screen centre */
    int8_t curve[RR_ROAD_POINTS + 1], hill[RR_ROAD_POINTS + 1]; /* the track from the camera on */
} RrRoadInputs;

/* Read them from work RAM. Returns 0, leaving `out` alone, if the RAM is not a
 * one-player race. */
int rr_road_read_inputs(const uint8_t *ram, RrRoadInputs *out);

/* What the game would project from with its camera part-way from a's to b's.
 * t is 16.16: 0 is a, 0x10000 is b, above is past b (the motion carried on).
 * The place, sideways position and height, and the view lines, reference
 * line and screen centre, are taken part-way and rounded to the nearest
 * whole; the track from the camera on is read from `ring_b`, the track ring
 * (RR_ROAD_RING bytes from 0xFFD828) as it was with b, at the in-between
 * segment. Returns 0, leaving `out` alone, if a and b differ in `count`, or
 * the place moves by more than RR_ROAD_JUMP from a to b, forwards or back:
 * that is a jump, not motion. */
enum { RR_ROAD_RING = 0x200, RR_ROAD_JUMP = 4 << 16 };
int rr_road_inputs_between(const RrRoadInputs *a, const RrRoadInputs *b, const uint8_t *ring_b,
                           int32_t t, RrRoadInputs *out);

/* The road points with fractions of a pixel, 16.16 fixed point. */
typedef struct {
    int count;
    int32_t line[RR_ROAD_POINTS], clipped_line[RR_ROAD_POINTS];
    int32_t left[RR_ROAD_POINTS], right[RR_ROAD_POINTS]; /* relative to the screen centre */
    int32_t centre[RR_ROAD_POINTS];                      /* the road's centre, likewise */
    int16_t centre_x, view_bottom;
} RrFineRoad;

/* The game's projection (0x14B44). The game places a point by multiplying a
 * world position by a scale and dropping the remainder; `fine` keeps the
 * remainder, `whole` drops it exactly as the game does, so `whole` is what the
 * game's own arrays hold after projecting from these inputs (which is how a
 * set of inputs is known to belong to a road). `rom` is the game ROM (the
 * perspective table at 0x27C84 is used); returns 0 if it is too short. */
int rr_road_project(const RrRoadInputs *in, const uint8_t *rom, size_t rom_size, RrFineRoad *fine,
                    RrRoad *whole);

typedef struct {
    int scale;
    uint8_t have[RR_SCREEN_H];                    /* 1 where the road reaches this line */
    int32_t a[RR_SCREEN_H * RR_SCALE_MAX];        /* plane A: row r of line l at [l * scale + r] */
    int32_t b[RR_SCREEN_H * RR_SCALE_MAX];        /* plane B, the same; both 16.16 fixed point */
    /* The game keeps each shift within limits, so an edge far off the screen
     * stops moving. How far the row's edge really lies beyond the limit: the
     * shift it would have without the limit, less the one it has. 16.16,
     * indexed like a and b, at most 256 pixels either way, 0 where the limit
     * does not bite and on lines the road does not reach. */
    int32_t beyond_a[RR_SCREEN_H * RR_SCALE_MAX], beyond_b[RR_SCREEN_H * RR_SCALE_MAX];
} RrRoadShifts;

/* The shifts for every row of every line the road reaches. `rom` is the game
 * ROM (the table of reciprocals at 0x15472 is used); if it is too short to
 * hold the table no line gets a shift. */
void rr_road_shifts(const RrRoad *road, const uint8_t *rom, size_t rom_size, int scale,
                    RrRoadShifts *out);

/* The shifts from fine road points. `whole` are the shifts of the same road's
 * whole-pixel points (rr_road_shifts): its scale and its lines are used, and
 * `out` starts as a copy of it. Each row that lies between two fine points
 * then takes its edge from the straight run between them, so points that
 * land between pixels give a run that does not bend where the whole-pixel
 * road does. */
void rr_road_shifts_fine(const RrFineRoad *road, const RrRoadShifts *whole, RrRoadShifts *out);

/* One value for every screen line between neighbouring points, as the game's
 * 0x153EA makes its per-line arrays from a per-point array `xs` (the road's
 * left, right or centre): 16.16, of which the game keeps the whole part.
 * `out` and `have` are RR_SCREEN_H long; have[line] is 1 where a value was
 * made. Nothing is made if the ROM is too short for the table of reciprocals. */
void rr_road_lines(const RrRoad *road, const int16_t *xs, const uint8_t *rom, size_t rom_size,
                   int32_t *out, uint8_t *have);

/* The same for fine points, on every row of a picture enlarged `scale` times:
 * `out`, `covered` and `band` are RR_SCREEN_H * scale long. covered[row] is 1
 * where the row lies between two fine points, and band[row] (if not NULL) is
 * then the number of the nearer of the two (0 is the band nearest the
 * camera). */
void rr_road_rows_fine(const RrFineRoad *road, const int32_t *xs, int scale, int32_t *out,
                       uint8_t *covered, uint8_t *band);

/* 1 if, on every line that has a shift, row 0's whole part is the value in
 * the video chip's horizontal scroll table: the road on screen is this road. */
int rr_road_matches_video(const RrRoadShifts *shifts, const RrVdp *v);

/* Which road is on screen. The picture shows a road the game projected a pass
 * or two ago, so the road points are noted each time the game starts turning
 * them into shifts, and the newest noted set whose whole-pixel shifts are the
 * ones in video memory is used. If none is, there are no shifts for that
 * frame and the picture is the plain enlargement: never a road that is not on
 * screen.
 *
 * Fine points: what the game projects from is noted each time it projects.
 * When road points are noted, the newest noted inputs whose projection, with
 * the remainders dropped, gives exactly those points are attached to them:
 * they are that road's inputs. A road with inputs is drawn from its fine
 * points (in RR_ROAD_FINE mode, the default); one without is drawn from its
 * whole-pixel points as before. */
enum { RR_ROAD_FINE, RR_ROAD_WHOLE };

typedef struct {
    RrRoad recent[RR_ROAD_RECENT];
    RrFineRoad recent_fine[RR_ROAD_RECENT];
    uint8_t has_fine[RR_ROAD_RECENT];
    int count, newest;
    int chosen;          /* index in `recent` of the road last chosen */
    RrRoadInputs inputs[RR_ROAD_RECENT];
    uint8_t inputs_ring[RR_ROAD_RECENT][RR_ROAD_RING]; /* the track ring as it was with each */
    int inputs_count, inputs_newest;
    RrRoadInputs recent_inputs[RR_ROAD_RECENT];        /* each recent road's inputs, where has_fine */
    uint8_t recent_ring[RR_ROAD_RECENT][RR_ROAD_RING];
    int have_chosen;     /* the last choice found a road on screen */
    uint8_t dashes_ok[RR_ROAD_RECENT]; /* each recent road's centre-line dashes are what the
                                          port's description of them says (rr_dashes.h) */
    int mode;            /* RR_ROAD_FINE or RR_ROAD_WHOLE */
    RrRoadShifts whole;  /* the chosen road's whole-pixel shifts: what video memory holds */
    RrRoadShifts shifts; /* what to draw with: the fine shifts, or a copy of `whole` */
    uint64_t race_frames, crisp_frames, fine_frames, whole_frames; /* crisp = fine + whole */
} RrRoadTracker;

enum {
    RR_ROAD_INPUTS_PC = 0x14A86, /* the game is about to project the road */
    RR_ROAD_NOTE_PC = 0x14566    /* the game starts turning road points into per-line edges */
};

void rr_road_tracker_reset(RrRoadTracker *t); /* the mode is kept */
/* Call when the main loop reaches RR_ROAD_INPUTS_PC. */
void rr_road_tracker_note_inputs(RrRoadTracker *t, const uint8_t *ram);
/* Call when the main loop reaches RR_ROAD_NOTE_PC. */
void rr_road_tracker_note(RrRoadTracker *t, const uint8_t *ram, const uint8_t *rom, size_t rom_size);
/* Call before a frame's first line is drawn. Returns the shifts to draw it
 * with (t->shifts; t->whole then holds what each line's entry in video memory
 * must be), or NULL. */
const RrRoadShifts *rr_road_tracker_choose(RrRoadTracker *t, const uint8_t *ram, const uint8_t *rom,
                                           size_t rom_size, const RrVdp *v, int scale);
/* The inputs of the road last chosen and the track ring as it was with them,
 * or NULL if no road was chosen for the last frame or it has no inputs. */
const RrRoadInputs *rr_road_tracker_chosen_inputs(const RrRoadTracker *t, const uint8_t **ring);

#endif
