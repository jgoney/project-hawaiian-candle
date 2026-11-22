/* Scanline renderer for the video chip model: 40-cell mode, 28 rows.
 *
 * Draws planes A and B, the window plane and sprites with their priorities.
 * 32-cell mode, 30-row mode, shadow/highlight and interlace are not
 * implemented: a line rendered with any of them enabled is solid magenta so it
 * cannot pass a picture check silently.
 */
#include <string.h>

#include "rr_vdp.h"

#define UNSUPPORTED_COLOUR 0xFF00FFu
#define MAX_SPRITES 80
#define MAX_SPRITES_PER_LINE 20
#define MAX_SPRITE_PIXELS_PER_LINE 320

/* A layer pixel: bits 0-5 colour index (palette line and colour), bit 6 priority.
 * Colour 0 of any palette line is transparent. */
#define PIXEL_PRIORITY 0x40
#define PIXEL_OPAQUE(p) (((p) & 0x0F) != 0)

/* The 3-bit to 8-bit levels of MAME's 315-5313, so pictures can be compared
 * exactly. Taken from the distinct channel values in MAME's boot snapshots
 * (generated/port/pictures): 0, 52, 87, 116, 144, 172, 206, 255. */
static uint32_t level(unsigned three_bits) {
    static const uint8_t levels[8] = {0, 52, 87, 116, 144, 172, 206, 255};
    return levels[three_bits & 7];
}

uint32_t rr_vdp_colour(uint16_t cram_value) {
    uint32_t r = level((cram_value >> 1) & 7);
    uint32_t g = level((cram_value >> 5) & 7);
    uint32_t b = level((cram_value >> 9) & 7);
    return r << 16 | g << 8 | b;
}

static uint16_t vram16(const RrVdp *v, uint32_t addr) {
    return (uint16_t)(v->vram[addr & 0xFFFF] << 8 | v->vram[(addr + 1) & 0xFFFF]);
}

/* Colour 0-15 of pixel (x, y) in a tile, after flips. */
static unsigned tile_colour(const RrVdp *v, uint16_t entry, unsigned x, unsigned y) {
    if (entry & 0x0800) x = 7 - x;
    if (entry & 0x1000) y = 7 - y;
    uint8_t byte = v->vram[((entry & 0x07FF) * 32u + y * 4 + x / 2) & 0xFFFF];
    return (x & 1) ? (byte & 0x0F) : (byte >> 4);
}

/* Layer pixel for a name-table entry: colour, palette line and priority. */
static uint8_t entry_pixel(const RrVdp *v, uint16_t entry, unsigned x, unsigned y) {
    unsigned colour = tile_colour(v, entry, x, y);
    if (colour == 0) return 0;
    return (uint8_t)(colour | ((entry >> 13) & 3) << 4 | ((entry & 0x8000) ? PIXEL_PRIORITY : 0));
}

static unsigned plane_cells(unsigned size_bits) {
    return size_bits == 0 ? 32 : size_bits == 1 ? 64 : 128;
}

/* Horizontal scroll value for a plane on this line (which = 0 for A, 1 for B). */
static unsigned hscroll(const RrVdp *v, int line, int which) {
    uint32_t table = (uint32_t)(v->reg[13] & 0x3F) << 10;
    unsigned row;
    switch (v->reg[11] & 3) {
    case 2: row = (unsigned)line & ~7u; break; /* per cell row */
    case 3: row = (unsigned)line; break;       /* per line */
    default: row = 0; break;                   /* whole screen */
    }
    return vram16(v, table + row * 4 + (unsigned)which * 2) & 0x3FF;
}

static int64_t floor_div64(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

/* One plane's pixels for a line, `scale` output pixels to each screen pixel,
 * from screen x = -margin to RR_SCREEN_W - 1 + margin. `ground`, if not NULL,
 * says the row is this ground block (see rr_vdp_render_line_wide).
 * `shift` is NULL for the line's own horizontal scroll, or points at a 16.16
 * value that replaces it and may place the plane between screen pixels. */
static void render_plane(const RrVdp *v, int line, int which, int scale, const int32_t *shift, int margin,
                         const RrWideBlock *ground, const int32_t *beyond, uint8_t *out, uint16_t *detail) {
    uint32_t base = which == 0 ? (uint32_t)(v->reg[2] & 0x38) << 10 : (uint32_t)(v->reg[4] & 7) << 13;
    unsigned width = plane_cells(v->reg[16] & 3);
    unsigned height = plane_cells((v->reg[16] >> 4) & 3);
    unsigned scroll_x = shift != NULL ? (unsigned)(*shift >> 16) & 0x3FF : hscroll(v, line, which);
    /* The shift in output pixels, rounded down. */
    int64_t fine_shift = shift != NULL ? ((int64_t)*shift * scale) >> 16 : (int64_t)scroll_x * scale;
    int per_column = (v->reg[11] & 4) != 0;
    unsigned fine = scroll_x & 15;
    unsigned py_line = (unsigned)line;
    /* The plane's x at output column 0 and how far into that plane pixel the column is. */
    int64_t plane_x = -fine_shift >= 0 ? -fine_shift / scale : -((fine_shift + scale - 1) / scale);
    int within = (int)(-fine_shift - plane_x * scale);
    int ox = 0;
    plane_x -= margin; /* the first output column is screen x = -margin */
    /* A ground row is one block, not a pattern that repeats every 1,024
     * columns: which copy of the plane holds the block that belongs on this
     * row (the one whose road-side end stands within the game's limits), as
     * the number of plane widths to take off plane_x to count its columns. */
    int64_t copy = 0, road_end = 0; /* road_end: the screen x of that copy's road-side end */
    if (ground != NULL) {
        int64_t whole_shift = shift != NULL ? floor_div64((int64_t)*shift * scale >> 16, scale) : (int64_t)scroll_x;
        road_end = (ground->road_high ? ground->end : ground->first) + whole_shift;
        copy = -floor_div64(road_end - ground->fit_low, 1024);
        road_end += copy * 1024;
        if (road_end > ground->fit_high) ground = NULL; /* no copy fits: the plane as it is */
    }
    /* Where the game holds the road-side end at the limit that is the
     * screen's edge, the road's edge really lies `excess` output pixels
     * further out, if that is known (a shifted row with `beyond`); if it is
     * not, the grass runs on and no mark is drawn (`open`). */
    int64_t excess = 0;
    int open = 0;
    if (ground != NULL) {
        if (shift != NULL && beyond != NULL) excess = ((int64_t)*beyond * scale) >> 16;
        else open = road_end == (ground->road_high ? ground->fit_high : ground->fit_low);
    }
    /* The plane pixel last looked up: `scale` output pixels share one. */
    int64_t had_x = INT64_MIN;
    unsigned had_py = 0;
    uint16_t entry = 0;
    uint8_t pixel = 0;
    for (int sx = -margin; sx < RR_SCREEN_W + margin; sx++) {
        /* Beyond the screen's edges the edge pixel's column stands in. */
        unsigned x = sx < 0 ? 0 : sx >= RR_SCREEN_W ? RR_SCREEN_W - 1 : (unsigned)sx;
        /* Two-cell columns sit on the horizontally scrolled grid, so column k
         * covers screen x from 16k + s, where s is the fine scroll. Pixels left
         * of the first whole column use column 0 (the hardware's behaviour
         * there is a known quirk that is not reproduced). */
        unsigned column = (per_column && x >= fine) ? ((x - fine) >> 4) : 0;
        unsigned scroll_y = v->vsram[(column * 2 + (unsigned)which) % RR_VDP_VSRAM_ENTRIES] & 0x3FF;
        unsigned py = (py_line + scroll_y) & (height * 8 - 1);
        for (int k = 0; k < scale; k++, ox++) {
            if (plane_x != had_x || py != had_py) {
                unsigned px = (unsigned)plane_x & (width * 8 - 1);
                entry = vram16(v, base + ((py >> 3) * width + (px >> 3)) * 2);
                pixel = entry_pixel(v, entry, px & 7, py & 7);
                had_x = plane_x;
                had_py = py;
            }
            out[ox] = pixel;
            detail[ox] = entry;
            if (ground != NULL && (sx < 0 || sx >= RR_SCREEN_W)) {
                /* Beyond the screen: this copy's block, continued as grass on
                 * its far side and up to the road's edge, then its road-side
                 * mark, then nothing. `at` is the pixel's place in output
                 * pixels counted from this copy's column 0. */
                int64_t q = plane_x - copy * 1024, at = q * scale + within, column = -1;
                int period = ground->period;
                if (ground->road_high) {
                    if (q < ground->first) {
                        column = ground->first + (q - ground->first) - floor_div64(q - ground->first, period) * period;
                    } else if (open || at < (int64_t)ground->end * scale + excess) {
                        column = q < ground->end ? q : ground->end - period + (q - ground->end) % period;
                    } else {
                        int64_t moved = floor_div64(at - excess, scale);
                        if (moved < ground->end + ground->mark) column = moved;
                    }
                } else {
                    if (q >= ground->end) {
                        column = ground->end - period + (q - ground->end) % period;
                    } else if (open || at >= (int64_t)ground->first * scale + excess) {
                        column = q >= ground->first
                                 ? q : ground->first + (q - ground->first) - floor_div64(q - ground->first, period) * period;
                    } else {
                        int64_t moved = floor_div64(at - excess, scale);
                        if (moved >= ground->first - ground->mark) column = moved;
                    }
                }
                if (column < 0) {
                    out[ox] = 0;
                } else {
                    unsigned cx = (unsigned)column & (width * 8 - 1);
                    uint16_t there = vram16(v, base + ((py >> 3) * width + (cx >> 3)) * 2);
                    out[ox] = entry_pixel(v, there, cx & 7, py & 7);
                    detail[ox] = there;
                }
            }
            if (++within == scale) {
                within = 0;
                plane_x++;
            }
        }
    }
}

int rr_vdp_plane_row_runs(const RrVdp *v, int which, int line, int *first, int *end, int max_runs) {
    uint32_t base = which == 0 ? (uint32_t)(v->reg[2] & 0x38) << 10 : (uint32_t)(v->reg[4] & 7) << 13;
    unsigned width = plane_cells(v->reg[16] & 3), height = plane_cells((v->reg[16] >> 4) & 3);
    unsigned scroll_y = v->vsram[(unsigned)which % RR_VDP_VSRAM_ENTRIES] & 0x3FF;
    unsigned py = ((unsigned)line + scroll_y) & (height * 8 - 1);
    int runs = 0, open = 0;
    for (unsigned px = 0; px <= width * 8; px++) {
        int opaque = 0;
        if (px < width * 8) {
            uint16_t entry = vram16(v, base + ((py >> 3) * width + (px >> 3)) * 2);
            opaque = tile_colour(v, entry, px & 7, py & 7) != 0;
        }
        if (opaque && !open) {
            if (runs == max_runs) return max_runs + 1;
            first[runs] = (int)px;
            open = 1;
        } else if (!opaque && open) {
            end[runs++] = (int)px;
            open = 0;
        }
    }
    return runs;
}

int rr_vdp_plane_row_pixels(const RrVdp *v, int which, int line, uint8_t *out) {
    uint32_t base = which == 0 ? (uint32_t)(v->reg[2] & 0x38) << 10 : (uint32_t)(v->reg[4] & 7) << 13;
    unsigned width = plane_cells(v->reg[16] & 3), height = plane_cells((v->reg[16] >> 4) & 3);
    unsigned scroll_y = v->vsram[(unsigned)which % RR_VDP_VSRAM_ENTRIES] & 0x3FF;
    unsigned py = ((unsigned)line + scroll_y) & (height * 8 - 1);
    for (unsigned px = 0; px < width * 8; px++) {
        uint16_t entry = vram16(v, base + ((py >> 3) * width + (px >> 3)) * 2);
        unsigned colour = tile_colour(v, entry, px & 7, py & 7);
        out[px] = colour == 0 ? 0 : (uint8_t)(colour | ((entry >> 13) & 3) << 4);
    }
    return (int)(width * 8);
}

int rr_vdp_is_block_row(const RrVdp *v, int which, int line, const RrWideBlock *b) {
    /* A block and a mark on either side of it are three runs; at the finish
     * line the marks come in pieces (nine runs seen on the recordings). */
    enum { RUNS = 24 };
    int first[RUNS], end[RUNS];
    int runs = rr_vdp_plane_row_runs(v, which, line, first, end, RUNS);
    if (runs < 1 || runs > RUNS) return 0;
    int covered = 0;
    for (int i = 0; i < runs; i++) {
        if (first[i] < b->first - b->mark || end[i] > b->end + b->mark) return 0;
        if (first[i] <= b->first && end[i] >= b->end) covered = 1;
    }
    return covered;
}

/* Whether the window plane replaces plane A at (x, line). */
static int in_window(const RrVdp *v, unsigned x, int line) {
    unsigned h = (unsigned)(v->reg[17] & 0x1F) * 16, vpos = (unsigned)(v->reg[18] & 0x1F) * 8;
    int horizontal = (v->reg[17] & 0x80) ? x >= h : x < h;
    int vertical = (v->reg[18] & 0x80) ? (unsigned)line >= vpos : (unsigned)line < vpos;
    return horizontal || vertical;
}

/* `from_window` is set where the window plane, not plane A, supplied the pixel. */
static void render_window(const RrVdp *v, int line, uint8_t *out, uint16_t *detail, uint8_t *from_window) {
    memset(out, 0, RR_SCREEN_W);
    uint32_t base = (uint32_t)(v->reg[3] & 0x3C) << 10;
    for (unsigned x = 0; x < RR_SCREEN_W; x++) {
        if (!in_window(v, x, line)) continue;
        uint16_t entry = vram16(v, base + (((unsigned)line >> 3) * 64 + (x >> 3)) * 2);
        out[x] = entry_pixel(v, entry, x & 7, (unsigned)line & 7);
        detail[x] = entry;
        from_window[x] = 1;
    }
}

/* `detail` receives the sprite table index of the sprite that supplied each pixel. */
static void render_sprites(const RrVdp *v, int line, uint8_t *out, uint16_t *detail) {
    uint32_t table = (uint32_t)(v->reg[5] & 0x7E) << 9;
    int on_line = 0, pixels = 0, seen_nonzero_x = 0;
    unsigned index = 0;
    memset(out, 0, RR_SCREEN_W);
    for (int visited = 0; visited < MAX_SPRITES; visited++) {
        uint32_t s = table + index * 8;
        int y = (vram16(v, s) & 0x1FF) - 128; /* 9 bits outside interlace */
        unsigned size = v->vram[(s + 2) & 0xFFFF];
        unsigned link = v->vram[(s + 3) & 0xFFFF] & 0x7F;
        int cells_w = (int)((size >> 2) & 3) + 1, cells_h = (int)(size & 3) + 1;
        if (line >= y && line < y + cells_h * 8) {
            uint16_t attr = vram16(v, s + 4);
            int raw_x = vram16(v, s + 6) & 0x1FF;
            if (++on_line > MAX_SPRITES_PER_LINE) return;
            /* A sprite at x = 0 hides the rest of the line's sprites, but only
             * after a sprite with a non-zero x has been seen on the line. */
            if (raw_x == 0 && seen_nonzero_x) return;
            if (raw_x != 0) seen_nonzero_x = 1;
            int row_in_sprite = line - y;
            if (attr & 0x1000) row_in_sprite = cells_h * 8 - 1 - row_in_sprite;
            for (int px = 0; px < cells_w * 8; px++) {
                if (++pixels > MAX_SPRITE_PIXELS_PER_LINE) return;
                int x = raw_x - 128 + px;
                if (x < 0 || x >= RR_SCREEN_W || out[x] != 0) continue;
                int column = (attr & 0x0800) ? cells_w * 8 - 1 - px : px;
                /* Tiles are numbered down each column, then across. */
                unsigned tile = (attr & 0x07FF) + (unsigned)((column >> 3) * cells_h + (row_in_sprite >> 3));
                uint16_t entry = (uint16_t)((attr & 0xE000) | (tile & 0x07FF));
                out[x] = entry_pixel(v, entry, (unsigned)column & 7, (unsigned)row_in_sprite & 7);
                detail[x] = (uint16_t)index;
            }
        }
        if (link == 0 || link >= MAX_SPRITES) return;
        index = link;
    }
}

/* The picture sprite table entry `index` is shown from, or NULL for its tiles. */
static const RrFinePicture *fine_picture(const RrFineSprites *fine, unsigned index) {
    if (fine == NULL || fine->pictures == NULL || index >= RR_SPRITES_MAX) return NULL;
    int which = fine->picture[index];
    if (which < 0 || which >= RR_SPRITES_MAX) return NULL;
    const RrFinePicture *p = &fine->pictures[which];
    return (p->size != 0 && p->rows != NULL && p->stride != 0) ? p : NULL;
}

/* Sprite table entry `index` as eight bytes: from the supplied table if there
 * is one, else from video memory (copied to `copy`, since the table may wrap). */
static const uint8_t *sprite_entry(const RrVdp *v, const RrFineSprites *fine, unsigned index, uint8_t *copy) {
    if (fine != NULL && fine->table != NULL) return fine->table + (index % MAX_SPRITES) * 8;
    uint32_t s = ((uint32_t)(v->reg[5] & 0x7E) << 9) + index * 8;
    for (unsigned i = 0; i < 8; i++) copy[i] = v->vram[(s + i) & 0xFFFF];
    return copy;
}

/* Whether any sprite on this line is shown from a finer picture. The walk is
 * render_sprites's, without the per-line limits (so it may say yes for a
 * sprite those limits drop, which only costs time). */
static int line_has_fine_sprite(const RrVdp *v, int line, const RrFineSprites *fine) {
    unsigned index = 0;
    uint8_t copy[8];
    for (int visited = 0; visited < MAX_SPRITES; visited++) {
        const uint8_t *e = sprite_entry(v, fine, index, copy);
        int y = ((e[0] << 8 | e[1]) & 0x1FF) - 128;
        int cells_h = (int)(e[2] & 3) + 1;
        unsigned link = e[3] & 0x7F;
        const RrFinePicture *p = fine_picture(fine, index);
        int grow = p != NULL && (p->fx != 0 || p->fy != 0);
        if (line >= y && line < y + cells_h * 8 + grow && p != NULL) return 1;
        if (link == 0 || link >= MAX_SPRITES) return 0;
        index = link;
    }
    return 0;
}

/* Whether any sprite on this line is one that is not to be drawn. */
static int line_has_skipped_sprite(const RrVdp *v, int line, const RrFineSprites *fine) {
    unsigned index = 0;
    uint8_t copy[8];
    for (int visited = 0; visited < MAX_SPRITES; visited++) {
        const uint8_t *e = sprite_entry(v, fine, index, copy);
        int y = ((e[0] << 8 | e[1]) & 0x1FF) - 128;
        int cells_h = (int)(e[2] & 3) + 1;
        unsigned link = e[3] & 0x7F;
        if (line >= y && line < y + cells_h * 8 && index < RR_SPRITES_MAX && fine->skip[index]) return 1;
        if (link == 0 || link >= MAX_SPRITES) return 0;
        index = link;
    }
    return 0;
}

/* render_sprites for output row `r` of a line enlarged `scale` times, for the
 * screen pixels x_low to x_high - 1 (render_table_fine): `out`, `detail` and
 * the two soft arrays are (x_high - x_low) * scale wide. Sprites with a finer picture
 * take each output pixel from it (rr_vdp.h gives the rule); the others are
 * their tiles with each pixel repeated. Order and limits are render_sprites's,
 * counted in screen pixels. */
/* Softened pictures (RrFineSprites.soft): the colour of each output pixel of
 * the row being drawn that a softened picture supplied, and whether one did. */
enum { LINE_W_MAX = RR_SCREEN_W + 2 * RR_WIDE_MARGIN_MAX }; /* the widest row, in screen pixels */
static _Thread_local uint32_t soft_rgb[LINE_W_MAX * RR_SCALE_MAX];
static _Thread_local uint8_t soft_set[LINE_W_MAX * RR_SCALE_MAX];

/* The mean colour of the picture pixels in columns [x0, x1) of rows [y0, y1),
 * see-through ones and those outside the picture left out, in colour set
 * `set`. Returns 0 if fewer than half of the pixels have a colour; else 1,
 * with one of their colour numbers in *index. */
static int soft_colour(const RrVdp *v, const RrFinePicture *p, uint64_t x0, uint64_t x1, uint64_t y0,
                       uint64_t y1, unsigned set, uint32_t *rgb, uint8_t *index) {
    uint64_t total = (x1 - x0) * (y1 - y0), opaque = 0, sum[3] = {0, 0, 0};
    for (uint64_t y = y0; y < y1 && y < p->height; y++) {
        const uint8_t *row = p->rows + y * p->stride;
        for (uint64_t x = x0; x < x1 && x < p->width; x++) {
            uint8_t byte = row[x >> 1];
            uint8_t colour = (x & 1) ? (byte & 0x0F) : (byte >> 4);
            if (colour == 0) continue;
            uint32_t c = rr_vdp_colour(v->cram[(set << 4 | colour) & 0x3F]);
            sum[0] += (c >> 16) & 0xFF;
            sum[1] += (c >> 8) & 0xFF;
            sum[2] += c & 0xFF;
            opaque++;
            *index = colour;
        }
    }
    if (opaque == 0 || 2 * opaque < total) return 0;
    *rgb = (uint32_t)(((sum[0] + opaque / 2) / opaque) << 16 | ((sum[1] + opaque / 2) / opaque) << 8 |
                      (sum[2] + opaque / 2) / opaque);
    return 1;
}

static void render_table_fine(const RrVdp *v, int line, int scale, int r, const RrFineSprites *fine,
                              int x_low, int x_high, uint8_t *out, uint16_t *detail, uint32_t *soft_rgb,
                              uint8_t *soft_set) {
    int on_line = 0, pixels = 0, seen_nonzero_x = 0;
    unsigned index = 0;
    uint8_t copy[8];
    memset(out, 0, (size_t)(x_high - x_low) * (size_t)scale);
    memset(soft_set, 0, (size_t)(x_high - x_low) * (size_t)scale);
    for (int visited = 0; visited < MAX_SPRITES; visited++) {
        const uint8_t *e = sprite_entry(v, fine, index, copy);
        int y = ((e[0] << 8 | e[1]) & 0x1FF) - 128;
        unsigned size = e[2];
        unsigned link = e[3] & 0x7F;
        int cells_w = (int)((size >> 2) & 3) + 1, cells_h = (int)(size & 3) + 1;
        const RrFinePicture *p = fine_picture(fine, index);
        /* A picture standing between pixels reaches one pixel past the entry. */
        int grow = p != NULL && (p->fx != 0 || p->fy != 0);
        if (line >= y && line < y + cells_h * 8 + grow) {
            uint16_t attr = (uint16_t)(e[4] << 8 | e[5]);
            int raw_x = (e[6] << 8 | e[7]) & 0x1FF;
            if (++on_line > MAX_SPRITES_PER_LINE) return;
            if (raw_x == 0 && seen_nonzero_x) return;
            if (raw_x != 0) seen_nonzero_x = 1;
            if (fine != NULL && index < RR_SPRITES_MAX && fine->skip[index]) {
                /* Not drawn, but it takes its room in the line's limits as if it were. */
                pixels += cells_w * 8;
                if (pixels > MAX_SPRITE_PIXELS_PER_LINE) return;
                if (link == 0 || link >= MAX_SPRITES) return;
                index = link;
                continue;
            }
            int row_in_sprite = line - y;
            if (attr & 0x1000) row_in_sprite = cells_h * 8 - 1 - row_in_sprite;
            uint8_t marks = (uint8_t)(((attr >> 13) & 3) << 4 | ((attr & 0x8000) ? PIXEL_PRIORITY : 0));
            /* For a finer picture: the row of it this output row shows, if any. */
            const uint8_t *picture_row = NULL;
            int soft = 0;
            uint64_t soft_y0 = 0, soft_y1 = 0;
            int64_t edge = 0;
            uint64_t per_pixel = 0;
            if (p != NULL) {
                per_pixel = 2ull * (uint64_t)scale * p->size;
                edge = ((int64_t)p->x + fine->dx[index]) * scale + (((int64_t)p->fx * scale) >> 16);
                int64_t w = (int64_t)line * scale + r - (int64_t)p->y * scale - (((int64_t)p->fy * scale) >> 16);
                if (w >= 0 && w < (int64_t)p->rows_shown * scale) {
                    uint64_t sy = p->first_row + ((uint64_t)(2 * w + 1) << 16) / per_pixel;
                    if (sy < p->height) picture_row = p->rows + sy * p->stride;
                    /* Softened: the picture rows this output row's pixels cover. */
                    soft = fine->soft && p->size < 0x10000;
                    soft_y0 = p->first_row + ((uint64_t)w << 16) / (per_pixel / 2);
                    soft_y1 = p->first_row + (((uint64_t)(w + 1) << 16) + per_pixel / 2 - 1) / (per_pixel / 2);
                    if (soft) picture_row = p->rows; /* the rows are looked up pixel by pixel */
                }
            }
            for (int px = 0; px < cells_w * 8 + grow; px++) {
                if (px < cells_w * 8 && ++pixels > MAX_SPRITE_PIXELS_PER_LINE) return;
                int x = raw_x - 128 + px;
                if (x < x_low || x >= x_high) continue;
                int at = (x - x_low) * scale; /* the pixel's first output column in `out` */
                uint8_t *to = &out[at];
                if (p == NULL) {
                    int column = (attr & 0x0800) ? cells_w * 8 - 1 - px : px;
                    unsigned tile = (attr & 0x07FF) + (unsigned)((column >> 3) * cells_h + (row_in_sprite >> 3));
                    uint16_t entry = (uint16_t)((attr & 0xE000) | (tile & 0x07FF));
                    uint8_t pixel = entry_pixel(v, entry, (unsigned)column & 7, (unsigned)row_in_sprite & 7);
                    if (pixel == 0) continue;
                    for (int k = 0; k < scale; k++) {
                        if (to[k] != 0) continue;
                        to[k] = pixel;
                        detail[at + k] = (uint16_t)index;
                    }
                    continue;
                }
                if (picture_row == NULL) continue;
                for (int k = 0; k < scale; k++) {
                    if (to[k] != 0) continue;
                    int64_t c = (int64_t)x * scale + k;
                    int64_t u = p->mirrored ? edge - 1 - c : c - edge;
                    if (u < 0 || u >= (int64_t)p->columns * scale) continue;
                    if (soft) {
                        uint64_t across = (uint64_t)(u + (int64_t)p->first_column * scale), each = per_pixel / 2;
                        uint32_t rgb = 0;
                        uint8_t one = 0;
                        if (!soft_colour(v, p, (across << 16) / each, (((across + 1) << 16) + each - 1) / each, soft_y0,
                                         soft_y1, (attr >> 13) & 3, &rgb, &one))
                            continue;
                        to[k] = (uint8_t)(one | marks);
                        detail[at + k] = (uint16_t)index;
                        soft_rgb[at + k] = rgb;
                        soft_set[at + k] = 1;
                        continue;
                    }
                    uint64_t sx = ((uint64_t)(2 * (u + (int64_t)p->first_column * scale) + 1) << 16) / per_pixel;
                    if (sx >= p->width) continue;
                    uint8_t byte = picture_row[sx >> 1];
                    uint8_t colour = (sx & 1) ? (byte & 0x0F) : (byte >> 4);
                    if (colour == 0) continue;
                    to[k] = (uint8_t)(colour | marks);
                    detail[at + k] = (uint16_t)index;
                }
            }
        }
        if (link == 0 || link >= MAX_SPRITES) return;
        index = link;
    }
}

/* The sprites of output row `r` of a line, then the strips behind them, for
 * screen x from -margin to RR_SCREEN_W - 1 + margin: the sprites are on the
 * screen itself, the strips run on into the margins, and `margin_fine`, if
 * given, is a second table of sprites drawn beyond the screen's sides with
 * line limits of its own. It also fills the screen's last five columns
 * where the first table has no sprite pixel (the game stops a picture it
 * cuts four pixels short of its right edge, a mirrored one four or five). A pixel of the second table
 * has RR_MARGIN_SPRITE set in its detail. */
static void render_sprites_fine(const RrVdp *v, int line, int scale, int r, const RrFineSprites *fine,
                                const RrFineSprites *margin_fine, int margin, uint8_t *out, uint16_t *detail) {
    static _Thread_local uint8_t beside[LINE_W_MAX * RR_SCALE_MAX], beside_soft[LINE_W_MAX * RR_SCALE_MAX];
    static _Thread_local uint16_t beside_detail[LINE_W_MAX * RR_SCALE_MAX];
    static _Thread_local uint32_t beside_rgb[LINE_W_MAX * RR_SCALE_MAX];
    int edge = margin * scale, wide = (RR_SCREEN_W + 2 * margin) * scale;
    if (edge > 0) {
        memset(out, 0, (size_t)edge);
        memset(out + edge + RR_SCREEN_W * scale, 0, (size_t)edge);
        memset(soft_set, 0, (size_t)edge);
        memset(soft_set + edge + RR_SCREEN_W * scale, 0, (size_t)edge);
    }
    render_table_fine(v, line, scale, r, fine, 0, RR_SCREEN_W, out + edge, detail + edge, soft_rgb + edge,
                      soft_set + edge);
    const RrStrips *strips = fine != NULL ? fine->strips : NULL;
    if (strips != NULL && strips->scale == scale) {
        int row = line * scale + r;
        /* The strips count their columns from the screen's left edge. */
        int64_t from = (int64_t)strips->from[row] + edge, to = (int64_t)strips->to[row] + edge;
        if (from < 0) from = 0;
        if (to > wide) to = wide;
        for (int64_t c = from; c < to; c++) {
            if (out[c] != 0) continue;
            out[c] = (uint8_t)(strips->colour & 0x3F);
            detail[c] = 0xFFFF;
        }
    }
    if (margin_fine == NULL || margin <= 0) return;
    render_table_fine(v, line, scale, r, margin_fine, -margin, RR_SCREEN_W + margin, beside, beside_detail,
                      beside_rgb, beside_soft);
    int seam = edge + (RR_SCREEN_W - 5) * scale, right = edge + RR_SCREEN_W * scale;
    for (int c = 0; c < wide; c++) {
        if (beside[c] == 0) continue;
        if (c >= edge && c < right) {
            /* On the screen: only the last five columns, and never over an object. */
            if (c < seam || (out[c] != 0 && detail[c] != 0xFFFF)) continue;
        }
        out[c] = beside[c];
        detail[c] = (uint16_t)(beside_detail[c] | RR_MARGIN_SPRITE);
        soft_set[c] = beside_soft[c];
        soft_rgb[c] = beside_rgb[c];
    }
}

/* The parts of a line that no shift moves. */
typedef struct {
    int drawn;       /* 0: only the backdrop shows (display off, or a line off the screen) */
    int unsupported; /* a mode this renderer does not implement */
    uint8_t window[RR_SCREEN_W], sprites[RR_SCREEN_W], from_window[RR_SCREEN_W];
    uint16_t window_detail[RR_SCREEN_W], sprite_detail[RR_SCREEN_W];
} FixedLayers;

static void fixed_layers(const RrVdp *v, int line, FixedLayers *f) {
    f->drawn = (v->reg[1] & 0x40) && line >= 0 && line < RR_SCREEN_H;
    /* Only 40-cell, 28-row, non-interlaced display without shadow/highlight is
     * drawn. Anything else is made unmistakable. */
    f->unsupported = f->drawn && ((v->reg[12] & 0x81) != 0x81 /* 32-cell mode */
                                  || (v->reg[12] & 0x08)        /* shadow/highlight */
                                  || (v->reg[12] & 0x06)        /* interlace */
                                  || (v->reg[1] & 0x08));       /* 30 rows */
    if (!f->drawn || f->unsupported) return;
    memset(f->from_window, 0, sizeof f->from_window);
    render_window(v, line, f->window, f->window_detail, f->from_window);
    render_sprites(v, line, f->sprites, f->sprite_detail);
}

/* Where every pixel of a line comes from, `scale` output pixels to each
 * screen pixel, from screen x = -margin to RR_SCREEN_W - 1 + margin, with
 * the planes optionally shifted (see render_plane). The window and the
 * tile sprites are only on the screen itself; `wide_sprites`, if given, is
 * the sprite layer for the whole row. Returns 1 when the line is drawn in a mode this
 * renderer does not implement (the sources are then all backdrop, and the
 * picture shows the line in magenta). */
static int compose_line(const RrVdp *v, int line, const FixedLayers *f, int scale, int margin,
                        const int32_t *shift_a, const int32_t *shift_b, const RrWideBlock *ground_a,
                        const RrWideBlock *ground_b, const int32_t *beyond_a, const int32_t *beyond_b,
                        const uint8_t *wide_sprites, const uint16_t *wide_sprite_detail, int planes_kept,
                        RrPixelSource *out) {
    const RrPixelSource backdrop = {RR_SRC_BACKDROP, (uint8_t)(v->reg[7] & 0x3F), 0};
    int wide = (RR_SCREEN_W + 2 * margin) * scale;
    for (int x = 0; x < wide; x++) out[x] = backdrop;
    if (!f->drawn) return 0;
    if (f->unsupported) return 1;
    static _Thread_local uint8_t plane[2][LINE_W_MAX * RR_SCALE_MAX];
    static _Thread_local uint16_t plane_detail[2][LINE_W_MAX * RR_SCALE_MAX];
    /* `planes_kept`: the caller's last call was this line with these shifts. */
    if (!planes_kept) {
        render_plane(v, line, 1, scale, shift_b, margin, ground_b, beyond_b, plane[0], plane_detail[0]);
        render_plane(v, line, 0, scale, shift_a, margin, ground_a, beyond_a, plane[1], plane_detail[1]);
    }
    int ox = 0;
    for (int x = -margin; x < RR_SCREEN_W + margin; x++) {
        int on_screen = x >= 0 && x < RR_SCREEN_W;
        int window = on_screen && f->from_window[x];
        for (int k = 0; k < scale; k++, ox++) {
            uint8_t pixel[3] = {plane[0][ox], window ? f->window[x] : plane[1][ox],
                                wide_sprites != NULL ? wide_sprites[ox] : on_screen ? f->sprites[x] : 0};
            /* Back to front: B, A, sprites at low priority, then the same at high. */
#define SHOWS_HIGH(p) (PIXEL_OPAQUE(p) && ((p) & PIXEL_PRIORITY))
            int chosen = SHOWS_HIGH(pixel[2]) ? 2 : SHOWS_HIGH(pixel[1]) ? 1 : SHOWS_HIGH(pixel[0]) ? 0
                         : PIXEL_OPAQUE(pixel[2]) ? 2 : PIXEL_OPAQUE(pixel[1]) ? 1 : PIXEL_OPAQUE(pixel[0]) ? 0 : -1;
#undef SHOWS_HIGH
            if (chosen < 0) continue;
            out[ox].layer = chosen == 0 ? RR_SRC_PLANE_B : chosen == 2 ? RR_SRC_SPRITE
                            : window ? RR_SRC_WINDOW : RR_SRC_PLANE_A;
            out[ox].colour = pixel[chosen] & 0x3F;
            out[ox].detail = chosen == 0 ? plane_detail[0][ox]
                             : chosen == 2 ? (wide_sprites != NULL ? wide_sprite_detail[ox] : f->sprite_detail[x])
                             : window ? f->window_detail[x] : plane_detail[1][ox];
        }
    }
    return 0;
}

static int line_sources(const RrVdp *v, int line, RrPixelSource *out) {
    FixedLayers fixed;
    fixed_layers(v, line, &fixed);
    return compose_line(v, line, &fixed, 1, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, out);
}

void rr_vdp_render_line_sources(const RrVdp *v, int line, RrPixelSource *out) {
    (void)line_sources(v, line, out);
}

/* The picture is derived from the sources, so the two cannot disagree. */
void rr_vdp_render_line(const RrVdp *v, int line, uint32_t *out) {
    RrPixelSource sources[RR_SCREEN_W];
    int unsupported = line_sources(v, line, sources);
    for (int x = 0; x < RR_SCREEN_W; x++)
        out[x] = unsupported ? UNSUPPORTED_COLOUR : rr_vdp_colour(v->cram[sources[x].colour]);
}

void rr_vdp_render_line_scaled(const RrVdp *v, int line, int scale, const int32_t *shift_a,
                               const int32_t *shift_b, uint32_t *out) {
    rr_vdp_render_line_fine(v, line, scale, shift_a, shift_b, NULL, out);
}

void rr_vdp_render_line_fine(const RrVdp *v, int line, int scale, const int32_t *shift_a,
                             const int32_t *shift_b, const RrFineSprites *fine, uint32_t *out) {
    rr_vdp_render_line_wide(v, line, scale, shift_a, shift_b, NULL, NULL, fine, NULL, NULL, out, NULL);
}

static int is_all_zero(const uint8_t *p, size_t n) {
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        if (w != 0) return 0;
    }
    for (; i < n; i++)
        if (p[i] != 0) return 0;
    return 1;
}

/* The rows of one line, each (RR_SCREEN_W + 2 * margin) * scale pixels, row r
 * written from out[r * stride] (and sources[r * stride], if wanted). */
static void line_rows(const RrVdp *v, int line, const FixedLayers *fixed, int scale, int margin,
                      const int32_t *shift_a, const int32_t *shift_b, const RrWideBlock *ground_a,
                      const RrWideBlock *ground_b, const int32_t *beyond_a, const int32_t *beyond_b,
                      const RrFineSprites *fine, const RrFineSprites *margin_fine, uint32_t *out,
                      RrPixelSource *sources_out, size_t stride) {
    static _Thread_local RrPixelSource sources[LINE_W_MAX * RR_SCALE_MAX];
    static _Thread_local uint8_t wide_sprites[LINE_W_MAX * RR_SCALE_MAX];
    static _Thread_local uint16_t wide_detail[LINE_W_MAX * RR_SCALE_MAX];
    int across = RR_SCREEN_W + 2 * margin, wide = across * scale;
    int finer = fine != NULL && fixed->drawn && !fixed->unsupported
                && (fine->table != NULL || fine->strips != NULL || line_has_fine_sprite(v, line, fine)
                    || line_has_skipped_sprite(v, line, fine));
    if (margin <= 0) margin_fine = NULL;
    if (margin_fine != NULL && fixed->drawn && !fixed->unsupported) finer = 1;
    if (shift_a == NULL && shift_b == NULL && !finer) {
        /* Nothing moves: the original line with each pixel repeated, on every row. */
        int unsupported = compose_line(v, line, fixed, 1, margin, NULL, NULL, ground_a, ground_b, NULL, NULL, NULL,
                                       NULL, 0, sources);
        for (int x = 0; x < across; x++) {
            uint32_t colour = unsupported ? UNSUPPORTED_COLOUR : rr_vdp_colour(v->cram[sources[x].colour]);
            for (int k = 0; k < scale; k++) out[x * scale + k] = colour;
            if (sources_out != NULL)
                for (int k = 0; k < scale; k++) sources_out[x * scale + k] = sources[x];
        }
        for (int r = 1; r < scale; r++) {
            memcpy(out + (size_t)r * stride, out, (size_t)wide * sizeof *out);
            if (sources_out != NULL)
                memcpy(sources_out + (size_t)r * stride, sources_out, (size_t)wide * sizeof *sources_out);
        }
        return;
    }
    uint32_t colours[64];
    for (int i = 0; i < 64; i++) colours[i] = rr_vdp_colour(v->cram[i]);
    int bare_above = 0; /* the row above had no sprite pixel */
    for (int r = 0; r < scale; r++) {
        uint32_t *to = out + (size_t)r * stride;
        if (finer) render_sprites_fine(v, line, scale, r, fine, margin_fine, margin, wide_sprites, wide_detail);
        /* The planes lie as on the row above; with no sprite pixel on either row, the row is the same. */
        int planes_kept = r > 0 && (shift_a == NULL || shift_a[r] == shift_a[r - 1])
                          && (shift_b == NULL || shift_b[r] == shift_b[r - 1])
                          && (beyond_a == NULL || beyond_a[r] == beyond_a[r - 1])
                          && (beyond_b == NULL || beyond_b[r] == beyond_b[r - 1]);
        int bare = !finer || is_all_zero(wide_sprites, (size_t)wide);
        if (planes_kept && bare && bare_above) {
            memcpy(to, to - stride, (size_t)wide * sizeof *to);
            if (sources_out != NULL)
                memcpy(sources_out + (size_t)r * stride, sources_out + (size_t)(r - 1) * stride,
                       (size_t)wide * sizeof *sources_out);
            continue;
        }
        bare_above = bare;
        int unsupported = compose_line(v, line, fixed, scale, margin, shift_a != NULL ? &shift_a[r] : NULL,
                                       shift_b != NULL ? &shift_b[r] : NULL, ground_a, ground_b,
                                       beyond_a != NULL ? &beyond_a[r] : NULL, beyond_b != NULL ? &beyond_b[r] : NULL,
                                       finer ? wide_sprites : NULL, finer ? wide_detail : NULL, planes_kept, sources);
        for (int x = 0; x < wide; x++) to[x] = unsupported ? UNSUPPORTED_COLOUR : colours[sources[x].colour];
        if (finer && !unsupported && ((fine != NULL && fine->soft) || (margin_fine != NULL && margin_fine->soft))) {
            /* Where a softened picture is what shows, its own colour. */
            for (int x = 0; x < wide; x++)
                if (soft_set[x] && sources[x].layer == RR_SRC_SPRITE) to[x] = soft_rgb[x];
        }
        if (sources_out != NULL) memcpy(sources_out + (size_t)r * stride, sources, (size_t)wide * sizeof *sources);
    }
}

void rr_vdp_render_line_wide(const RrVdp *v, int line, int scale, const int32_t *shift_a,
                             const int32_t *shift_b, const int32_t *beyond_a, const int32_t *beyond_b,
                             const RrFineSprites *fine, const RrFineSprites *margin_fine,
                             const RrWide *wide, uint32_t *out, RrPixelSource *sources) {
    FixedLayers fixed;
    if (scale < 1) scale = 1;
    if (scale > RR_SCALE_MAX) scale = RR_SCALE_MAX;
    int margin = wide == NULL || wide->margin < 0 ? 0
                 : wide->margin > RR_WIDE_MARGIN_MAX ? RR_WIDE_MARGIN_MAX : wide->margin;
    fixed_layers(v, line, &fixed);
    size_t stride = (size_t)(RR_SCREEN_W + 2 * margin) * (size_t)scale;
    int narrow = margin > 0 && line >= wide->lines;
    if (margin > 0 && !narrow && fixed.drawn && !fixed.unsupported) {
        for (int x = 0; x < RR_SCREEN_W && !narrow; x++) narrow = fixed.from_window[x];
    }
    if (!narrow) {
        /* Which planes hold a ground row on this line. */
        const RrWideBlock *ground[2] = {NULL, NULL};
        if (margin > 0 && fixed.drawn && !fixed.unsupported && !(v->reg[11] & 4)
            && plane_cells(v->reg[16] & 3) == 128
            && (wide->ground_backdrop < 0 || (v->reg[7] & 0x3F) == wide->ground_backdrop)) {
            for (int which = 0; which < 2; which++) {
                const RrWideBlock *b = &wide->block[which];
                if (b->period != 0 && rr_vdp_is_block_row(v, which, line, b)) ground[which] = b;
            }
        }
        line_rows(v, line, &fixed, scale, margin, shift_a, shift_b, ground[0], ground[1], beyond_a, beyond_b, fine,
                  margin_fine, out, sources, stride);
        return;
    }
    /* The original row between two panels. */
    size_t edge = (size_t)margin * (size_t)scale, middle = (size_t)RR_SCREEN_W * (size_t)scale;
    const RrPixelSource panel = {RR_SRC_PANEL, 0, 0};
    for (int r = 0; r < scale; r++) {
        uint32_t *row = out + (size_t)r * stride;
        for (size_t c = 0; c < edge; c++) row[c] = row[edge + middle + c] = wide->panel;
        if (sources == NULL) continue;
        RrPixelSource *from = sources + (size_t)r * stride;
        for (size_t c = 0; c < edge; c++) from[c] = from[edge + middle + c] = panel;
    }
    line_rows(v, line, &fixed, scale, 0, shift_a, shift_b, NULL, NULL, NULL, NULL, fine, NULL, out + edge,
              sources != NULL ? sources + edge : NULL, stride);
    /* The dashboard: the window's, from one edge of the screen to the other. */
    if (wide->dash.columns == 0 || wide->lines <= 0 || line < wide->lines || !fixed.drawn || fixed.unsupported
        || !fixed.from_window[0] || !fixed.from_window[RR_SCREEN_W - 1])
        return;
    int mirrors = line >= wide->dash.first && line < wide->dash.end;
    uint32_t fill = rr_vdp_colour(v->cram[wide->dash.fill & 0x3F]);
    for (int r = 0; r < scale; r++) {
        uint32_t *row = out + (size_t)r * stride;
        const uint32_t *screen = row + edge;
        for (int k = 0; k < margin; k++) {
            int left = 0, right = RR_SCREEN_W - 1, framed = mirrors && k < wide->dash.columns;
            if (framed) {
                left = wide->dash.left_from + k;
                right = wide->dash.right_from - k;
                if (left < 0 || left >= RR_SCREEN_W || right < 0 || right >= RR_SCREEN_W) continue;
            }
            for (int s = 0; s < scale; s++) {
                row[edge - 1 - (size_t)(k * scale + s)] = mirrors && !framed ? fill : screen[left * scale + s];
                row[edge + middle + (size_t)(k * scale + s)] = mirrors && !framed ? fill
                                                                 : screen[right * scale + scale - 1 - s];
            }
        }
    }
}

void rr_vdp_line_state(const RrVdp *v, int line, RrVdpLineState *out) {
    memcpy(out->reg, v->reg, sizeof out->reg);
    out->hscroll[0] = (uint16_t)hscroll(v, line, 0);
    out->hscroll[1] = (uint16_t)hscroll(v, line, 1);
    out->vscroll[0] = v->vsram[0] & 0x3FF;
    out->vscroll[1] = v->vsram[1] & 0x3FF;
}

int rr_vdp_write_ppm(const RrVdp *v, const char *path) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) return 1;
    fprintf(f, "P6\n%d %d\n255\n", RR_SCREEN_W, RR_SCREEN_H);
    uint32_t row[RR_SCREEN_W];
    for (int line = 0; line < RR_SCREEN_H; line++) {
        rr_vdp_render_line(v, line, row);
        for (int x = 0; x < RR_SCREEN_W; x++) {
            fputc((int)(row[x] >> 16) & 0xFF, f);
            fputc((int)(row[x] >> 8) & 0xFF, f);
            fputc((int)row[x] & 0xFF, f);
        }
    }
    return fclose(f) != 0;
}
