/* Unit tests for the wide picture's ground. Run with: make -C port test */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rr_vdp.h"
#include "rr_wide.h"

/* A video chip with two 128 x 32 cell planes: A at 0x4000, B at 0x6000. */
static RrVdp *wide_planes(void) {
    RrVdp *v = calloc(1, sizeof *v);
    assert(v != NULL);
    v->reg[1] = 0x44;
    v->reg[2] = 0x10;
    v->reg[4] = 0x03;
    v->reg[12] = 0x81;
    v->reg[16] = 0x03;
    return v;
}

static void tile_pixel(RrVdp *v, int tile, int x, int y, int colour) {
    uint8_t *byte = &v->vram[tile * 32 + y * 4 + x / 2];
    *byte = (x & 1) ? (uint8_t)((*byte & 0xF0) | colour) : (uint8_t)((*byte & 0x0F) | colour << 4);
}

/* Colour `colour` on pixels [from, to) of pixel row `py` of a plane, each
 * touched cell getting a tile of its own (tiles 0x400 to 0x5FF). */
static void plane_pixels(RrVdp *v, int which, int py, int from, int to, int colour) {
    uint32_t base = which == 0 ? 0x4000 : 0x6000;
    for (int px = from; px < to; px++) {
        int cell = (py >> 3) * 128 + (px >> 3), tile = 0x400 + (which * 0x100) + ((py >> 3) & 1) * 0x80 + (px >> 3);
        v->vram[base + cell * 2] = (uint8_t)(tile >> 8);
        v->vram[base + cell * 2 + 1] = (uint8_t)tile;
        tile_pixel(v, tile, px & 7, py & 7, colour);
    }
}

static void test_the_blocks_are_the_games(void) {
    RrWideBlock b[2];
    rr_wide_blocks(b);
    assert(b[0].first == 352 && b[0].end == 672 && b[0].mark == 24 && b[0].road_high == 1 && b[0].period == 8);
    assert(b[0].fit_low == -352 && b[0].fit_high == 320);
    assert(b[1].first == 352 && b[1].end == 672 && b[1].mark == 24 && b[1].road_high == 0 && b[1].period == 8);
    assert(b[1].fit_low == 0 && b[1].fit_high == 384);
}

static void test_a_block_with_marks_is_a_ground_row(void) {
    RrVdp *v = wide_planes();
    RrWideBlock b[2];
    rr_wide_blocks(b);
    plane_pixels(v, 0, 3, 328, 336, 5);
    plane_pixels(v, 0, 3, 352, 672, 6);
    plane_pixels(v, 0, 3, 688, 696, 5);
    assert(rr_wide_is_ground_row(v, 0, 3, &b[0]));
    /* Marks that touch the block, as far out as they go. */
    plane_pixels(v, 1, 5, 328, 696, 6);
    assert(rr_wide_is_ground_row(v, 1, 5, &b[1]));
    /* One pixel further out than a mark reaches. */
    plane_pixels(v, 1, 5, 327, 328, 6);
    assert(!rr_wide_is_ground_row(v, 1, 5, &b[1]));
    free(v);
}

/* At the finish line the marks are drawn in pieces (full-throttle recording, frame 11555). */
static void test_marks_in_pieces_are_still_a_ground_row(void) {
    RrVdp *v = wide_planes();
    RrWideBlock b[2];
    rr_wide_blocks(b);
    static const int piece[9][2] = {{329, 338}, {340, 344}, {345, 346}, {347, 348}, {349, 675},
                                    {676, 677}, {678, 679}, {680, 684}, {686, 696}};
    for (int i = 0; i < 9; i++) plane_pixels(v, 0, 3, piece[i][0], piece[i][1], 6);
    assert(rr_wide_is_ground_row(v, 0, 3, &b[0]));
    plane_pixels(v, 0, 3, 700, 701, 6);
    assert(!rr_wide_is_ground_row(v, 0, 3, &b[0]));
    free(v);
}

static void test_a_block_alone_is_a_ground_row(void) {
    RrVdp *v = wide_planes();
    RrWideBlock b[2];
    rr_wide_blocks(b);
    plane_pixels(v, 1, 3, 352, 672, 6);
    assert(rr_wide_is_ground_row(v, 1, 3, &b[1]));
    /* One pixel short of the block is not. */
    plane_pixels(v, 0, 3, 352, 671, 6);
    assert(!rr_wide_is_ground_row(v, 0, 3, &b[0]));
    free(v);
}

static void test_a_sparse_sky_row_inside_the_window_is_not(void) {
    RrVdp *v = wide_planes();
    RrWideBlock b[2];
    rr_wide_blocks(b);
    plane_pixels(v, 1, 3, 396, 498, 6);
    assert(!rr_wide_is_ground_row(v, 1, 3, &b[1]));
    free(v);
}

static void test_a_full_row_is_not(void) {
    RrVdp *v = wide_planes();
    RrWideBlock b[2];
    rr_wide_blocks(b);
    plane_pixels(v, 1, 3, 0, 1024, 6);
    assert(!rr_wide_is_ground_row(v, 1, 3, &b[1]));
    free(v);
}

static void test_a_block_with_something_at_column_100_is_not(void) {
    RrVdp *v = wide_planes();
    RrWideBlock b[2];
    rr_wide_blocks(b);
    plane_pixels(v, 0, 3, 352, 672, 6);
    assert(rr_wide_is_ground_row(v, 0, 3, &b[0]));
    plane_pixels(v, 0, 3, 100, 101, 6);
    assert(!rr_wide_is_ground_row(v, 0, 3, &b[0]));
    free(v);
}

static void test_an_empty_row_is_not(void) {
    RrVdp *v = wide_planes();
    RrWideBlock b[2];
    rr_wide_blocks(b);
    assert(!rr_wide_is_ground_row(v, 0, 3, &b[0]));
    free(v);
}

/* --- the checks, against right and wrong pictures ----------------------- */

enum { SCALE = 2, MARGIN = 39, STRIDE = (RR_SCREEN_W + 2 * MARGIN) * SCALE };
static uint32_t wide_px[STRIDE * RR_SCREEN_H * SCALE], narrow_px[RR_SCREEN_W * SCALE * RR_SCREEN_H * SCALE];
static RrPixelSource wide_src[STRIDE * RR_SCREEN_H * SCALE];
static RrRoadShifts road;

/* Ground rows on line 0 of both planes (block, and a mark beyond either end,
 * each cell a tile of its own), drawn with the road's backdrop. */
static RrVdp *ground_frame(void) {
    RrVdp *v = wide_planes();
    v->reg[7] = RR_WIDE_ROAD_BACKDROP;
    v->cram[RR_WIDE_ROAD_BACKDROP] = 0x0222;
    v->cram[5] = 0x00E0;
    v->cram[6] = 0x000E;
    for (int which = 0; which < 2; which++) {
        plane_pixels(v, which, 0, 331, 338, 5);
        plane_pixels(v, which, 0, 352, 672, 6);
        plane_pixels(v, which, 0, 686, 693, 5);
    }
    return v;
}

static RrWide the_wide(void) {
    RrWide w;
    memset(&w, 0, sizeof w);
    w.margin = MARGIN;
    w.lines = RR_WIDE_LINES;
    rr_wide_blocks(w.block);
    w.ground_backdrop = RR_WIDE_ROAD_BACKDROP;
    return w;
}

/* The road on line 0: its left edge at screen x = left + left_beyond, held by
 * the game at `left`; the right edge likewise. */
static void road_on_line_0(int left, int left_beyond, int right, int right_beyond) {
    memset(&road, 0, sizeof road);
    road.scale = SCALE;
    road.have[0] = 1;
    for (int r = 0; r < SCALE; r++) {
        road.a[r] = (left - 0x2A0) * 65536;
        road.b[r] = (right + 0x2A0) * 65536;
        road.beyond_a[r] = left_beyond * 65536;
        road.beyond_b[r] = right_beyond * 65536;
    }
}

static RrWideGroundCount ground_of_line_0(const RrVdp *v, const RrWide *w) {
    RrWideGroundCount count;
    memset(&count, 0, sizeof count);
    rr_vdp_render_line_wide(v, 0, SCALE, road.a, road.b, road.beyond_a, road.beyond_b, NULL, NULL, w, wide_px,
                            wide_src);
    rr_wide_ground(wide_src, &road, v, 0, SCALE, MARGIN, &count);
    return count;
}

static void test_a_right_picture_scores_zero_wrong_and_examines_pixels(void) {
    RrVdp *v = ground_frame();
    RrWide w = the_wide();
    /* The road across the middle: grass in both margins. */
    road_on_line_0(100, 0, 220, 0);
    RrWideGroundCount c = ground_of_line_0(v, &w);
    assert(c.examined == 2 * MARGIN * SCALE * SCALE && c.grass == c.examined && c.wrong == 0 && c.beyond_lines == 0);
    /* The road's left edge 20 pixels into the right margin (the game holds it at 320)... */
    road_on_line_0(320, 20, 0x180, 100);
    c = ground_of_line_0(v, &w);
    assert(c.examined == 2 * MARGIN * SCALE * SCALE && c.wrong == 0 && c.beyond_lines == 1);
    assert(c.grass == (uint64_t)(MARGIN + 20) * SCALE * SCALE && c.mark > 0 && c.road > 0);
    /* ...its right edge 20 into the left margin (held at 0)... */
    road_on_line_0(-0x160, -100, 0, -20);
    c = ground_of_line_0(v, &w);
    assert(c.examined == 2 * MARGIN * SCALE * SCALE && c.wrong == 0 && c.beyond_lines == 1);
    assert(c.grass == (uint64_t)(MARGIN + 20) * SCALE * SCALE && c.mark > 0 && c.road > 0);
    /* ...and the whole road inside the right margin, nothing held. */
    road_on_line_0(330, 0, 350, 0);
    road.a[0] = road.a[1] = (320 - 0x2A0) * 65536; /* the game holds the left edge at 320 */
    road.beyond_a[0] = road.beyond_a[1] = 10 * 65536;
    c = ground_of_line_0(v, &w);
    assert(c.wrong == 0 && c.examined == 2 * MARGIN * SCALE * SCALE);
    /* The centre of every line of a frame is the narrow picture. */
    for (int line = 0; line < RR_SCREEN_H; line++) {
        int ground = line == 0;
        rr_vdp_render_line_wide(v, line, SCALE, ground ? road.a : NULL, ground ? road.b : NULL,
                                ground ? road.beyond_a : NULL, ground ? road.beyond_b : NULL, NULL, NULL, &w,
                                &wide_px[line * SCALE * STRIDE], &wide_src[line * SCALE * STRIDE]);
        rr_vdp_render_line_fine(v, line, SCALE, ground ? road.a : NULL, ground ? road.b : NULL, NULL,
                                &narrow_px[line * SCALE * RR_SCREEN_W * SCALE]);
    }
    RrWideCentre centre;
    memset(&centre, 0, sizeof centre);
    rr_wide_centre(wide_px, narrow_px, SCALE, MARGIN, RR_WIDE_LINES, road.have, NULL, NULL, &centre);
    assert(centre.pixels == (uint64_t)RR_SCREEN_W * SCALE * RR_SCREEN_H * SCALE && centre.differing == 0);
    assert(centre.road_line + centre.object + centre.dash + centre.other == 0);
    /* A sky line's margins: nothing of a panel, and here only backdrop. */
    RrWideSky sky;
    memset(&sky, 0, sizeof sky);
    rr_wide_sky(&wide_src[10 * SCALE * STRIDE], SCALE, MARGIN, &sky);
    assert(sky.examined == 2 * MARGIN * SCALE * SCALE && sky.backdrop == sky.examined && sky.panel == 0 && sky.plane == 0);
    free(v);
}

static void test_black_margins_are_caught(void) {
    RrVdp *v = ground_frame();
    RrWide w = the_wide();
    road_on_line_0(100, 0, 220, 0);
    (void)ground_of_line_0(v, &w);
    /* The margins of line 0 drawn as panels instead. */
    const RrPixelSource panel = {RR_SRC_PANEL, 0, 0};
    for (int r = 0; r < SCALE; r++)
        for (int c = 0; c < MARGIN * SCALE; c++) wide_src[r * STRIDE + c] = wide_src[r * STRIDE + STRIDE - 1 - c] = panel;
    RrWideGroundCount count;
    memset(&count, 0, sizeof count);
    rr_wide_ground(wide_src, &road, v, 0, SCALE, MARGIN, &count);
    assert(count.examined == 2 * MARGIN * SCALE * SCALE && count.wrong == count.examined && count.grass == 0);
    RrWideSky sky;
    memset(&sky, 0, sizeof sky);
    rr_wide_sky(wide_src, SCALE, MARGIN, &sky);
    assert(sky.panel == sky.examined && sky.examined == 2 * MARGIN * SCALE * SCALE);
    /* A line from RR_WIDE_LINES down is between panels by design; the sky check says so if asked. */
    rr_vdp_render_line_wide(v, 200, SCALE, NULL, NULL, NULL, NULL, NULL, NULL, &w, wide_px, wide_src);
    memset(&sky, 0, sizeof sky);
    rr_wide_sky(wide_src, SCALE, MARGIN, &sky);
    assert(sky.panel == sky.examined);
    free(v);
}

static void test_a_wrapped_block_is_caught(void) {
    RrVdp *v = ground_frame();
    RrWide w = the_wide();
    /* The left edge held at the left limit, far off the screen; the right edge far off to the right. */
    road_on_line_0(-0x160, -100, 0x180, 100);
    RrWideGroundCount right = ground_of_line_0(v, &w);
    assert(right.wrong == 0 && right.road == right.examined && right.examined > 0);
    /* Drawn as the plane is, the block's copy 1,024 columns on shows in the right margin. */
    w.block[0].period = w.block[1].period = 0;
    RrWideGroundCount wrong = ground_of_line_0(v, &w);
    assert(wrong.wrong > 0 && wrong.examined == right.examined);
    free(v);
}

static void test_road_where_grass_belongs_is_caught(void) {
    RrVdp *v = ground_frame();
    RrWide w = the_wide();
    /* The left edge at the screen's right edge: grass through the left margin. */
    road_on_line_0(320, 0, 0x180, 0);
    RrWideGroundCount right = ground_of_line_0(v, &w);
    assert(right.wrong == 0 && right.grass >= (uint64_t)MARGIN * SCALE * SCALE);
    /* Without the continuation the plane is empty left of its block: road is drawn there. */
    w.block[0].period = w.block[1].period = 0;
    RrWideGroundCount wrong = ground_of_line_0(v, &w);
    assert(wrong.wrong > 0);
    /* And one grass pixel made road is one wrong pixel. */
    w = the_wide();
    (void)ground_of_line_0(v, &w);
    wide_src[3] = (RrPixelSource){RR_SRC_BACKDROP, RR_WIDE_ROAD_BACKDROP, 0};
    RrWideGroundCount one;
    memset(&one, 0, sizeof one);
    rr_wide_ground(wide_src, &road, v, 0, SCALE, MARGIN, &one);
    assert(one.wrong == 1 && one.grass == right.grass - 1);
    /* Grass where the road belongs, too. */
    road_on_line_0(100, 0, 0x180, 0);
    (void)ground_of_line_0(v, &w);
    wide_src[STRIDE - 1].layer = RR_SRC_PLANE_B;
    wide_src[STRIDE - 1].detail = 0x400 + 0x100 + 50; /* the tile of plane B's block at columns 400 to 407 */
    memset(&one, 0, sizeof one);
    rr_wide_ground(wide_src, &road, v, 0, SCALE, MARGIN, &one);
    assert(one.wrong == 1);
    free(v);
}

static void test_a_line_the_road_does_not_reach_is_not_examined(void) {
    RrVdp *v = ground_frame();
    RrWide w = the_wide();
    road_on_line_0(100, 0, 220, 0);
    (void)ground_of_line_0(v, &w);
    road.have[0] = 0;
    RrWideGroundCount none;
    memset(&none, 0, sizeof none);
    rr_wide_ground(wide_src, &road, v, 0, SCALE, MARGIN, &none);
    rr_wide_ground(wide_src, NULL, v, 0, SCALE, MARGIN, &none);
    assert(none.examined == 0 && none.wrong == 0);
    free(v);
}

static void test_a_changed_centre_pixel_is_classed(void) {
    static uint8_t have[RR_SCREEN_H], cover[RR_SCREEN_W * SCALE * RR_SCREEN_H * SCALE];
    static RrStrips strips;
    memset(wide_px, 0, sizeof wide_px);
    memset(narrow_px, 0, sizeof narrow_px);
    memset(have, 0, sizeof have);
    memset(cover, 0, sizeof cover);
    memset(&strips, 0, sizeof strips);
    strips.scale = SCALE;
    have[100] = have[120] = 1;
    /* Screen column 20 of output rows: 200 (a road line), 81 (under an object), 240 (a
     * dash on a road line), 21 (none of them), and 441 (below the wide lines). */
    static const int rows[5] = {200, 81, 240, 21, 441};
    for (int i = 0; i < 5; i++) wide_px[rows[i] * STRIDE + (MARGIN + 20) * SCALE] = 0xFFFFFF;
    cover[81 * RR_SCREEN_W * SCALE + 20 * SCALE] = 1;
    strips.from[240] = 20 * SCALE;
    strips.to[240] = 20 * SCALE + 1;
    RrWideCentre c;
    memset(&c, 0, sizeof c);
    rr_wide_centre(wide_px, narrow_px, SCALE, MARGIN, RR_WIDE_LINES, have, cover, &strips, &c);
    assert(c.pixels == (uint64_t)RR_SCREEN_W * SCALE * RR_SCREEN_H * SCALE && c.differing == 5);
    assert(c.road_line == 1 && c.object == 1 && c.dash == 1 && c.other == 2);
    /* A changed margin pixel is not the centre's business. */
    wide_px[200 * STRIDE] = 0xFFFFFF;
    memset(&c, 0, sizeof c);
    rr_wide_centre(wide_px, narrow_px, SCALE, MARGIN, RR_WIDE_LINES, have, NULL, NULL, &c);
    assert(c.differing == 5 && c.road_line == 2 && c.other == 3);
}

int main(void) {
    test_the_blocks_are_the_games();
    test_a_block_with_marks_is_a_ground_row();
    test_marks_in_pieces_are_still_a_ground_row();
    test_a_block_alone_is_a_ground_row();
    test_a_sparse_sky_row_inside_the_window_is_not();
    test_a_full_row_is_not();
    test_a_block_with_something_at_column_100_is_not();
    test_an_empty_row_is_not();
    test_a_right_picture_scores_zero_wrong_and_examines_pixels();
    test_black_margins_are_caught();
    test_a_wrapped_block_is_caught();
    test_road_where_grass_belongs_is_caught();
    test_a_line_the_road_does_not_reach_is_not_examined();
    test_a_changed_centre_pixel_is_classed();
    puts("all wide tests passed");
    return 0;
}
