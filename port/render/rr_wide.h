/* The wide picture: what lies beyond the game's 320 pixels.
 *
 * The game's two scrolling layers are 1,024 pixels wide and it shows 320 of
 * them. On a line of ground it does not fill the layer: it keeps one block
 * of ground at plane columns 352 to 671, with the road's edge line as a
 * short mark just outside either end of it, and slides that block sideways
 * (analysis/widescreen-study.md). Everything further out is empty, so a
 * wider picture has to continue the ground itself; a line of sky is a
 * panorama that wraps and can simply be shown further.
 */
#ifndef RR_WIDE_H
#define RR_WIDE_H

#include "rr_road.h"
#include "rr_vdp.h"

/* The wide picture is RR_WIDE_MARGIN pixels wider on either side (398 in
 * all) on the first RR_WIDE_LINES lines, the view above the dashboard. */
enum { RR_WIDE_MARGIN = 39, RR_WIDE_LINES = 152 };

/* The road's surface is the backdrop: register 7 holds this colour on the
 * lines from the horizon down, and the sky's (63) above it
 * (analysis/race-picture.md, section 3). */
enum { RR_WIDE_ROAD_BACKDROP = 62 };

/* The dashboard's two mirrors stand on lines 179 to 219 and are cut by the
 * screen's sides. Their inner frames are two pixels and a black line, at
 * screen columns 64 to 66 and 255 to 253; the dashboard's blue is colour 50
 * while it is drawn. Sets w->dash so that the mirrors get an outer frame
 * and the dashboard its blue beyond them. */
void rr_wide_dashboard(RrWide *w);

/* The game's ground blocks: [0] plane A's, [1] plane B's. */
void rr_wide_blocks(RrWideBlock block[2]);

/* Is the row of plane `which` (0 A, 1 B) shown on `line` a ground row of
 * `b`: opaque right across [first, end) and blank outside
 * [first - mark, end + mark)? The row alone does not make a line ground:
 * on the Tennessee recording some rows of the sky's picture pass too, so a
 * caller also needs the line to be drawn with RR_WIDE_ROAD_BACKDROP. */
int rr_wide_is_ground_row(const RrVdp *v, int which, int line, const RrWideBlock *b);

/* --- Checks of a wide picture -------------------------------------------
 * For tools: they are how a wide picture is known to be right, and each is
 * tested against pictures that are wrong (tests/test_wide.c). */

/* The middle of the wide picture against the picture without margins. */
typedef struct {
    uint64_t pixels;    /* compared */
    uint64_t differing; /* = road_line + object + dash + other */
    uint64_t object;    /* under an object's picture */
    uint64_t dash;      /* on a centre-line strip */
    uint64_t road_line; /* on a line the road reaches (and neither of the above) */
    uint64_t other;
} RrWideCentre;
/* `wide_px` holds RR_SCREEN_H * scale rows of (RR_SCREEN_W + 2 * margin) *
 * scale pixels, `narrow_px` as many of RR_SCREEN_W * scale. Every row is
 * compared (the rows from line `lines` down lie between panels and must be
 * the narrow rows too). `have` (RR_SCREEN_H) says which lines the road
 * reaches; `object_cover`, if not NULL, is one byte for every pixel of the
 * narrow picture, non-zero under an object; `strips` may be NULL. Counts are
 * added to `out`. */
void rr_wide_centre(const uint32_t *wide_px, const uint32_t *narrow_px, int scale, int margin, int lines,
                    const uint8_t *have, const uint8_t *object_cover, const RrStrips *strips, RrWideCentre *out);

/* The margins of a ground line against the road. */
typedef struct {
    uint64_t examined;          /* margin pixels looked at (an object's are passed over) */
    uint64_t grass, mark, road; /* drawn as what belongs there */
    uint64_t wrong;             /* drawn as something that does not */
    uint64_t short_lines;       /* for the caller: lines drawn without the road's shifts */
    uint64_t beyond_lines;      /* lines on which an edge lies beyond one of the game's limits */
} RrWideGroundCount;
/* What a margin pixel of a ground line may be is worked out from the road
 * alone, not from the planes: `road` holds the row's shifts and how far the
 * edges lie beyond the game's limits, so the left edge is at
 * a + 0x2A0 + beyond_a and the right at b - 0x2A0 + beyond_b. Then:
 *   - plane A's grass belongs only left of the left edge, plane B's only
 *     right of the right edge;
 *   - a plane's edge-line mark only within the block's `mark` pixels on the
 *     road side of its edge;
 *   - road (the backdrop) only from the left edge up to the right one.
 * Anything else, a panel included, is wrong. What was drawn is read from
 * `sources`, the line's `scale` rows of (RR_SCREEN_W + 2 * margin) * scale
 * sources: grass is a pixel of a plane whose tile is one of the plane row's
 * block cells, a mark one whose tile is one of the cells beside the block.
 * Nothing is examined if `road` is NULL, has another scale, or does not
 * reach `line`. Counts are added to `out`. */
void rr_wide_ground(const RrPixelSource *sources, const RrRoadShifts *road, const RrVdp *v, int line,
                    int scale, int margin, RrWideGroundCount *out);

/* The margins of a line that is not ground: what kind of pixel each is. */
typedef struct { uint64_t examined, panel, plane, backdrop; } RrWideSky;
void rr_wide_sky(const RrPixelSource *sources, int scale, int margin, RrWideSky *out);

#endif
