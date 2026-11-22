/* Sega 315-5313 video chip model (mode 5 only): registers, ports, memories, DMA.
 *
 * The model holds no timing of its own. Whoever owns it says whether the beam
 * is in blanking when the status port is read, and raises the frame interrupt
 * flag at the start of vertical blanking.
 */
#ifndef RR_VDP_H
#define RR_VDP_H

#include <stdio.h>

#include "rr_machine.h"

#define RR_VDP_REGISTERS 24
#define RR_VDP_VSRAM_ENTRIES 40
#define RR_SCREEN_W 320
#define RR_SCREEN_H 224

typedef struct {
    uint8_t reg[RR_VDP_REGISTERS];
    uint8_t vram[0x10000];
    uint16_t cram[64];                  /* 9-bit colours as written: 0000 BBB0 GGG0 RRR0 */
    uint16_t vsram[RR_VDP_VSRAM_ENTRIES];
    uint16_t addr;                      /* current address register */
    uint8_t code;                       /* CD5..CD0 */
    uint8_t pending;                    /* 1 after the first half of a two-word command */
    uint8_t fill_pending;               /* a VRAM fill is waiting for its data word */
    uint16_t first_word;                /* the first half of the command in progress */
    uint16_t status;                    /* sticky status bits (frame interrupt occurred) */
    uint32_t dma_words;                 /* words moved by 68000-to-VDP DMA, until taken */
    uint64_t vram_writes;               /* counts every change to video memory, however made */
} RrVdp;

void rr_vdp_reset(RrVdp *v);

/* `m` is the bus that 68000-to-VDP DMA reads its source from. */
void rr_vdp_write_control(RrVdp *v, RrMachine *m, uint16_t value);
void rr_vdp_write_data(RrVdp *v, uint16_t value);
uint16_t rr_vdp_read_data(RrVdp *v);

/* Status port. Reading clears a half-written command. */
uint16_t rr_vdp_read_status(RrVdp *v, int in_vblank, int in_hblank);

/* The frame (vertical blanking) interrupt has occurred: sets the status flag.
 * The flag stays set, whatever is read, until the 68000 acknowledges the
 * interrupt by taking it. */
void rr_vdp_frame_interrupt(RrVdp *v);
void rr_vdp_ack_frame_interrupt(RrVdp *v);

/* Words transferred by 68000-to-VDP DMA since the last call; the caller uses
 * it to hold the 68000 for the time the transfer takes. */
uint32_t rr_vdp_take_dma_words(RrVdp *v);

int rr_vdp_save(const RrVdp *v, FILE *f);
int rr_vdp_load(RrVdp *v, FILE *f);

/* The runs of opaque pixels on the plane row shown on display `line`, for
 * plane `which` (0 = A, 1 = B): plane pixel columns [first[i], end[i]), lowest
 * first, on that one pixel row (through the vertical scroll of the first
 * column). Returns the number of runs; if there are more than `max_runs`,
 * returns max_runs + 1 with the first max_runs filled. */
int rr_vdp_plane_row_runs(const RrVdp *v, int which, int line, int *first, int *end, int max_runs);

/* The whole row of plane `which` shown on `line`, one byte a pixel (colour in
 * the low four bits, palette line above; 0 where the plane shows nothing),
 * into `out` (room for 1,024). Returns the plane's width in pixels. */
int rr_vdp_plane_row_pixels(const RrVdp *v, int which, int line, uint8_t *out);

/* The ground block of one plane, for the wide picture (render/rr_wide.h). */
typedef struct {
    uint16_t first, end;  /* the ground block's plane columns: [first, end) */
    uint16_t mark;        /* the widest the edge-line mark gets beyond either end */
    uint8_t road_high;    /* 1: the road is on the high-column side (plane A); 0: the low (plane B) */
    uint8_t period;       /* the block repeats sideways with this period */
    int16_t fit_low, fit_high; /* on a line with no shift given: the screen x of the block's
                                  road-side end lies in [fit_low, fit_high] */
} RrWideBlock;
/* Is the row of plane `which` (0 A, 1 B) shown on `line` this ground block:
 * opaque right across [first, end) and blank outside
 * [first - mark, end + mark)? (The vertical scroll's first value is used.) */
int rr_vdp_is_block_row(const RrVdp *v, int which, int line, const RrWideBlock *b);

/* Render one display line (0..223) into 320 pixels of 0x00RRGGBB. */
void rr_vdp_render_line(const RrVdp *v, int line, uint32_t *out);
/* Which layer supplied a pixel. The video chip builds the picture from a
 * backdrop colour, two scrolling tile planes (B behind A), a fixed window
 * plane that replaces part of plane A, and sprites. */
enum { RR_SRC_BACKDROP, RR_SRC_PLANE_B, RR_SRC_PLANE_A, RR_SRC_WINDOW, RR_SRC_SPRITE,
       RR_SRC_PANEL /* a side panel of a wide row (rr_vdp_render_line_wide): not the game's */ };
typedef struct {
    uint8_t layer;   /* RR_SRC_* */
    uint8_t colour;  /* 0..63, index into CRAM */
    uint16_t detail; /* sprite: its index in the sprite table; plane or window: the name-table entry */
} RrPixelSource;
/* The same line as rr_vdp_render_line, as sources: the colour of pixel x is
 * rr_vdp_colour(cram[out[x].colour]) (except in the unimplemented modes). */
void rr_vdp_render_line_sources(const RrVdp *v, int line, RrPixelSource *out);

/* What governs one display line: the registers, and each plane's horizontal
 * scroll for that line and vertical scroll (first column). Index 0 is plane
 * A, 1 is plane B. */
typedef struct {
    uint8_t reg[RR_VDP_REGISTERS];
    uint16_t hscroll[2], vscroll[2];
} RrVdpLineState;
void rr_vdp_line_state(const RrVdp *v, int line, RrVdpLineState *out);

/* The same line enlarged: `scale` rows of 320 * scale pixels, row after row.
 * shift_a and shift_b are NULL, or `scale` values, one per row: that plane's
 * sideways shift for the row in 16.16 fixed point, in the sense of the
 * horizontal scroll table (its whole part replaces the table's value for the
 * line). With a fraction the plane moves by parts of an original pixel, so a
 * shift that changes steadily from line to line draws a slope instead of a
 * staircase. Everything else (the other plane, the window, sprites, the
 * backdrop) is the original line with each pixel repeated.
 *
 * Whatever the shifts, output column X of a row reads the plane where the
 * original reads it for screen x = floor((X - floor(shift * scale)) / scale)
 * with no shift. The original uses only the whole part of a shift, which
 * places the plane up to one pixel left of where the fraction says. So with
 * a shift whose whole part is the table's value, the last of the `scale`
 * output pixels of each screen pixel (column scale * x + scale - 1) is the
 * original line's pixel x, whatever the fraction; with whole shifts every one
 * of them is. */
enum { RR_SCALE_MAX = 8 };
void rr_vdp_render_line_scaled(const RrVdp *v, int line, int scale, const int32_t *shift_a,
                               const int32_t *shift_b, uint32_t *out);

/* A picture some sprites show, finer than their tiles: the picture a sprite's
 * tiles were scaled from, with where it stands on screen, so the enlarged
 * frame can take each of its pixels from the picture itself. */
typedef struct {
    const uint8_t *rows;    /* `height` rows of `stride` bytes; two pixels to a byte, the high
                               four bits first; colour 0 is see-through */
    uint16_t width, height, stride; /* in picture pixels and bytes */
    uint32_t size;          /* enlargement on screen; 0x10000 is the picture's natural size */
    int16_t x, y;           /* screen pixel of the first shown column (when mirrored: one past
                               its right-hand end) and of the first shown row */
    uint8_t mirrored;
    uint16_t first_column;  /* screen pixels of the enlarged picture cut off before x */
    uint16_t first_row;     /* picture rows skipped above y */
    uint16_t columns, rows_shown; /* screen pixels shown from x and lines shown from y;
                                     beyond them the picture is see-through */
    uint16_t fx, fy;        /* how far right and down of (x, y) the picture really stands, in
                               65,536ths of a screen pixel; 0, 0 is exactly (x, y) */
} RrFinePicture;
enum { RR_SPRITES_MAX = 80 };

/* Strips drawn with the sprites of an enlarged picture: on output row q
 * (0 .. RR_SCREEN_H * scale - 1) the output columns from[q] up to, not
 * including, to[q] show palette entry `colour` (0..63) at low priority,
 * behind every sprite. from >= to is no strip. Used only when `scale` is the
 * scale the line is drawn at. */
typedef struct {
    int scale;
    uint8_t colour;
    int32_t from[RR_SCREEN_H * RR_SCALE_MAX], to[RR_SCREEN_H * RR_SCALE_MAX];
} RrStrips;

typedef struct {
    const RrFinePicture *pictures;
    int16_t picture[RR_SPRITES_MAX]; /* per sprite table entry: index into pictures, or -1 */
    int16_t dx[RR_SPRITES_MAX];      /* how far sideways this entry's copy of the picture stands */
    const uint8_t *table;            /* NULL, or 640 bytes (80 entries of 8) used in place of the
                                        sprite table in video memory: links, positions, sizes and
                                        the tiles of entries without a picture */
    const RrStrips *strips;          /* NULL, or strips drawn behind the sprites */
    uint8_t skip[RR_SPRITES_MAX];    /* 1: this table entry is not drawn. It still counts in the
                                        per-line limits, so leaving it out changes nothing else */
    uint8_t soft;                    /* 1: a picture drawn below its natural size (size under
                                        0x10000) is softened: each output pixel is the mean colour
                                        of the picture pixels its square covers, see-through ones
                                        left out, and is itself see-through if fewer than half of
                                        them have a colour. Other pictures are drawn as with 0.
                                        The mean is not one of the game's 64 colours. */
} RrFineSprites;

/* As rr_vdp_render_line_scaled, and a sprite table entry i with
 * fine->picture[i] >= 0 takes its pixels from that picture instead of from
 * its tiles. fine may be NULL (then this is rr_vdp_render_line_scaled).
 *
 * The entry still covers only its own rectangle, has its own colour set and
 * priority, and keeps its place in the sprite order and in the per-line
 * limits; where the picture is see-through the next sprite or the planes
 * show. Within the rectangle, output column c and output row q (both counted
 * in output pixels from the top-left of the frame) show the picture pixel
 *
 *   u = mirrored ? (x + dx) * scale + FX - 1 - c : c - (x + dx) * scale - FX
 *   w = q - y * scale - FY
 *   column floor(((u + 0.5) / scale + first_column) / (size / 0x10000))
 *   row    first_row + floor(((w + 0.5) / scale) / (size / 0x10000))
 *
 * and nothing where u is outside 0 .. columns * scale - 1, w is outside
 * 0 .. rows_shown * scale - 1, or the pixel is outside the picture. So a
 * picture at its natural size is each of its pixels repeated, a smaller one
 * shows more of its pixels than the screen's own resolution could, and a
 * larger one gives every pixel the same room. A picture with size 0 is not
 * used. FX and FY are the picture's fx and fy in whole output pixels
 * (floor(fx * scale / 65536)). An entry whose picture has a non-zero fx or fy
 * covers its rectangle grown by one screen pixel to the right and one line
 * down, so that the moved picture is not cut at the entry's edge; the limits
 * still count the entry's own width. Meant for scale 2 and up: at scale 1 it samples the picture at pixel
 * centres, which is not what the game's own scaler shows. */
void rr_vdp_render_line_fine(const RrVdp *v, int line, int scale, const int32_t *shift_a,
                             const int32_t *shift_b, const RrFineSprites *fine, uint32_t *out);

/* A wider enlarged picture: `margin` more screen pixels on either side. */
enum { RR_WIDE_MARGIN_MAX = 64 };
typedef struct {
    int margin;            /* game pixels each side, 0..RR_WIDE_MARGIN_MAX (more is the most) */
    int lines;             /* lines 0..lines-1 are drawn wide; the rest are the original between panels */
    uint32_t panel;        /* 0x00RRGGBB of the side panels */
    RrWideBlock block[2];  /* plane A, plane B; period 0: the ground is not continued */
    int ground_backdrop;   /* a line is ground only if register 7 holds this colour (0..63)
                              as it is drawn; -1: whatever it holds */
    /* The dashboard below the wide lines: a line there whose two edge pixels
     * are the window's is continued into the margins instead of standing
     * between panels. On lines [first, end), the mirrors', margin pixel k
     * (0 nearest the screen) is screen column left_from + k on the left and
     * right_from - k on the right, for k < columns: the mirrors' inner frames,
     * mirrored. Further out it is colour `fill` (0..63). On other lines the
     * edge pixel runs on. columns 0, or no wide lines (a menu): panels. */
    struct {
        int16_t first, end, left_from, right_from;
        uint8_t columns, fill;
    } dash;
} RrWide;

/* One display line as `scale` rows of (RR_SCREEN_W + 2 * margin) * scale
 * pixels: screen x from -margin to RR_SCREEN_W - 1 + margin, so the game's
 * own picture is the middle of each row. `wide` NULL or a margin of 0 is
 * rr_vdp_render_line_fine exactly.
 *
 * On a line below wide->lines each plane is sampled on beyond the screen's
 * edges with the row's own shift, wrapping round the plane as the chip does;
 * a vertical scroll value per column keeps the first column's value to the
 * left and the last column's to the right. The window, the tile sprites and
 * the fine sprites exist only in the middle; fine->strips run on into the
 * margins (their columns are counted from the screen's left edge, so they
 * may be negative). A line with any window pixel,
 * and every line from wide->lines down, is the original row between two
 * panels of colour wide->panel.
 *
 * A ground row is not sampled round the plane. The game's ground is one
 * block of each plane (wide->block), which it slides so that the block's
 * road-side end stands at the road's edge; the copies of it 1,024 columns
 * away are not ground. On a wide line drawn with wide->ground_backdrop, a
 * plane whose row is its block (rr_vdp_is_block_row; period 0 turns this
 * off, as does a vertical scroll per column or a plane that is not 1,024
 * wide) is taken as the one copy whose road-side end stands at a screen x
 * within [fit_low, fit_high], and beyond the screen's edges that copy is
 * drawn as the ground it stands for:
 *   - beyond the block's far end the block is continued, repeating its last
 *     `period` columns (the mark the plane holds there is not drawn);
 *   - on the road side the block's mark shows, and nothing further out.
 * The game holds a road edge that leaves its screen at the screen's edge.
 * beyond_a and beyond_b (NULL, or one 16.16 value per row like the shifts:
 * RrRoadShifts.beyond_a/b) say how much further out the edge really is on a
 * shifted row: the block is continued up to there and its mark drawn there.
 * Where that is not known (no shift for the plane, or no `beyond`) and the
 * block's road-side end stands exactly at the screen's edge, the block is
 * continued right through the margin and no mark is drawn.
 * The screen's own 320 pixels are never changed by any of this.
 *
 * margin_fine, if not NULL, is a second set of sprites with a table of its
 * own (margin_fine->table; its strips are not used), drawn on the wide
 * lines beyond the screen's sides with per-line limits of its own, so that
 * it never costs the screen a sprite. It is in front of the strips and
 * takes its place among the planes by the same priority rule. It also
 * fills the screen's last five columns (315 to 319) wherever the first set
 * has no sprite pixel there: the game stops a picture it cuts four pixels
 * short of its right edge (a mirrored one four or five, as its pairs of
 * pixels fall), and the same picture uncut in margin_fine closes that gap. Nowhere else does it change the screen's own 320 pixels.
 *
 * `sources`, if not NULL, receives the same rows as sources (a panel pixel
 * is RR_SRC_PANEL; a softened fine picture's pixel keeps its sprite source;
 * a pixel of margin_fine is a sprite whose detail has RR_MARGIN_SPRITE set). */
enum { RR_MARGIN_SPRITE = 0x4000 };
void rr_vdp_render_line_wide(const RrVdp *v, int line, int scale, const int32_t *shift_a,
                             const int32_t *shift_b, const int32_t *beyond_a, const int32_t *beyond_b,
                             const RrFineSprites *fine, const RrFineSprites *margin_fine,
                             const RrWide *wide, uint32_t *out, RrPixelSource *sources);

/* Render the whole frame to a binary PPM (P6). Returns 0 on success. */
int rr_vdp_write_ppm(const RrVdp *v, const char *path);
/* A 9-bit colour as stored in CRAM to 0x00RRGGBB. */
uint32_t rr_vdp_colour(uint16_t cram_value);

#endif
