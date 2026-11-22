/* Unit tests for the centre-line dashes, on synthetic roads.
 * Run with: make -C port test */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rr_dashes.h"

static uint8_t rom[0x30000], ram[0x10000];
static RrRoad road;
static RrDashLines dashes, in_ram;

static void put16(uint8_t *at, int value) {
    at[0] = (uint8_t)(value >> 8);
    at[1] = (uint8_t)value;
}

static void tables(void) {
    memset(rom, 0, sizeof rom);
    for (int n = 2; n < 256; n++) put16(rom + 0x15472 + 2 * n, 65536 / n);
}

/* A road of `count` points: lines falling from 152 by `fall` a point, each
 * point's edges and centre given by the three functions of its number. */
static void make_road(int count, int fall, int left0, int left_step, int right0, int right_step,
                      int centre0, int centre_step) {
    tables();
    memset(&road, 0, sizeof road);
    road.count = count;
    for (int i = 0; i < count; i++) {
        road.line[i] = (int16_t)(152 - fall * i);
        road.clipped_line[i] = road.line[i];
        road.left[i] = (int16_t)(left0 + left_step * i);
        road.right[i] = (int16_t)(right0 + right_step * i);
        road.centre[i] = (int16_t)(centre0 + centre_step * i);
    }
    road.centre_x = 160;
    road.view_bottom = 152;
    road.horizon_line = road.line[count - 1];
}

static void lines_of(uint32_t place) { rr_dashes_lines(&road, place, -1, rom, sizeof rom, &dashes); }

static void test_width_is_three_halves_of_the_road_over_64(void) {
    make_road(4, 16, -320, 0, 320, 0, 160, 0); /* 640 wide everywhere */
    lines_of(0x00040000);
    assert(dashes.ok && dashes.width[150] == 15 && dashes.width[137] == 15);
    make_road(4, 16, -10, 0, 10, 0, 160, 0);   /* 20 wide: 30 >> 6 is 0, and the game makes that 1 */
    lines_of(0x00040000);
    assert(dashes.width[150] == 1);
    make_road(4, 16, -100, 0, 101, 0, 160, 0); /* 201 wide: half is 100, 301 >> 6 = 4 */
    lines_of(0x00040000);
    assert(dashes.width[150] == 4);
}

static void test_start_follows_the_centre_line(void) {
    static int32_t centre[RR_SCREEN_H];
    static uint8_t have[RR_SCREEN_H];
    make_road(5, 16, -200, 30, 200, -30, 100, 12);
    lines_of(0x00040000);
    rr_road_lines(&road, road.centre, rom, sizeof rom, centre, have);
    int seen = 0;
    for (int line = 88; line < 152; line++) {
        assert(have[line]);
        assert(dashes.start[line] == (int16_t)(centre[line] >> 16));
        seen += dashes.start[line] != dashes.start[151];
    }
    assert(dashes.start[151] >= 100 && dashes.start[151] <= 101 && seen > 40);
}

static void test_every_other_segment_is_blank(void) {
    make_road(5, 16, -200, 0, 200, 0, 160, 0);
    lines_of(0x00040000); /* the camera in segment 4: the nearest band is painted */
    for (int line = 136; line < 152; line++) assert(dashes.width[line] > 0);
    for (int line = 120; line < 136; line++) assert(dashes.width[line] == 0 && dashes.start[line] == 160);
    for (int line = 104; line < 120; line++) assert(dashes.width[line] > 0);
    for (int line = 88; line < 104; line++) assert(dashes.width[line] == 0);
    lines_of(0x0005FFFF); /* in segment 5 they swap, wherever in the segment it is */
    for (int line = 136; line < 152; line++) assert(dashes.width[line] == 0);
    for (int line = 120; line < 136; line++) assert(dashes.width[line] > 0);
}

static void test_a_start_off_the_right_is_no_dash(void) {
    make_road(3, 16, -200, 0, 200, 0, 0x140, 0);
    lines_of(0x00040000);
    assert(dashes.start[150] == 0 && dashes.width[150] == 0);
    make_road(3, 16, -200, 0, 200, 0, 0x13F, 0);
    lines_of(0x00040000);
    assert(dashes.start[150] == 0x13F && dashes.width[150] == 9);
}

static void test_a_start_off_the_left_is_cut(void) {
    make_road(3, 16, -100, 0, 113, 0, -3, 0); /* 213 wide: width 4 */
    lines_of(0x00040000);
    assert(dashes.start[150] == 0 && dashes.width[150] == 1);
    make_road(3, 16, -100, 0, 113, 0, -9, 0);
    lines_of(0x00040000);
    assert(dashes.start[150] == 0 && dashes.width[150] == 0);
}

static void test_lines_above_the_road_have_no_dash(void) {
    make_road(4, 16, -200, 0, 200, 0, 160, 0);
    memset(&dashes, 0x55, sizeof dashes);
    lines_of(0x00040000);
    for (int line = 0; line < 104; line++) assert(dashes.start[line] == 0 && dashes.width[line] == 0);
    for (int line = 152; line < RR_SCREEN_H; line++) assert(dashes.start[line] == 0 && dashes.width[line] == 0);
}

static void test_a_crest_hides_the_dash_behind_it(void) {
    make_road(5, 16, -200, 0, 200, 0, 160, 0);
    /* Point 2 is over a crest: its line (140) is below point 1's (136), so the band
     * from point 1 to point 2 has no lines, and point 3's band is cut at 136. */
    road.line[2] = 140;
    road.clipped_line[2] = 136;
    road.clipped_line[3] = 104;
    road.clipped_line[4] = 88;
    lines_of(0x00040000);
    for (int line = 136; line < 152; line++) assert(dashes.width[line] > 0);  /* band 0, segment 4 */
    for (int line = 104; line < 136; line++) assert(dashes.width[line] > 0);  /* band 2, segment 6: band 1 has no lines */
    for (int line = 88; line < 104; line++) assert(dashes.width[line] == 0);  /* band 3, segment 7 */
}

static void test_the_walk_stops_at_its_stop_line(void) {
    make_road(5, 16, -200, 0, 200, 0, 160, 0);
    rr_dashes_lines(&road, 0x00050000, 128, rom, sizeof rom, &dashes);
    for (int line = 136; line < 152; line++) assert(dashes.width[line] == 0); /* segment 5: blank */
    /* Segment 6's band is painted; segment 7's would be blank, but the walk ended at line 128. */
    for (int line = 88; line < 136; line++) assert(dashes.width[line] > 0);
}

static void test_read_returns_the_arrays_in_ram(void) {
    memset(ram, 0, sizeof ram);
    put16(ram + 0x0302, 0xE038);
    put16(ram + 0x1CA0, 32);
    put16(ram + 0x2EDE + 2 * 120, 150);
    put16(ram + 0x309E + 2 * 120, 6);
    put16(ram + 0x0698, 0xFFFF);
    rr_dashes_read(ram, &in_ram);
    assert(in_ram.ok && in_ram.start[120] == 150 && in_ram.width[120] == 6 && in_ram.width[121] == 0);
    put16(ram + 0x0698, 3); /* the finish line is near */
    rr_dashes_read(ram, &in_ram);
    assert(!in_ram.ok);
    put16(ram + 0x0698, 0);
    put16(ram + 0x0302, 0x1234); /* not a race */
    rr_dashes_read(ram, &in_ram);
    assert(!in_ram.ok);
    /* The stop line is the word the pointer at 0xFF27BA points to. */
    put16(ram + 0x27BA, 0x00FF);
    put16(ram + 0x27BC, 0x4000);
    put16(ram + 0x4000, 77);
    assert(rr_dashes_stop_line(ram) == 77);
}

static void test_wild_road_is_refused(void) {
    struct {
        uint8_t before[64];
        RrDashLines out;
        uint8_t after[64];
    } guarded;
    make_road(4, 16, -200, 0, 200, 0, 160, 0);
    memset(&guarded, 0x5A, sizeof guarded);
    road.count = 0;
    rr_dashes_lines(&road, 0x00040000, -1, rom, sizeof rom, &guarded.out);
    assert(!guarded.out.ok);
    road.count = RR_ROAD_POINTS + 1;
    rr_dashes_lines(&road, 0x00040000, -1, rom, sizeof rom, &guarded.out);
    assert(!guarded.out.ok);
    make_road(4, 16, -200, 0, 200, 0, 160, 0);
    rr_dashes_lines(&road, 0x00040000, -1, rom, 0x100, &guarded.out); /* no table of reciprocals */
    assert(!guarded.out.ok);
    make_road(4, 16, -200, 0, 200, 0, 160, 0);
    road.line[0] = 30000;
    road.line[1] = -30000;
    road.clipped_line[0] = 30000;
    road.view_bottom = 30000;
    rr_dashes_lines(&road, 0x00040000, -1, rom, sizeof rom, &guarded.out);
    for (int i = 0; i < 64; i++) assert(guarded.before[i] == 0x5A && guarded.after[i] == 0x5A);
}

/* ---- strips of the enlarged picture ---- */

static RrFineRoad fine_road;
static RrStrips strips;

/* The fine road with the same points as `road`, without fractions. */
static void fine_of_road(void) {
    memset(&fine_road, 0, sizeof fine_road);
    fine_road.count = road.count;
    for (int i = 0; i < road.count; i++) {
        fine_road.line[i] = road.line[i] * 65536;
        fine_road.clipped_line[i] = road.clipped_line[i] * 65536;
        fine_road.left[i] = road.left[i] * 65536;
        fine_road.right[i] = road.right[i] * 65536;
        fine_road.centre[i] = (road.centre[i] - road.centre_x) * 65536;
    }
    fine_road.centre_x = road.centre_x;
    fine_road.view_bottom = road.view_bottom;
}

static void test_strips_at_scale_1_are_the_whole_pixel_dashes(void) {
    make_road(5, 16, -200, 30, 200, -30, 100, 12);
    fine_of_road();
    lines_of(0x00040000);
    rr_dashes_strips(&fine_road, 0x00040000, NULL, 1, &strips);
    assert(strips.scale == 1 && strips.colour == 0x3C);
    int painted = 0, exact = 0;
    for (int line = 0; line < RR_SCREEN_H; line++) {
        int wide = strips.to[line] > strips.from[line] ? strips.to[line] - strips.from[line] : 0;
        assert((wide > 0) == (dashes.width[line] > 0)); /* the same lines are painted */
        if (wide == 0) continue;
        painted++;
        /* The game's per-line division is by a table of reciprocals; the strip's is exact. */
        assert(strips.from[line] - dashes.start[line] >= -1 && strips.from[line] - dashes.start[line] <= 1);
        assert(wide - dashes.width[line] >= -1 && wide - dashes.width[line] <= 1);
        exact += strips.from[line] == dashes.start[line] && wide == dashes.width[line];
    }
    /* (Where the centre falls between pixels the strip may be a pixel wider than the game's.) */
    assert(painted == 32 && exact >= 16);
}

static void test_strips_move_by_rows(void) {
    make_road(3, 32, -200, 0, 200, 0, 100, 64); /* the centre moves two pixels a line */
    fine_of_road();
    rr_dashes_strips(&fine_road, 0x00040000, NULL, 4, &strips);
    for (int row = 121 * 4; row < 150 * 4; row++) {
        assert(strips.to[row] > strips.from[row]);
        assert(strips.from[row + 1] - strips.from[row] == -2); /* two output pixels a row, not eight a line */
    }
    /* 400 wide: 9.375 pixels, so 37 or 38 output pixels at scale 4. */
    assert(strips.to[600] - strips.from[600] >= 37 && strips.to[600] - strips.from[600] <= 38);
}

static void test_strips_are_cut_at_the_screen_edges(void) {
    make_road(3, 16, -200, 0, 200, 0, -3, 0);
    fine_of_road();
    rr_dashes_strips(&fine_road, 0x00040000, NULL, 2, &strips);
    assert(strips.from[300] == 0 && strips.to[300] == 12); /* 9.375 wide from -3: to 6.375 */
    make_road(3, 16, -200, 0, 200, 0, 316, 0);
    fine_of_road();
    rr_dashes_strips(&fine_road, 0x00040000, NULL, 2, &strips);
    assert(strips.from[300] == 632 && strips.to[300] == 640);
    make_road(3, 16, -200, 0, 200, 0, 330, 0);
    fine_of_road();
    rr_dashes_strips(&fine_road, 0x00040000, NULL, 2, &strips);
    for (int row = 0; row < RR_SCREEN_H * 2; row++) assert(strips.to[row] <= strips.from[row]);
    /* Rows outside the road, and a wild road, have none. */
    make_road(3, 16, -200, 0, 200, 0, 160, 0);
    fine_of_road();
    rr_dashes_strips(&fine_road, 0x00040000, NULL, 2, &strips);
    for (int row = 0; row < 120 * 2; row++) assert(strips.to[row] <= strips.from[row]);
    fine_road.count = 99;
    rr_dashes_strips(&fine_road, 0x00040000, NULL, 2, &strips);
    for (int row = 0; row < RR_SCREEN_H * 2; row++) assert(strips.to[row] <= strips.from[row]);
}

static void strips_at(int centre, int margin) {
    make_road(3, 16, -200, 0, 200, 0, centre, 0);
    fine_of_road();
    rr_dashes_strips_wide(&fine_road, 0x00040000, NULL, 2, margin, &strips);
}

static void test_wide_strips_run_into_the_margins(void) {
    /* The dash is 9.375 pixels wide from the road's centre; scale 2, a margin of 39. */
    strips_at(-3, 39);
    assert(strips.from[300] == -6 && strips.to[300] == 12);
    strips_at(316, 39);
    assert(strips.from[300] == 632 && strips.to[300] == 650);
    strips_at(330, 39); /* wholly in the right margin: no strip at all in the narrow picture */
    assert(strips.from[300] == 660 && strips.to[300] == 678);
    strips_at(355, 39); /* cut at the wide picture's right side, x = 359 */
    assert(strips.from[300] == 710 && strips.to[300] == 718);
    strips_at(370, 39);
    assert(strips.to[300] <= strips.from[300]);
    strips_at(-45, 39); /* cut at its left side, x = -39 */
    assert(strips.from[300] == -78 && strips.to[300] == -72);
    strips_at(-60, 39);
    assert(strips.to[300] <= strips.from[300]);
    /* A margin of 0 is rr_dashes_strips. */
    static RrStrips narrow;
    strips_at(316, 0);
    rr_dashes_strips(&fine_road, 0x00040000, NULL, 2, &narrow);
    assert(memcmp(&strips, &narrow, sizeof narrow) == 0 && narrow.to[300] == 640);
}

static void test_wide_strips_equal_the_narrow_ones_in_the_centre(void) {
    static RrStrips narrow;
    static const int centres[9] = {-60, -45, -3, 0, 160, 316, 330, 355, 370};
    for (int i = 0; i < 9; i++) {
        strips_at(centres[i], 39);
        rr_dashes_strips(&fine_road, 0x00040000, NULL, 2, &narrow);
        for (int row = 0; row < RR_SCREEN_H * 2; row++) {
            int from = strips.from[row] < 0 ? 0 : strips.from[row], to = strips.to[row] > 640 ? 640 : strips.to[row];
            if (to <= from) assert(narrow.to[row] <= narrow.from[row]);
            else assert(narrow.from[row] == from && narrow.to[row] == to);
        }
    }
}

static void test_strips_skip_every_other_segment(void) {
    make_road(5, 16, -200, 0, 200, 0, 160, 0);
    fine_of_road();
    rr_dashes_strips(&fine_road, 0x00050000, NULL, 3, &strips);
    assert(strips.to[150 * 3] <= strips.from[150 * 3]);  /* segment 5 */
    assert(strips.to[130 * 3] > strips.from[130 * 3]);   /* segment 6 */
    assert(strips.to[110 * 3] <= strips.from[110 * 3]);  /* segment 7 */
}

/* Close to the camera the game does not let the second point hide anything
 * (its clipped line stays at the bottom of the view), so the second band's run
 * is drawn over the first band's lines too. Those lines are still the first
 * band's segment when it comes to painting. */
static void test_the_nearest_band_keeps_its_own_segment(void) {
    make_road(5, 16, -200, 0, 200, 0, 160, 0);
    road.clipped_line[1] = 152;
    fine_of_road();
    lines_of(0x00040000);
    rr_dashes_strips(&fine_road, 0x00040000, NULL, 2, &strips);
    for (int line = 136; line < 152; line++) {
        assert(dashes.width[line] > 0);
        assert(strips.to[2 * line] > strips.from[2 * line] && strips.to[2 * line + 1] > strips.from[2 * line + 1]);
    }
    for (int line = 120; line < 136; line++) {
        assert(dashes.width[line] == 0);
        assert(strips.to[2 * line] <= strips.from[2 * line]);
    }
}

static void entry(uint8_t *table, int index, int attr) {
    table[8 * index + 4] = (uint8_t)(attr >> 8);
    table[8 * index + 5] = (uint8_t)attr;
}

static void test_mark_takes_only_dash_entries(void) {
    static uint8_t table[640], skip[RR_SPRITES_MAX];
    memset(table, 0, sizeof table);
    entry(table, 0, 0x6640); /* an object that happens to use colour set 3 */
    entry(table, 1, 0x2641);
    entry(table, 2, 0x6650); /* a dash */
    entry(table, 3, 0xE651); /* colour set 3 but high priority: not a dash */
    entry(table, 4, 0x6652); /* a dash */
    memset(skip, 0x55, sizeof skip);
    rr_dashes_mark(table, 2, skip);
    assert(!skip[0] && !skip[1] && skip[2] && !skip[3] && skip[4]);
    for (int i = 5; i < RR_SPRITES_MAX; i++) assert(!skip[i]);
    rr_dashes_mark(table, 200, skip); /* a first entry past the table */
    for (int i = 0; i < RR_SPRITES_MAX; i++) assert(!skip[i]);
}

/* ---- which roads have their dashes proven ---- */

static RrRoadTracker tracker;

static void test_note_marks_the_road_whose_dashes_the_model_makes(void) {
    static RrDashLines made;
    make_road(5, 16, -200, 30, 200, -30, 100, 12);
    memset(ram, 0, sizeof ram);
    put16(ram + 0x0302, 0xE038);
    put16(ram + 0x1CA0, road.count);
    put16(ram + 0x0A0A, 4);
    put16(ram + 0xF2B2, 152);
    put16(ram + 0xF2BE, 160);
    for (int i = 0; i < road.count; i++) {
        put16(ram + 0x1E20 + 2 * i, road.line[i]);
        put16(ram + 0xD2B8 + 2 * i, road.clipped_line[i]);
        put16(ram + 0x1E84 + 2 * i, road.left[i]);
        put16(ram + 0x1F4C + 2 * i, road.right[i]);
        put16(ram + 0x1DBC + 2 * i, road.centre[i]);
    }
    put16(ram + 0x27BC, 0x7000); /* the stop line: the word at 0xFF7000 */
    put16(ram + 0x7000, 0xFFFF);
    rr_road_tracker_reset(&tracker);
    assert(rr_dashes_proven(&tracker) == 0);
    rr_road_tracker_note(&tracker, ram, rom, sizeof rom);
    rr_dashes_note(&tracker, ram, rom, sizeof rom); /* the arrays in RAM are all zero: not what the model makes */
    assert(tracker.dashes_ok[tracker.newest] == 0);
    assert(rr_road_read(ram, &road) == 1);
    rr_dashes_lines(&road, 0x00040000, -1, rom, sizeof rom, &made);
    for (int line = 0; line < RR_SCREEN_H; line++) {
        put16(ram + 0x2EDE + 2 * line, made.start[line]);
        put16(ram + 0x309E + 2 * line, made.width[line]);
    }
    rr_dashes_note(&tracker, ram, rom, sizeof rom);
    assert(tracker.dashes_ok[tracker.newest] == 1);
    /* One line different, the finish line near, or road points that are not the noted road's: not proven. */
    put16(ram + 0x309E + 2 * 140, made.width[140] + 1);
    rr_dashes_note(&tracker, ram, rom, sizeof rom);
    assert(tracker.dashes_ok[tracker.newest] == 0);
    put16(ram + 0x309E + 2 * 140, made.width[140]);
    put16(ram + 0x0698, 2);
    rr_dashes_note(&tracker, ram, rom, sizeof rom);
    assert(tracker.dashes_ok[tracker.newest] == 0);
    put16(ram + 0x0698, 0);
    put16(ram + 0x1E84, road.left[0] - 1);
    rr_dashes_note(&tracker, ram, rom, sizeof rom);
    assert(tracker.dashes_ok[tracker.newest] == 0);
    /* A newly noted road starts unproven. */
    put16(ram + 0x1E84, road.left[0]);
    rr_dashes_note(&tracker, ram, rom, sizeof rom);
    assert(tracker.dashes_ok[tracker.newest] == 1);
    rr_road_tracker_note(&tracker, ram, rom, sizeof rom);
    assert(tracker.dashes_ok[tracker.newest] == 0);
}

/* ---- what the final review found ---- */

static void test_strips_keep_to_the_lines_the_road_has(void) {
    static uint8_t have[RR_SCREEN_H];
    make_road(3, 16, -200, 0, 200, 0, 160, 0);
    fine_of_road();
    memset(have, 1, sizeof have);
    have[120] = 0; /* the road's top line as the fine points have it, but not one of the game's road lines */
    have[140] = 0;
    rr_dashes_strips(&fine_road, 0x00040000, have, 2, &strips);
    for (int row = 240; row < 304; row++) {
        int line = row / 2;
        assert((strips.to[row] > strips.from[row]) == (line != 120 && line != 140 && line >= 136));
    }
    rr_dashes_strips(&fine_road, 0x00040000, NULL, 2, &strips);
    assert(strips.to[280] > strips.from[280] && strips.to[281] > strips.from[281]);
}

static void test_a_reused_place_starts_unproven(void) {
    test_note_marks_the_road_whose_dashes_the_model_makes(); /* leaves a noted road in `ram`, unproven */
    rr_dashes_note(&tracker, ram, rom, sizeof rom);
    /* (the arrays in RAM are that road's: proven) */
    int proven_slot = tracker.newest;
    assert(tracker.dashes_ok[proven_slot] == 1);
    for (int i = 1; i <= RR_ROAD_RECENT; i++) {
        put16(ram + 0x1E84, -300 - i); /* another road each time */
        rr_road_tracker_note(&tracker, ram, rom, sizeof rom);
        assert(tracker.dashes_ok[tracker.newest] == 0);
    }
    assert(tracker.newest == proven_slot); /* the place has come round again */
}

static void test_proven_needs_a_chosen_road_with_fine_points(void) {
    memset(&tracker, 0, sizeof tracker);
    tracker.count = 1;
    tracker.dashes_ok[0] = 1;
    tracker.has_fine[0] = 1;
    tracker.have_chosen = 1;
    assert(rr_dashes_proven(&tracker) == 1);
    tracker.has_fine[0] = 0;
    assert(rr_dashes_proven(&tracker) == 0);
    tracker.has_fine[0] = 1;
    tracker.have_chosen = 0;
    assert(rr_dashes_proven(&tracker) == 0);
    tracker.have_chosen = 1;
    tracker.dashes_ok[0] = 0;
    assert(rr_dashes_proven(&tracker) == 0);
}

int main(void) {
    test_strips_keep_to_the_lines_the_road_has();
    test_a_reused_place_starts_unproven();
    test_proven_needs_a_chosen_road_with_fine_points();
    test_strips_at_scale_1_are_the_whole_pixel_dashes();
    test_strips_move_by_rows();
    test_strips_are_cut_at_the_screen_edges();
    test_strips_skip_every_other_segment();
    test_wide_strips_run_into_the_margins();
    test_wide_strips_equal_the_narrow_ones_in_the_centre();
    test_the_nearest_band_keeps_its_own_segment();
    test_mark_takes_only_dash_entries();
    test_note_marks_the_road_whose_dashes_the_model_makes();
    test_width_is_three_halves_of_the_road_over_64();
    test_start_follows_the_centre_line();
    test_every_other_segment_is_blank();
    test_a_start_off_the_right_is_no_dash();
    test_a_start_off_the_left_is_cut();
    test_lines_above_the_road_have_no_dash();
    test_a_crest_hides_the_dash_behind_it();
    test_the_walk_stops_at_its_stop_line();
    test_read_returns_the_arrays_in_ram();
    test_wild_road_is_refused();
    puts("all dashes tests passed");
    return 0;
}
