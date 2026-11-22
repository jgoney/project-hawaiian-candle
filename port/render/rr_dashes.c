#include "rr_dashes.h"

#include <string.h>

#define RACE_CALLBACK 0x0000E038u

static unsigned u16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static int16_t s16(const uint8_t *p) { return (int16_t)u16(p); }

void rr_dashes_lines(const RrRoad *road, uint32_t place, int stop_line, const uint8_t *rom,
                     size_t rom_size, RrDashLines *out) {
    static _Thread_local int32_t left[RR_SCREEN_H], right[RR_SCREEN_H], centre[RR_SCREEN_H];
    uint8_t have_left[RR_SCREEN_H], have_right[RR_SCREEN_H], have_centre[RR_SCREEN_H];
    memset(out, 0, sizeof *out);
    if (road->count < 2 || road->count > RR_ROAD_POINTS) return;
    rr_road_lines(road, road->left, rom, rom_size, left, have_left);
    rr_road_lines(road, road->right, rom, rom_size, right, have_right);
    rr_road_lines(road, road->centre, rom, rom_size, centre, have_centre);
    int any = 0;
    /* 0x14822: a start and a width for every road line. */
    for (int line = 0; line < RR_SCREEN_H; line++) {
        if (!have_left[line] || !have_right[line] || !have_centre[line]) continue;
        any = 1;
        uint16_t across = (uint16_t)((right[line] >> 16) - (left[line] >> 16));
        int16_t half = (int16_t)((int16_t)across >> 1);
        int width = (uint16_t)(across + (uint16_t)half) >> 6;
        if (width == 0) width = 1;
        int start = (int16_t)(centre[line] >> 16);
        if (start >= 0x140) {
            start = 0;
            width = 0;
        }
        if (start < 0) {
            width += start;
            start = 0;
            if (width < 0) width = 0;
        }
        out->start[line] = (int16_t)start;
        out->width[line] = (int16_t)width;
    }
    if (!any) return; /* a ROM without the table, or a road with no lines */
    /* 0x12768: from the bottom of the view up, the lines of each band between two
     * points; the bands of odd track segments are not painted. */
    int at = road->view_bottom;
    unsigned segment = place >> 16;
    for (int point = 1; point < road->count; point++, segment++) {
        int top = road->line[point];
        int stopped = 0;
        while (at > top) {
            at--;
            if ((segment & 1) && at >= 0 && at < RR_SCREEN_H) out->width[at] = 0;
            if (at == stop_line) {
                stopped = 1;
                break;
            }
            if (at < -1) break; /* nothing further up is on the screen */
        }
        if (stopped) break;
        if (at < -1) at = top < at ? top : at;
    }
    out->ok = 1;
}

void rr_dashes_read(const uint8_t *ram, RrDashLines *out) {
    memset(out, 0, sizeof *out);
    uint32_t callback = (uint32_t)u16(ram + 0x0300) << 16 | u16(ram + 0x0302);
    int count = (int)u16(ram + 0x1CA0);
    if (callback != RACE_CALLBACK || u16(ram + 0x0304) == 1 || count < 2 || count > RR_ROAD_POINTS) return;
    if (s16(ram + 0x0698) > 0) return; /* near the finish line: not modelled */
    for (int line = 0; line < RR_SCREEN_H; line++) {
        out->start[line] = s16(ram + 0x2EDE + 2 * line);
        out->width[line] = s16(ram + 0x309E + 2 * line);
    }
    out->ok = 1;
}

int rr_dashes_stop_line(const uint8_t *ram) {
    unsigned at = u16(ram + 0x27BC); /* the low word of the pointer: an address in work RAM */
    return (int16_t)(ram[at] << 8 | ram[(at + 1) & 0xFFFF]);
}

void rr_dashes_strips(const RrFineRoad *road, uint32_t place, const uint8_t *have, int scale,
                      RrStrips *out) {
    rr_dashes_strips_wide(road, place, have, scale, 0, out);
}

void rr_dashes_strips_wide(const RrFineRoad *road, uint32_t place, const uint8_t *have, int scale, int margin,
                           RrStrips *out) {
    static _Thread_local int32_t left[RR_SCREEN_H * RR_SCALE_MAX], right[RR_SCREEN_H * RR_SCALE_MAX],
        centre[RR_SCREEN_H * RR_SCALE_MAX];
    static _Thread_local uint8_t have_left[RR_SCREEN_H * RR_SCALE_MAX], have_right[RR_SCREEN_H * RR_SCALE_MAX],
        have_centre[RR_SCREEN_H * RR_SCALE_MAX], band[RR_SCREEN_H * RR_SCALE_MAX];
    if (scale < 1) scale = 1;
    if (scale > RR_SCALE_MAX) scale = RR_SCALE_MAX;
    int rows = RR_SCREEN_H * scale;
    out->scale = scale;
    out->colour = 0x3C; /* colour set 3, colour 12: what 0x13F20 paints (its table at 0x13EF4) */
    for (int row = 0; row < RR_SCREEN_H * RR_SCALE_MAX; row++) out->from[row] = out->to[row] = 0;
    if (road->count < 2 || road->count > RR_ROAD_POINTS) return;
    rr_road_rows_fine(road, road->left, scale, left, have_left, NULL);
    rr_road_rows_fine(road, road->right, scale, right, have_right, NULL);
    rr_road_rows_fine(road, road->centre, scale, centre, have_centre, NULL);
    /* Which band each row is, as the game's walk has it (0x12768): from the bottom
     * of the view up, each point takes the rows above the last one taken, down to
     * its own line. (Not the band whose run was drawn last on the row: near the
     * camera a farther band's run is drawn over a nearer band's lines.) */
    memset(band, 0xFF, (size_t)rows);
    int64_t at = (int64_t)road->view_bottom * scale;
    if (at > rows) at = rows;
    for (int point = 1; point < road->count; point++) {
        int64_t top = (int64_t)road->line[point] * scale;
        top = top <= 0 ? 0 : (top + 0xFFFF) >> 16;
        for (int64_t row = top; row < at; row++) band[row] = (uint8_t)(point - 1);
        if (top < at) at = top;
    }
    if (margin < 0) margin = 0;
    int64_t low = -(int64_t)margin * scale, wide = (int64_t)(RR_SCREEN_W + margin) * scale;
    for (int row = 0; row < rows; row++) {
        if (!have_left[row] || !have_right[row] || !have_centre[row]) continue;
        if (have != NULL && !have[row / scale]) continue; /* not one of the game's road lines */
        if (band[row] == 0xFF || (((place >> 16) + band[row]) & 1)) continue; /* an unpainted segment */
        int64_t start = (int64_t)centre[row] + (int64_t)road->centre_x * 65536;
        int64_t width = ((int64_t)right[row] - left[row]) * 3 / 128;
        if (width < 0x10000) width = 0x10000;
        if (start >= (int64_t)(RR_SCREEN_W + margin) * 65536) continue;
        int64_t from = (start * scale) >> 16, to = ((start + width) * scale) >> 16;
        if (from < low) from = low;
        if (to > wide) to = wide;
        if (to <= from) continue;
        out->from[row] = (int32_t)from;
        out->to[row] = (int32_t)to;
    }
}

void rr_dashes_mark(const uint8_t *table, int first, uint8_t *skip) {
    for (int i = 0; i < RR_SPRITES_MAX; i++)
        skip[i] = i >= first && (table[8 * i + 4] & 0xE0) == 0x60;
}

void rr_dashes_note(RrRoadTracker *t, const uint8_t *ram, const uint8_t *rom, size_t rom_size) {
    static _Thread_local RrRoad road;
    static _Thread_local RrDashLines made, held;
    if (t->count == 0) return;
    t->dashes_ok[t->newest] = 0;
    if (!rr_road_read(ram, &road) || memcmp(&road, &t->recent[t->newest], sizeof road) != 0) return;
    rr_dashes_read(ram, &held);
    if (!held.ok) return;
    uint32_t place = (uint32_t)u16(ram + 0x0A0A) << 16 | u16(ram + 0x0A0C);
    rr_dashes_lines(&road, place, rr_dashes_stop_line(ram), rom, rom_size, &made);
    if (!made.ok) return;
    int top = s16(ram + 0xF2B0);
    for (int line = top < 0 ? 0 : top; line < road.view_bottom && line < RR_SCREEN_H; line++)
        if (made.start[line] != held.start[line] || made.width[line] != held.width[line]) return;
    t->dashes_ok[t->newest] = 1;
}

int rr_dashes_proven(const RrRoadTracker *t) {
    return t->have_chosen && t->has_fine[t->chosen] && t->dashes_ok[t->chosen];
}

int rr_dashes_frame(const RrRoadTracker *road, const RrObjectSet *set, const RrFineSprites *fine,
                    const RrVdp *v, int scale, RrStrips *strips, RrFineSprites *out) {
    return rr_dashes_frame_wide(road, set, fine, v, scale, 0, strips, out);
}

int rr_dashes_frame_wide(const RrRoadTracker *road, const RrObjectSet *set, const RrFineSprites *fine,
                         const RrVdp *v, int scale, int margin, RrStrips *strips, RrFineSprites *out) {
    uint8_t table[RR_SPRITES_MAX * 8];
    if (set == NULL || fine == NULL || road->mode != RR_ROAD_FINE || !rr_dashes_proven(road)) return 0;
    rr_dashes_strips_wide(&road->recent_fine[road->chosen], road->recent_inputs[road->chosen].place,
                          road->whole.have, scale, margin, strips);
    *out = *fine;
    uint32_t at = (uint32_t)(v->reg[5] & 0x7E) << 9;
    for (uint32_t i = 0; i < sizeof table; i++) table[i] = v->vram[(at + i) & 0xFFFF];
    rr_dashes_mark(table, set->sprites, out->skip);
    out->strips = strips;
    return 1;
}
