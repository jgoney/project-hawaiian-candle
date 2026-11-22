/* The centre-line dashes.
 *
 * The game does not draw the centre line from a picture. For every screen
 * line of the road it keeps a start x and a width (0xFF2EDE, 0xFF309E), and
 * later packs the lines that have a width into small sprites (0x13F20).
 * Measured against the game's memory on the recorded race (analysis/
 * race-picture.md, section 9):
 *
 *   - 0x14822, for each road line: width = (3 * (right - left) / 2) >> 6, at
 *     least 1, as 16-bit words (half = (right - left) >> 1 arithmetic, the
 *     sum shifted right logically); start = the road's centre on that line
 *     (the per-point centres run through 0x153EA like the edges). A start of
 *     0x140 or more is no dash (start 0, width 0); a negative start is cut
 *     off: added to the width, which is not let below 0, and set to 0.
 *   - 0x12768 then walks the bands between neighbouring road points from the
 *     bottom of the view up and sets the width to 0 on the lines of every
 *     band whose track segment (the camera's segment number plus the band's
 *     number) is odd: one segment painted, one not.
 *   - Near the finish line (the word at 0xFF0698 above 0) 0x12768 does
 *     something else, which is not modelled: such an update is not covered.
 *
 * The first two are repeated here on the game's whole-pixel road, to be
 * compared with its memory, and on the fine road, to draw the dashes as
 * strips of the enlarged picture.
 */
#ifndef RR_DASHES_H
#define RR_DASHES_H

#include <stddef.h>
#include <stdint.h>

#include "rr_objects.h"
#include "rr_road.h"
#include "rr_vdp.h"

typedef struct {
    int ok;
    int16_t start[RR_SCREEN_H], width[RR_SCREEN_H]; /* per screen line; width 0: no dash */
} RrDashLines;

enum { RR_DASHES_PC = 0x13F20 }; /* the game is about to pack the dashes into sprites */

/* What the game holds for the dashes at RR_DASHES_PC, made from the road
 * points as it has them (whole pixels) and the camera's place (0xFF0A0A;
 * only the segment number is used). `stop_line` is the line at which the
 * game's walk of the bands ends (the word the pointer at 0xFF27BA points
 * to); pass a negative number for none. ok = 0 if the road is unusable or
 * the ROM too short to hold the game's table of reciprocals. */
void rr_dashes_lines(const RrRoad *road, uint32_t place, int stop_line, const uint8_t *rom,
                     size_t rom_size, RrDashLines *out);

/* The two arrays as they stand in work RAM. ok = 0 near the finish line
 * (see above) or outside a one-player race. */
void rr_dashes_read(const uint8_t *ram, RrDashLines *out);
/* The stop line for rr_dashes_lines, from work RAM. */
int rr_dashes_stop_line(const uint8_t *ram);

/* The dashes of a fine road as strips of the picture enlarged `scale` times:
 * on each row of each road line of a painted segment, from the road's centre
 * to the centre plus the width (3/128 of the road's width on that row, at
 * least one screen pixel), both taken from the fine points on that row, and
 * cut at the screen's sides. What hides a row is the road's own
 * (rr_road_rows_fine); which band a row is follows the game's walk from the
 * bottom of the view. `have` (RR_SCREEN_H long, or NULL for every line) says
 * which screen lines are road lines as the game has them: a line that is
 * not gets no strip, so that nothing is drawn above the road's top line. A
 * road that is not usable gives no strips. */
void rr_dashes_strips(const RrFineRoad *road, uint32_t place, const uint8_t *have, int scale,
                      RrStrips *out);
/* The same for a picture `margin` screen pixels wider on either side: the
 * strips are cut at the wide picture's sides instead. from and to are still
 * counted from the left edge of the screen itself, so they may be negative
 * or beyond RR_SCREEN_W * scale; between those two the strips are the
 * narrow ones. A margin of 0 is rr_dashes_strips. */
void rr_dashes_strips_wide(const RrFineRoad *road, uint32_t place, const uint8_t *have, int scale, int margin,
                           RrStrips *out);

/* Which entries of a sprite table (640 bytes) are the game's dashes: those
 * from entry `first` on with colour set 3 and low priority (what 0x13F20
 * gives them; measured: every sprite the game adds after its objects).
 * skip[i] is set to 1 for them and 0 for every other entry. */
void rr_dashes_mark(const uint8_t *table, int first, uint8_t *skip);

/* Call when the main loop reaches RR_DASHES_PC. If the road points in work
 * RAM are the newest road the tracker noted, that road's dashes are marked
 * as proven when the description made from the points equals the game's
 * arrays on every line of the 3D view, and as not proven otherwise. */
void rr_dashes_note(RrRoadTracker *t, const uint8_t *ram, const uint8_t *rom, size_t rom_size);
/* 1 if the road last chosen has fine points and proven dashes. */
int rr_dashes_proven(const RrRoadTracker *t);

/* For a frame drawn from the update on screen (no in-between scene): if the
 * road chosen has proven dashes and fine points, and the objects on screen
 * are known (`set`, with `fine` the sprites it is drawn with), fill `strips`
 * with the road's dashes at `scale` and `out` with a copy of `fine` that
 * draws them and leaves the game's dash sprites out, and return 1. Otherwise
 * return 0: the frame keeps the game's dash sprites. */
int rr_dashes_frame(const RrRoadTracker *road, const RrObjectSet *set, const RrFineSprites *fine,
                    const RrVdp *v, int scale, RrStrips *strips, RrFineSprites *out);
/* The same with strips for a wider picture (rr_dashes_strips_wide). */
int rr_dashes_frame_wide(const RrRoadTracker *road, const RrObjectSet *set, const RrFineSprites *fine,
                    const RrVdp *v, int scale, int margin, RrStrips *strips, RrFineSprites *out);

#endif
