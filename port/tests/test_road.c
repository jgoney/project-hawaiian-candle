/* Unit tests for the road shifts worked out from the game's road points.
 * Run with: make -C port test */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rr_road.h"

static uint8_t ram[0x10000];
static uint8_t rom[0x30000];
static RrRoadShifts shifts;

static void put16(uint8_t *at, int value) {
    at[0] = (uint8_t)(value >> 8);
    at[1] = (uint8_t)value;
}

/* A one-player race with three road points: one below the screen, two on it. */
static void race(int near_left) {
    static const int line[3] = {200, 120, 100}, clipped[3] = {152, 120, 100};
    int left[3] = {near_left, -100, -20};
    static const int right[3] = {300, 100, 20};
    memset(ram, 0, sizeof ram);
    memset(rom, 0, sizeof rom);
    for (int n = 2; n < 256; n++) put16(rom + 0x15472 + 2 * n, 65536 / n);
    put16(ram + 0x0302, 0xE038); /* the race's frame callback */
    put16(ram + 0x1CA0, 3);
    put16(ram + 0xF2B2, 152);    /* bottom of the 3D view */
    put16(ram + 0xF2BE, 160);    /* screen centre */
    for (int i = 0; i < 3; i++) {
        put16(ram + 0x1E20 + 2 * i, line[i]);
        put16(ram + 0xD2B8 + 2 * i, clipped[i]);
        put16(ram + 0x1E84 + 2 * i, left[i]);
        put16(ram + 0x1F4C + 2 * i, right[i]);
    }
}

static RrRoad road;

static void test_row_0_equals_the_integer_table(void) {
    race(-300);
    assert(rr_road_read(ram, &road) == 1);
    assert(road.count == 3 && road.horizon_line == 100 && road.view_bottom == 152);
    rr_road_shifts(&road, rom, sizeof rom, 1, &shifts);
    /* Between the two far points the left edge moves 80 in 20 lines: by the game's
     * rule, 65536/20 = 3276 per unit, so -262080/65536 a line from -20 at line 100. Line
     * 101 is -23.999..., which the game rounds down to -24. Plane A's shift is the edge
     * plus the screen centre less 0x2A0; plane B's is the right edge the same way plus
     * 0x2A0. */
    assert(shifts.have[100] && shifts.have[101] && shifts.have[151]);
    assert((shifts.a[100] >> 16) == -20 + 160 - 0x2A0 && (shifts.b[100] >> 16) == 20 + 160 + 0x2A0);
    assert((shifts.a[101] >> 16) == -24 + 160 - 0x2A0 && (shifts.b[101] >> 16) == 23 + 160 + 0x2A0);
    /* Between the near two: 200 in 80 lines, 65536/80 = 819, so -163800 a line from -100
     * at line 120; line 121 is -102.49..., rounded down to -103. */
    assert((shifts.a[121] >> 16) == -103 + 160 - 0x2A0 && (shifts.b[121] >> 16) == 102 + 160 + 0x2A0);
}

static void test_rows_continue_the_slope(void) {
    race(-300);
    rr_road_read(ram, &road);
    rr_road_shifts(&road, rom, sizeof rom, 4, &shifts);
    assert(shifts.scale == 4);
    for (int r = 0; r < 4; r++) {
        assert(shifts.a[121 * 4 + r] - shifts.a[121 * 4] == -163800 * r / 4);
        assert(shifts.b[121 * 4 + r] - shifts.b[121 * 4] == 163800 * r / 4);
    }
    /* Row 0 of each line is still the game's own value. */
    assert((shifts.a[121 * 4] >> 16) == -103 + 160 - 0x2A0);
}

static void test_clamped_edge_stays_clamped_on_every_row(void) {
    race(-2000); /* the near left edge is far off the screen */
    rr_road_read(ram, &road);
    rr_road_shifts(&road, rom, sizeof rom, 4, &shifts);
    for (int r = 0; r < 4; r++) assert(shifts.a[151 * 4 + r] == (-0x160 - 0x2A0) * 65536);
    assert(shifts.a[121 * 4 + 1] != shifts.a[121 * 4]); /* further up it is not clamped */
}

static void test_line_beyond_the_last_point_has_no_shift(void) {
    race(-300);
    rr_road_read(ram, &road);
    rr_road_shifts(&road, rom, sizeof rom, 2, &shifts);
    assert(!shifts.have[99] && !shifts.have[0] && !shifts.have[152] && !shifts.have[223]);
}

static void test_not_a_race_is_refused(void) {
    race(-300);
    put16(ram + 0x0302, 0x1234); /* some other frame callback: a menu */
    assert(rr_road_read(ram, &road) == 0);
    race(-300);
    put16(ram + 0x0304, 1); /* two players: the split screen is not read */
    assert(rr_road_read(ram, &road) == 0);
    race(-300);
    put16(ram + 0x1CA0, 33); /* more points than there is room for */
    assert(rr_road_read(ram, &road) == 0);
}

static void test_a_rom_without_the_table_gives_no_shifts(void) {
    race(-300);
    rr_road_read(ram, &road);
    rr_road_shifts(&road, rom, 0x400, 4, &shifts);
    for (int line = 0; line < RR_SCREEN_H; line++) assert(!shifts.have[line]);
}

/* ---- the projection ---- */

static uint16_t table_scale(int distance) { return (uint16_t)(0x10000L * 64 / (distance + 64) - 1); }

static void perspective_table(void) {
    memset(rom, 0, sizeof rom);
    for (int n = 2; n < 256; n++) put16(rom + 0x15472 + 2 * n, 65536 / n);
    for (int d = 0; d < 16384; d++) put16(rom + 0x27C84 + 2 * d, table_scale(d));
}

/* A one-player race about to project: camera 0x4000 into segment 5, a bend and a rise ahead. */
static void race_inputs(int sideways, int height) {
    memset(ram, 0, sizeof ram);
    put16(ram + 0x0302, 0xE038);
    put16(ram + 0x1CA0, 32);
    put16(ram + 0x0A0A, 5);
    put16(ram + 0x0A0C, 0x4000);
    put16(ram + 0x0A02, sideways);
    put16(ram + 0x0A08, height);
    put16(ram + 0xF2B0, 0);
    put16(ram + 0xF2B2, 152);
    put16(ram + 0xF2BC, 60);
    put16(ram + 0xF2BE, 160);
    for (int i = 0; i < 256; i++) {
        ram[0xD828 + 2 * i] = (uint8_t)(i >= 8 && i < 20 ? -3 : i >= 24 ? 2 : 0); /* curve */
        ram[0xD828 + 2 * i + 1] = (uint8_t)(i >= 10 && i < 16 ? 4 : i >= 16 && i < 22 ? -5 : 0); /* hill */
    }
}

static int word(long v) { return (int16_t)(uint16_t)v; }
static int scaled(int value, int scale) {
    if (scale == 0) return value;
    return value < 0 ? -(int)(((long)-value * scale) >> 16) : (int)(((long)value * scale) >> 16);
}

/* The game's projection as src/rr2port/scene.py writes it, in plain whole numbers. */
static void reference_project(RrRoad *out) {
    int count = 32, fraction = 0x4000, at = 5 * 2;
    int curve = (int8_t)ram[0xD828 + at++];
    long product = (long)(curve < 0 ? -curve : curve) * fraction;
    int done = (int)(product >> 16) + ((product & 0x8000) ? 1 : 0);
    if (curve < 0) done = -done;
    int step = word(curve - done);
    int sideways = word(-(int16_t)((ram[0x0A02] << 8) | ram[0x0A03]));
    int hill = (int8_t)ram[0xD828 + at++];
    int rise = (int)(((long)(hill < 0 ? -hill : hill) * fraction) >> 16);
    rise = word(-(hill < 0 ? -rise : rise));
    int distance = word(-(fraction >> 8));
    int height = (int16_t)((ram[0x0A08] << 8) | ram[0x0A09]);
    int lowest = 152;
    out->count = count;
    for (int i = 0; i < count; i++) {
        int scale = distance > 0 ? table_scale(distance) : 0;
        int under = word(height - rise);
        int line = word(scaled(under, scale) + 60);
        if (under < 0 && line < 0) line = 0;
        if ((uint16_t)line < (uint16_t)lowest && distance >= 0x100) lowest = line;
        int x = scaled(sideways, scale), half = scaled(0xE0, scale);
        out->line[i] = (int16_t)line;
        out->clipped_line[i] = (int16_t)lowest;
        out->left[i] = (int16_t)word(x - half);
        out->right[i] = (int16_t)word(x + half);
        distance = word(distance + 0x100);
        curve = (int8_t)ram[0xD828 + at++];
        sideways = word(sideways + step);
        step = word(step + curve);
        rise = word(rise + (int8_t)ram[0xD828 + at++]);
    }
}

static RrRoadInputs inputs;
static RrFineRoad fine;
static RrRoad whole, expected;

static void test_dropping_the_remainder_gives_the_whole_points(void) {
    static const int cases[4][2] = {{0, 96}, {-150, 96}, {220, 40}, {-30, -20}};
    perspective_table();
    for (int k = 0; k < 4; k++) {
        race_inputs(cases[k][0], cases[k][1]);
        assert(rr_road_read_inputs(ram, &inputs) == 1);
        assert(rr_road_project(&inputs, rom, sizeof rom, &fine, &whole) == 1);
        reference_project(&expected);
        assert(whole.count == 32 && whole.centre_x == 160 && whole.view_bottom == 152);
        for (int i = 0; i < 32; i++) {
            assert(whole.line[i] == expected.line[i] && whole.clipped_line[i] == expected.clipped_line[i]);
            assert(whole.left[i] == expected.left[i] && whole.right[i] == expected.right[i]);
        }
    }
}

static long magnitude(long v) { return v < 0 ? -v : v; }

static void test_the_remainder_is_small(void) {
    perspective_table();
    race_inputs(-150, 96);
    rr_road_read_inputs(ram, &inputs);
    rr_road_project(&inputs, rom, sizeof rom, &fine, &whole);
    int any = 0;
    for (int i = 0; i < 32; i++) {
        /* A line is one product: less than a pixel. An edge is two (position and half
         * width), each dropped separately by the game: less than two. */
        assert(magnitude((long)fine.line[i] - whole.line[i] * 65536L) < 0x10000);
        assert(magnitude((long)fine.left[i] - whole.left[i] * 65536L) < 0x20000);
        assert(magnitude((long)fine.right[i] - whole.right[i] * 65536L) < 0x20000);
        any |= (fine.left[i] & 0xFFFF) != 0;
    }
    assert(any); /* there are remainders to keep */
}

static void test_a_line_above_the_view_is_clamped_without_a_remainder(void) {
    perspective_table();
    race_inputs(0, -30000); /* the camera far below the road: far points project above line 0 */
    rr_road_read_inputs(ram, &inputs);
    rr_road_project(&inputs, rom, sizeof rom, &fine, &whole);
    assert(whole.line[20] == 0 && fine.line[20] == 0);
}

static void test_inputs_are_refused_outside_a_race_and_with_a_short_rom(void) {
    perspective_table();
    race_inputs(0, 96);
    assert(rr_road_read_inputs(ram, &inputs) == 1);
    assert(rr_road_project(&inputs, rom, 0x27C84 + 100, &fine, &whole) == 0);
    put16(ram + 0x0302, 0x1234);
    assert(rr_road_read_inputs(ram, &inputs) == 0);
    race_inputs(0, 96);
    put16(ram + 0x0304, 1);
    assert(rr_road_read_inputs(ram, &inputs) == 0);
}

/* ---- shifts from the fine points ---- */

static RrRoadShifts whole_shifts, fine_shifts;
static RrVdp vdp;

static long deviation_from_chord(const int32_t *a, int first_row, int last_row) {
    long worst = 0;
    for (int r = first_row; r <= last_row; r++) {
        long chord = a[first_row] + (long)((int64_t)(a[last_row] - a[first_row]) * (r - first_row) / (last_row - first_row));
        if (magnitude(a[r] - chord) > worst) worst = magnitude(a[r] - chord);
    }
    return worst;
}

static void test_a_straight_road_has_straight_edges(void) {
    perspective_table();
    race_inputs(-72, 60);
    memset(ram + 0xD828, 0, 512); /* flat and straight */
    rr_road_read_inputs(ram, &inputs);
    rr_road_project(&inputs, rom, sizeof rom, &fine, &whole);
    rr_road_shifts(&whole, rom, sizeof rom, 4, &whole_shifts);
    rr_road_shifts_fine(&fine, &whole_shifts, &fine_shifts);
    /* The left edge from just below the third point to just above the first: it passes
     * the second point on the way, where the whole-pixel road has a kink. */
    int first = (whole.line[3] + 1) * 4, last = (whole.line[1] - 1) * 4;
    assert(last - first > 30 && whole_shifts.have[whole.line[3] + 1] && whole_shifts.have[whole.line[1] - 1]);
    assert(deviation_from_chord(fine_shifts.a, first, last) < 0x4000);   /* a quarter pixel: one output pixel */
    assert(deviation_from_chord(whole_shifts.a, first, last) >= 0x4000); /* the wobble */
    assert(deviation_from_chord(fine_shifts.b, first, last) < 0x4000);
    assert(deviation_from_chord(whole_shifts.b, first, last) >= 0x4000);
}

static void test_a_crest_hides_the_road_behind_it(void) {
    static const int line[4] = {200, 120, 130, 100}, clipped[4] = {152, 120, 120, 100};
    static const int left[4] = {-300, -100, -60, -20};
    memset(&fine, 0, sizeof fine);
    memset(&whole, 0, sizeof whole);
    perspective_table();
    fine.count = whole.count = 4;
    fine.centre_x = whole.centre_x = 160;
    fine.view_bottom = whole.view_bottom = 152;
    whole.horizon_line = 100;
    for (int i = 0; i < 4; i++) {
        whole.line[i] = (int16_t)line[i];
        whole.clipped_line[i] = (int16_t)clipped[i];
        whole.left[i] = (int16_t)left[i];
        whole.right[i] = (int16_t)-left[i];
        fine.line[i] = line[i] * 65536 + 0x8000; /* every fine value half a pixel on */
        fine.clipped_line[i] = clipped[i] * 65536 + 0x8000;
        fine.left[i] = left[i] * 65536 + 0x8000;
        fine.right[i] = -left[i] * 65536 + 0x8000;
    }
    rr_road_shifts(&whole, rom, sizeof rom, 2, &whole_shifts);
    rr_road_shifts_fine(&fine, &whole_shifts, &fine_shifts);
    /* Lines 100 to 119 show the run from the farthest point towards the hidden one (which
     * is 30 lines below it), cut off at the crest. Line 110, second row, is 10 lines down
     * that run: a third of the way from -20 to -60, half a pixel on. */
    assert(fine_shifts.have[110] && fine_shifts.have[119] && fine_shifts.have[120]);
    long want = (-20 * 65536L + 0x8000) + (-40 * 65536L) * 10 / 30 + (160 - 0x2A0) * 65536L;
    assert(magnitude(fine_shifts.a[110 * 2 + 1] - want) < 16);
    /* From line 121 on it is the nearest run, from the crest point (120.5, -99.5). */
    long near = (-100 * 65536L + 0x8000) + (-200 * 65536L) * 10 / 80 + (160 - 0x2A0) * 65536L;
    assert(magnitude(fine_shifts.a[130 * 2 + 1] - near) < 16);
}

/* The game extends the run between its second and third points down to the bottom of the
 * view. If those two points are almost on the same line, the extension is enormous: it
 * must run off to the proper side, not wrap round to the other. And if the third point
 * is not above the second at all there is no run to extend: those rows keep the
 * whole-pixel road. */
static void hand_made_road(long second_line, long third_line) {
    static const int clipped[4] = {152, 152, 100, 90};
    static const int left[4] = {-300, -100, -60, -20};
    long line[4] = {200 * 65536L, second_line, third_line, 90 * 65536L};
    memset(&fine, 0, sizeof fine);
    memset(&whole, 0, sizeof whole);
    perspective_table();
    fine.count = whole.count = 4;
    fine.centre_x = whole.centre_x = 160;
    fine.view_bottom = whole.view_bottom = 152;
    whole.horizon_line = 90;
    for (int i = 0; i < 4; i++) {
        whole.line[i] = (int16_t)(line[i] >> 16);
        whole.clipped_line[i] = (int16_t)clipped[i];
        whole.left[i] = (int16_t)left[i];
        whole.right[i] = (int16_t)-left[i];
        fine.line[i] = (int32_t)line[i];
        fine.clipped_line[i] = clipped[i] * 65536;
        fine.left[i] = left[i] * 65536;
        fine.right[i] = -left[i] * 65536;
    }
    rr_road_shifts(&whole, rom, sizeof rom, 4, &whole_shifts);
    rr_road_shifts_fine(&fine, &whole_shifts, &fine_shifts);
}

static void test_a_run_extended_enormously_goes_off_the_proper_side(void) {
    hand_made_road(100 * 65536L + 0x60, 100 * 65536L); /* a tiny part of a line apart, 40 px across */
    assert(whole_shifts.have[140]);
    /* The left edge moves left going down, hundreds of thousands of pixels a line: it is at the left
     * limit, and the right edge at the right limit. */
    for (int r = 0; r < 4; r++) {
        assert(fine_shifts.a[140 * 4 + r] == (-0x160 - 0x2A0) * 65536);
        assert(fine_shifts.b[140 * 4 + r] == (0x180 + 0x2A0) * 65536);
    }
}

static void test_a_run_with_no_height_keeps_the_whole_pixel_road(void) {
    hand_made_road(100 * 65536L, 100 * 65536L + 0x8000); /* the third point is below the second */
    assert(whole_shifts.have[140]);
    for (int r = 0; r < 4; r++) {
        assert(fine_shifts.a[140 * 4 + r] == whole_shifts.a[140 * 4 + r]);
        assert(fine_shifts.b[140 * 4 + r] == whole_shifts.b[140 * 4 + r]);
    }
}

static void test_clamped_edge_stays_clamped_on_every_row_fine(void) {
    perspective_table();
    race_inputs(1500, 96); /* far to the right of the road: its left edge is off the screen's left */
    memset(ram + 0xD828, 0, 512);
    rr_road_read_inputs(ram, &inputs);
    rr_road_project(&inputs, rom, sizeof rom, &fine, &whole);
    rr_road_shifts(&whole, rom, sizeof rom, 4, &whole_shifts);
    rr_road_shifts_fine(&fine, &whole_shifts, &fine_shifts);
    assert(whole_shifts.have[150]);
    for (int r = 0; r < 4; r++) assert(fine_shifts.a[150 * 4 + r] == (-0x160 - 0x2A0) * 65536);
}

/* Two points two lines apart, each edge moving one pixel a line outwards going
 * down: the left edge is `left` at line 100, the right edge `right`. */
static void edge_road(int left, int right) {
    memset(&fine, 0, sizeof fine);
    memset(&whole, 0, sizeof whole);
    perspective_table();
    fine.count = whole.count = 2;
    fine.centre_x = whole.centre_x = 160;
    fine.view_bottom = whole.view_bottom = 152;
    whole.horizon_line = 100;
    for (int i = 0; i < 2; i++) {
        int line = i == 0 ? 102 : 100, out = i == 0 ? 2 : 0;
        whole.line[i] = whole.clipped_line[i] = (int16_t)line;
        whole.left[i] = (int16_t)(left - out);
        whole.right[i] = (int16_t)(right + out);
        fine.line[i] = fine.clipped_line[i] = line * 65536;
        fine.left[i] = (left - out) * 65536;
        fine.right[i] = (right + out) * 65536;
    }
    rr_road_shifts(&whole, rom, sizeof rom, 4, &whole_shifts);
    rr_road_shifts_fine(&fine, &whole_shifts, &fine_shifts);
}

static void test_beyond_is_zero_inside_the_clamps(void) {
    /* Plane A's limits are -0x160 and 0x140 for edge + centre, plane B's 0 and 0x180:
     * both edges exactly on a limit at line 101 row 0, inside it above. */
    edge_road(-0x160 - 160 + 1, 0x180 - 160 - 1);
    assert(whole_shifts.have[100] && whole_shifts.have[101] && !whole_shifts.have[102]);
    for (int at = 100 * 4; at <= 101 * 4; at++) {
        assert(whole_shifts.beyond_a[at] == 0 && whole_shifts.beyond_b[at] == 0);
        assert(fine_shifts.beyond_a[at] == 0 && fine_shifts.beyond_b[at] == 0);
    }
    /* A line the road does not reach has none either. */
    assert(whole_shifts.beyond_a[50 * 4] == 0 && fine_shifts.beyond_b[223 * 4 + 3] == 0);
}

static void test_beyond_is_the_excess_on_every_row(void) {
    /* 30 pixels past the limits at line 100, a quarter of a pixel more on every row. */
    edge_road(-0x160 - 160 - 30, 0x180 - 160 + 30);
    for (int i = 0; i < 2; i++) {
        const RrRoadShifts *sh = i == 0 ? &whole_shifts : &fine_shifts;
        for (int r = 0; r < 8; r++) {
            assert(sh->beyond_a[100 * 4 + r] == -(0x1E0000 + r * 0x4000));
            assert(sh->beyond_b[100 * 4 + r] == 0x1E0000 + r * 0x4000);
            /* The shifts themselves stay at the limits. */
            assert(sh->a[100 * 4 + r] == (-0x160 - 0x2A0) * 65536 && sh->b[100 * 4 + r] == (0x180 + 0x2A0) * 65536);
        }
        assert(sh->beyond_b[100 * 4 + 2] == 0x1E8000); /* 30.5 pixels */
    }
    /* The other limits: the left edge past plane A's high one, the right past plane B's low. */
    edge_road(0x140 - 160 + 32, -160 - 30);
    assert(whole_shifts.beyond_a[100 * 4] == 0x200000 && fine_shifts.beyond_a[100 * 4] == 0x200000);
    assert(whole_shifts.beyond_b[100 * 4] == -0x1E0000 && fine_shifts.beyond_b[100 * 4] == -0x1E0000);
}

static void test_beyond_saturates(void) {
    edge_road(-5000 - 0x160 - 160, 5000 + 0x180 - 160);
    for (int at = 100 * 4; at < 102 * 4; at++) {
        assert(whole_shifts.beyond_a[at] == -0x1000000 && whole_shifts.beyond_b[at] == 0x1000000);
        assert(fine_shifts.beyond_a[at] == -0x1000000 && fine_shifts.beyond_b[at] == 0x1000000);
    }
}

static void test_a_b_and_have_are_unchanged_by_beyond(void) {
    /* The values the earlier tests pin, once more beside a clamped edge. */
    race(-2000);
    rr_road_read(ram, &road);
    rr_road_shifts(&road, rom, sizeof rom, 4, &shifts);
    assert(shifts.have[100] && shifts.have[151] && !shifts.have[99] && !shifts.have[152]);
    assert((shifts.a[100 * 4] >> 16) == -20 + 160 - 0x2A0 && (shifts.b[100 * 4] >> 16) == 20 + 160 + 0x2A0);
    assert(shifts.a[151 * 4] == (-0x160 - 0x2A0) * 65536 && shifts.beyond_a[151 * 4] < 0);
    assert(shifts.beyond_a[100 * 4] == 0 && shifts.beyond_b[100 * 4] == 0);
}

/* A fine edge is not always within a pixel or two of the game's. The game also drops the
 * remainder of each point's *line*, so its edge can be up to one line out of place, and
 * near the camera the edge moves several pixels sideways per line. The difference is
 * bounded by two pixels (the two dropped remainders of an edge) plus the edge's sideways
 * movement over one line. */
static void test_fine_and_whole_edges_differ_by_less_than_two_pixels_and_a_line(void) {
    perspective_table();
    race_inputs(-150, 96);
    rr_road_read_inputs(ram, &inputs);
    rr_road_project(&inputs, rom, sizeof rom, &fine, &whole);
    rr_road_shifts(&whole, rom, sizeof rom, 4, &whole_shifts);
    rr_road_shifts_fine(&fine, &whole_shifts, &fine_shifts);
    int lines = 0;
    for (int l = 0; l + 1 < RR_SCREEN_H; l++) {
        assert(fine_shifts.have[l] == whole_shifts.have[l]);
        if (!whole_shifts.have[l] || !whole_shifts.have[l + 1]) continue;
        lines++;
        /* The movement over one line, of whichever edge moves more (at the screen's side
         * one of them is already held at the limit while the other is still moving). */
        long per_line_a = magnitude((long)whole_shifts.a[(l + 1) * 4] - whole_shifts.a[l * 4]);
        long per_line_b = magnitude((long)whole_shifts.b[(l + 1) * 4] - whole_shifts.b[l * 4]);
        long fine_a = magnitude((long)fine_shifts.a[(l + 1) * 4] - fine_shifts.a[l * 4]);
        long fine_b = magnitude((long)fine_shifts.b[(l + 1) * 4] - fine_shifts.b[l * 4]);
        if (fine_a > per_line_a) per_line_a = fine_a;
        if (fine_b > per_line_b) per_line_b = fine_b;
        assert(magnitude((long)fine_shifts.a[l * 4] - whole_shifts.a[l * 4]) < 0x20000 + per_line_a);
        assert(magnitude((long)fine_shifts.b[l * 4] - whole_shifts.b[l * 4]) < 0x20000 + per_line_b);
    }
    assert(lines > 40);
}

/* ---- which inputs belong to which road ---- */

static RrRoadTracker tracker;

static void store_points(const RrRoad *road) {
    for (int i = 0; i < road->count; i++) {
        put16(ram + 0x1E20 + 2 * i, road->line[i]);
        put16(ram + 0xD2B8 + 2 * i, road->clipped_line[i]);
        put16(ram + 0x1E84 + 2 * i, road->left[i]);
        put16(ram + 0x1F4C + 2 * i, road->right[i]);
        put16(ram + 0x1DBC + 2 * i, road->centre[i]);
    }
}

/* Video memory showing the shift table of the road whose points are in RAM. */
static void show_road_in_ram(void) {
    static RrRoad shown;
    static RrRoadShifts table;
    memset(&vdp, 0, sizeof vdp);
    vdp.reg[11] = 0x03;
    vdp.reg[13] = 0x30;
    assert(rr_road_read(ram, &shown) == 1);
    rr_road_shifts(&shown, rom, sizeof rom, 1, &table);
    for (int l = 0; l < RR_SCREEN_H; l++) {
        if (!table.have[l]) continue;
        put16(&vdp.vram[0xC000 + l * 4], table.a[l] >> 16);
        put16(&vdp.vram[0xC000 + l * 4 + 2], table.b[l] >> 16);
    }
}

/* The game projects (inputs noted), then turns the points into the picture (road noted). */
static void project_and_note(int sideways, int points_sideways) {
    perspective_table();
    rr_road_tracker_reset(&tracker);
    race_inputs(sideways, 96);
    rr_road_tracker_note_inputs(&tracker, ram);
    race_inputs(points_sideways, 96);
    rr_road_read_inputs(ram, &inputs);
    rr_road_project(&inputs, rom, sizeof rom, &fine, &whole);
    store_points(&whole);
    rr_road_tracker_note(&tracker, ram, rom, sizeof rom);
    show_road_in_ram();
}

static void test_matching_inputs_give_fine_shifts_and_the_whole_table_for_the_guard(void) {
    project_and_note(-150, -150);
    const RrRoadShifts *chosen = rr_road_tracker_choose(&tracker, ram, rom, sizeof rom, &vdp, 4);
    assert(chosen == &tracker.shifts);
    assert(tracker.fine_frames == 1 && tracker.whole_frames == 0 && tracker.crisp_frames == 1);
    assert(rr_road_matches_video(&tracker.whole, &vdp));
    assert(memcmp(tracker.shifts.have, tracker.whole.have, sizeof tracker.whole.have) == 0);
    assert(memcmp(tracker.shifts.a, tracker.whole.a, sizeof tracker.whole.a) != 0);
}

static void test_inputs_that_project_to_other_points_are_not_used(void) {
    project_and_note(-150, -90); /* the points in RAM are another projection's */
    const RrRoadShifts *chosen = rr_road_tracker_choose(&tracker, ram, rom, sizeof rom, &vdp, 4);
    assert(chosen != NULL);
    assert(tracker.fine_frames == 0 && tracker.whole_frames == 1 && tracker.crisp_frames == 1);
    assert(memcmp(tracker.shifts.a, tracker.whole.a, sizeof tracker.whole.a) == 0);
}

static void test_whole_mode_draws_the_whole_points(void) {
    project_and_note(-150, -150);
    tracker.mode = RR_ROAD_WHOLE;
    const RrRoadShifts *chosen = rr_road_tracker_choose(&tracker, ram, rom, sizeof rom, &vdp, 4);
    assert(chosen != NULL && tracker.fine_frames == 0 && tracker.whole_frames == 1);
    assert(memcmp(tracker.shifts.a, tracker.whole.a, sizeof tracker.whole.a) == 0);
}

static void test_fine_is_the_default_mode_and_a_reset_keeps_the_mode(void) {
    static RrRoadTracker fresh; /* all zero, as the console and the tool start theirs */
    rr_road_tracker_reset(&fresh);
    assert(fresh.mode == RR_ROAD_FINE);
    fresh.mode = RR_ROAD_WHOLE;
    fresh.fine_frames = 7;
    rr_road_tracker_reset(&fresh);
    assert(fresh.mode == RR_ROAD_WHOLE && fresh.fine_frames == 0);
}

/* ---- the road between two updates ---- */

static RrRoadInputs in_a, in_b, in_t;

/* Two sets of inputs over the same track: the camera at place_a and place_b. */
static void two_cameras(long place_a, int sideways_a, int height_a, long place_b, int sideways_b, int height_b) {
    perspective_table();
    race_inputs(sideways_a, height_a);
    put16(ram + 0x0A0A, (int)(place_a >> 16));
    put16(ram + 0x0A0C, (int)(place_a & 0xFFFF));
    assert(rr_road_read_inputs(ram, &in_a) == 1);
    put16(ram + 0x0A0A, (int)(place_b >> 16));
    put16(ram + 0x0A0C, (int)(place_b & 0xFFFF));
    put16(ram + 0x0A02, sideways_b);
    put16(ram + 0x0A08, height_b);
    assert(rr_road_read_inputs(ram, &in_b) == 1);
}

static void test_inputs_carry_the_cameras_place(void) {
    two_cameras(0x00050000, 0, 96, 0x00058000, 0, 96);
    assert(in_a.place == 0x00050000 && in_b.place == 0x00058000 && in_b.fraction == 0x8000);
}

static void test_inputs_between_at_zero_is_a(void) {
    two_cameras(0x00054000, 10, 100, 0x0005C000, 30, 104);
    memset(&in_t, 0x55, sizeof in_t);
    assert(rr_road_inputs_between(&in_a, &in_b, ram + 0xD828, 0, &in_t) == 1);
    assert(memcmp(&in_t, &in_a, sizeof in_a) == 0);
}

static void test_inputs_between_at_one_is_b(void) {
    two_cameras(0x00054000, 10, 100, 0x00064000, 30, 104);
    assert(rr_road_inputs_between(&in_a, &in_b, ram + 0xD828, 0x10000, &in_t) == 1);
    assert(memcmp(&in_t, &in_b, sizeof in_b) == 0);
}

static void test_inputs_between_halfway(void) {
    two_cameras(0x00050000, 10, 100, 0x00058000, 30, 104);
    in_b.reference = (int16_t)(in_a.reference + 3);
    assert(rr_road_inputs_between(&in_a, &in_b, ram + 0xD828, 0x8000, &in_t) == 1);
    assert(in_t.place == 0x00054000 && in_t.fraction == 0x4000);
    assert(in_t.sideways == 20 && in_t.height == 102);
    assert(in_t.reference == in_a.reference + 2); /* 1.5 rounds to 2 */
    assert(in_t.count == in_a.count && in_t.top == in_a.top && in_t.bottom == in_a.bottom);
    assert(memcmp(in_t.curve, in_a.curve, sizeof in_a.curve) == 0); /* still segment 5 */
}

static void test_inputs_between_crosses_a_segment(void) {
    two_cameras(0x0005C000, 0, 96, 0x00064000, 0, 96);
    assert(rr_road_inputs_between(&in_a, &in_b, ram + 0xD828, 0x8000, &in_t) == 1);
    assert(in_t.place == 0x00060000 && in_t.fraction == 0);
    assert(memcmp(in_t.curve, in_b.curve, sizeof in_b.curve) == 0); /* segment 6's window */
    assert(memcmp(in_t.hill, in_b.hill, sizeof in_b.hill) == 0);
    assert(in_t.curve[2] == -3 && in_a.curve[2] == 0);              /* which differs from segment 5's */
}

static void test_inputs_between_past_b(void) {
    two_cameras(0x00050000, 10, 100, 0x00058000, 30, 104);
    assert(rr_road_inputs_between(&in_a, &in_b, ram + 0xD828, 0x18000, &in_t) == 1);
    assert(in_t.place == 0x0005C000 && in_t.sideways == 40 && in_t.height == 106);
}

static void test_camera_jump_is_not_slid(void) {
    two_cameras(0x00050000, 0, 96, 0x000A0000, 0, 96); /* five segments on */
    memset(&in_t, 0x55, sizeof in_t);
    RrRoadInputs before = in_t;
    assert(rr_road_inputs_between(&in_a, &in_b, ram + 0xD828, 0x8000, &in_t) == 0);
    assert(rr_road_inputs_between(&in_b, &in_a, ram + 0xD828, 0x8000, &in_t) == 0); /* backwards */
    assert(memcmp(&in_t, &before, sizeof before) == 0);
    /* Rolling back a little is motion too (the bike can be pushed backwards). */
    two_cameras(0x00058000, 10, 96, 0x00054000, 30, 96);
    assert(rr_road_inputs_between(&in_a, &in_b, ram + 0xD828, 0x8000, &in_t) == 1);
    assert(in_t.place == 0x00056000 && in_t.sideways == 20);
    assert(rr_road_inputs_between(&in_a, &in_b, ram + 0xD828, 0x18000, &in_t) == 1 && in_t.place == 0x00052000);
    two_cameras(0x00050000, 0, 96, 0x00090000, 0, 96); /* four: still motion */
    assert(rr_road_inputs_between(&in_a, &in_b, ram + 0xD828, 0x8000, &in_t) == 1);
    in_b.count = in_a.count - 1;
    assert(rr_road_inputs_between(&in_a, &in_b, ram + 0xD828, 0x8000, &in_t) == 0);
}

static void test_projecting_between_is_between(void) {
    static RrFineRoad at_a, at_b, at_t;
    static RrRoad unused;
    two_cameras(0x00050000, -40, 96, 0x00058000, -40, 96);
    memset(ram + 0xD828, 0, 0x200); /* straight and flat */
    assert(rr_road_read_inputs(ram, &in_b) == 1);
    in_a = in_b;
    in_a.place = 0x00050000;
    in_a.fraction = 0;
    assert(rr_road_inputs_between(&in_a, &in_b, ram + 0xD828, 0x8000, &in_t) == 1);
    rr_road_project(&in_a, rom, sizeof rom, &at_a, &unused);
    rr_road_project(&in_b, rom, sizeof rom, &at_b, &unused);
    rr_road_project(&in_t, rom, sizeof rom, &at_t, &unused);
    int strictly = 0;
    for (int i = 1; i < at_a.count; i++) {
        int32_t low = at_a.line[i] < at_b.line[i] ? at_a.line[i] : at_b.line[i];
        int32_t high = at_a.line[i] < at_b.line[i] ? at_b.line[i] : at_a.line[i];
        assert(at_t.line[i] >= low && at_t.line[i] <= high);
        strictly += at_t.line[i] > low && at_t.line[i] < high;
    }
    assert(strictly > 10);
}

static void test_tracker_keeps_the_chosen_roads_inputs(void) {
    const uint8_t *ring = NULL;
    rr_road_tracker_reset(&tracker);
    assert(rr_road_tracker_chosen_inputs(&tracker, &ring) == NULL);
    project_and_note(-150, -150);
    uint8_t noted_ring[0x200];
    memcpy(noted_ring, ram + 0xD828, sizeof noted_ring);
    assert(rr_road_tracker_choose(&tracker, ram, rom, sizeof rom, &vdp, 4) != NULL);
    memset(ram + 0xD828, 0x11, 0x200); /* the game has moved on since */
    const RrRoadInputs *kept = rr_road_tracker_chosen_inputs(&tracker, &ring);
    assert(kept != NULL && kept->sideways == -150 && kept->place == 0x00054000);
    assert(ring != NULL && memcmp(ring, noted_ring, sizeof noted_ring) == 0);
    /* A road without inputs has none to give. */
    project_and_note(-150, -90);
    assert(rr_road_tracker_choose(&tracker, ram, rom, sizeof rom, &vdp, 4) != NULL);
    assert(rr_road_tracker_chosen_inputs(&tracker, &ring) == NULL);
    /* Nor has a frame for which no road was chosen. */
    project_and_note(-150, -150);
    memset(&vdp, 0, sizeof vdp);
    vdp.vram[0xC000 + 150 * 4] = 0x7F;
    (void)rr_road_tracker_choose(&tracker, ram, rom, sizeof rom, &vdp, 4);
    assert(rr_road_tracker_chosen_inputs(&tracker, &ring) == NULL);
}

int main(void) {
    test_inputs_carry_the_cameras_place();
    test_inputs_between_at_zero_is_a();
    test_inputs_between_at_one_is_b();
    test_inputs_between_halfway();
    test_inputs_between_crosses_a_segment();
    test_inputs_between_past_b();
    test_camera_jump_is_not_slid();
    test_projecting_between_is_between();
    test_tracker_keeps_the_chosen_roads_inputs();
    test_a_straight_road_has_straight_edges();
    test_a_crest_hides_the_road_behind_it();
    test_clamped_edge_stays_clamped_on_every_row_fine();
    test_beyond_is_zero_inside_the_clamps();
    test_beyond_is_the_excess_on_every_row();
    test_beyond_saturates();
    test_a_b_and_have_are_unchanged_by_beyond();
    test_a_run_extended_enormously_goes_off_the_proper_side();
    test_a_run_with_no_height_keeps_the_whole_pixel_road();
    test_fine_and_whole_edges_differ_by_less_than_two_pixels_and_a_line();
    test_matching_inputs_give_fine_shifts_and_the_whole_table_for_the_guard();
    test_inputs_that_project_to_other_points_are_not_used();
    test_whole_mode_draws_the_whole_points();
    test_fine_is_the_default_mode_and_a_reset_keeps_the_mode();
    test_dropping_the_remainder_gives_the_whole_points();
    test_the_remainder_is_small();
    test_a_line_above_the_view_is_clamped_without_a_remainder();
    test_inputs_are_refused_outside_a_race_and_with_a_short_rom();
    test_row_0_equals_the_integer_table();
    test_rows_continue_the_slope();
    test_clamped_edge_stays_clamped_on_every_row();
    test_line_beyond_the_last_point_has_no_shift();
    test_not_a_race_is_refused();
    test_a_rom_without_the_table_gives_no_shifts();
    puts("all road tests passed");
    return 0;
}
