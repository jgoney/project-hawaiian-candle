#include "rr_road.h"

#include <string.h>

#define RACE_CALLBACK 0x0000E038u
#define RECIPROCALS 0x15472u /* ROM: 65536 / n for n up to 255 */

static unsigned u16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static int16_t s16(const uint8_t *p) { return (int16_t)u16(p); }

#define TRACK_RING 0xD828u
#define PERSPECTIVE 0x27C84u /* ROM: scale for each distance, 16,384 words */
#define ROAD_HALF_WIDTH 0xE0

static int race_in(const uint8_t *ram) {
    uint32_t callback = (uint32_t)u16(ram + 0x0300) << 16 | u16(ram + 0x0302);
    return callback == RACE_CALLBACK && u16(ram + 0x0304) != 1;
}

int rr_road_read(const uint8_t *ram, RrRoad *out) {
    int count = (int)u16(ram + 0x1CA0);
    if (!race_in(ram) || count < 2 || count > RR_ROAD_POINTS) return 0;
    memset(out, 0, sizeof *out);
    out->count = count;
    for (int i = 0; i < count; i++) {
        out->line[i] = s16(ram + 0x1E20 + 2 * i);
        out->clipped_line[i] = s16(ram + 0xD2B8 + 2 * i);
        out->left[i] = s16(ram + 0x1E84 + 2 * i);
        out->right[i] = s16(ram + 0x1F4C + 2 * i);
        out->centre[i] = s16(ram + 0x1DBC + 2 * i);
    }
    out->centre_x = s16(ram + 0xF2BE);
    out->view_bottom = (int16_t)u16(ram + 0xF2B2);
    int last = out->clipped_line[count - 1];
    out->horizon_line = (int16_t)(last < out->view_bottom - 2 ? last : out->view_bottom - 2);
    return 1;
}

/* The track from the camera's segment on, out of the ring. */
static void track_from(const uint8_t *ring, uint32_t place, RrRoadInputs *out) {
    unsigned at = ((place >> 16) & 0xFF) * 2;
    for (int i = 0; i <= out->count; i++) {
        out->curve[i] = (int8_t)ring[at];
        out->hill[i] = (int8_t)ring[at + 1];
        at = (at + 2) % RR_ROAD_RING;
    }
}

int rr_road_read_inputs(const uint8_t *ram, RrRoadInputs *out) {
    int count = (int)u16(ram + 0x1CA0);
    if (!race_in(ram) || count < 2 || count > RR_ROAD_POINTS) return 0;
    memset(out, 0, sizeof *out);
    out->count = count;
    out->place = (uint32_t)u16(ram + 0x0A0A) << 16 | u16(ram + 0x0A0C);
    out->fraction = (uint16_t)u16(ram + 0x0A0C);
    out->sideways = s16(ram + 0x0A02);
    out->height = s16(ram + 0x0A08);
    out->top = s16(ram + 0xF2B0);
    out->bottom = (int16_t)u16(ram + 0xF2B2);
    out->reference = s16(ram + 0xF2BC);
    out->centre_x = s16(ram + 0xF2BE);
    track_from(ram + TRACK_RING, out->place, out);
    return 1;
}

static int32_t part_way(int32_t a, int32_t b, int32_t t) {
    int64_t moved = ((int64_t)b - a) * t;
    return a + (int32_t)((moved + 0x8000) >> 16);
}

int rr_road_inputs_between(const RrRoadInputs *a, const RrRoadInputs *b, const uint8_t *ring_b,
                           int32_t t, RrRoadInputs *out) {
    int64_t moved = (int64_t)b->place - (int64_t)a->place;
    if (a->count != b->count || moved < -RR_ROAD_JUMP || moved > RR_ROAD_JUMP) return 0;
    RrRoadInputs made;
    memset(&made, 0, sizeof made);
    made.count = a->count;
    made.place = (uint32_t)((int64_t)a->place + ((moved * t + 0x8000) >> 16));
    made.fraction = (uint16_t)made.place;
    made.sideways = (int16_t)part_way(a->sideways, b->sideways, t);
    made.height = (int16_t)part_way(a->height, b->height, t);
    made.top = (int16_t)part_way(a->top, b->top, t);
    made.bottom = (int16_t)part_way(a->bottom, b->bottom, t);
    made.reference = (int16_t)part_way(a->reference, b->reference, t);
    made.centre_x = (int16_t)part_way(a->centre_x, b->centre_x, t);
    track_from(ring_b, made.place, &made);
    *out = made;
    return 1;
}

/* value x scale as 16.16, the sign put back on the product of the magnitudes as
 * the game does. Dropping the low 16 bits of the magnitude gives the game's
 * value. A scale of 0 (the point at the camera) leaves the value as it is. */
static int64_t product(int value, int scale) {
    if (scale == 0) return (int64_t)value * 65536;
    return (int64_t)value * scale;
}
static int dropped(int64_t fine) { return fine < 0 ? -(int)(-fine >> 16) : (int)(fine >> 16); }

int rr_road_project(const RrRoadInputs *in, const uint8_t *rom, size_t rom_size, RrFineRoad *fine,
                    RrRoad *whole) {
    if (rom_size < PERSPECTIVE + 2 * 16384) return 0;
    memset(fine, 0, sizeof *fine);
    memset(whole, 0, sizeof *whole);
    int curve = in->curve[0];
    /* The part of the first segment's curve already behind the camera, rounded. */
    long behind = (long)(curve < 0 ? -curve : curve) * in->fraction;
    int done = (int)(behind >> 16) + ((behind & 0x8000) ? 1 : 0);
    if (curve < 0) done = -done;
    int16_t step = (int16_t)(curve - done);
    int16_t sideways = (int16_t)-in->sideways;
    int hill = in->hill[0];
    int risen = (int)(((long)(hill < 0 ? -hill : hill) * in->fraction) >> 16);
    int16_t rise = (int16_t)-(hill < 0 ? -risen : risen);
    int16_t distance = (int16_t)-(in->fraction >> 8);
    int lowest = in->bottom;
    int32_t lowest_fine = (int32_t)in->bottom * 65536;
    for (int i = 0; i < in->count; i++) {
        int scale = distance > 0 ? (int)u16(rom + PERSPECTIVE + 2 * (unsigned)distance) : 0;
        int16_t under = (int16_t)(in->height - rise);
        int64_t down = product(under, scale);
        int16_t line = (int16_t)(dropped(down) + in->reference);
        int32_t line_fine = (int32_t)(down + (int64_t)in->reference * 65536);
        if (under < 0 && line < in->top) {
            line = in->top;
            line_fine = (int32_t)in->top * 65536;
        }
        if ((uint16_t)line < (uint16_t)lowest && distance >= 0x100) {
            lowest = line;
            lowest_fine = line_fine;
        }
        int64_t across = product(sideways, scale), half = product(ROAD_HALF_WIDTH, scale);
        whole->line[i] = line;
        whole->clipped_line[i] = (int16_t)lowest;
        whole->left[i] = (int16_t)(dropped(across) - dropped(half));
        whole->right[i] = (int16_t)(dropped(across) + dropped(half));
        whole->centre[i] = (int16_t)(dropped(across) + in->centre_x);
        fine->centre[i] = (int32_t)across;
        fine->line[i] = line_fine;
        fine->clipped_line[i] = lowest_fine;
        fine->left[i] = (int32_t)(across - half);
        fine->right[i] = (int32_t)(across + half);
        distance = (int16_t)(distance + 0x100);
        sideways = (int16_t)(sideways + step);
        step = (int16_t)(step + in->curve[i + 1]);
        rise = (int16_t)(rise + in->hill[i + 1]);
    }
    fine->count = whole->count = in->count;
    fine->centre_x = whole->centre_x = in->centre_x;
    fine->view_bottom = whole->view_bottom = in->bottom;
    int last = whole->clipped_line[in->count - 1];
    whole->horizon_line = (int16_t)(last < in->bottom - 2 ? last : in->bottom - 2);
    return 1;
}

static int64_t floor_div(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

/* One edge for every row between neighbouring points, as 16.16 screen x
 * relative to the screen centre (the game's 0x153EA, with rows added). */
static void interpolate(const RrRoad *road, const int16_t *xs, const uint8_t *rom, int scale,
                        int32_t *out, uint8_t *have) {
    for (int i = 0; i + 1 < road->count; i++) {
        int far_line = road->line[i + 1];
        int lines = (int16_t)(road->clipped_line[i] - far_line);
        if (lines <= 0) continue;
        int across = (int16_t)(xs[i] - xs[i + 1]);
        int down = (int16_t)(road->line[i] - far_line);
        int32_t step;
        if (down <= 1) {
            step = (int32_t)((uint32_t)(uint16_t)across << 16);
        } else if (down == 2) {
            step = (int32_t)((uint32_t)(uint16_t)across << 16) >> 1;
        } else {
            int shift = 0;
            while (down >= 0x100) {
                down >>= 1;
                shift++;
            }
            step = across * (int16_t)(u16(rom + RECIPROCALS + 2 * down) >> shift);
        }
        int32_t value = (int32_t)((uint32_t)(uint16_t)xs[i + 1] << 16);
        for (int line = far_line; line < far_line + lines; line++) {
            if (line >= 0 && line < RR_SCREEN_H) {
                for (int r = 0; r < scale; r++)
                    out[line * scale + r] = (int32_t)(value + floor_div((int64_t)step * r, scale));
                have[line] = 1;
            }
            value = (int32_t)((uint32_t)value + (uint32_t)step); /* wraps as the 68000's long does */
        }
    }
}

void rr_road_lines(const RrRoad *road, const int16_t *xs, const uint8_t *rom, size_t rom_size,
                   int32_t *out, uint8_t *have) {
    memset(have, 0, RR_SCREEN_H);
    if (rom_size < RECIPROCALS + 512 || road->count < 2 || road->count > RR_ROAD_POINTS) return;
    interpolate(road, xs, rom, 1, out, have);
}

/* edge + centre, kept within the game's limits, plus the plane's offset.
 * `beyond` receives how far the limit moved it (see RrRoadShifts). */
static int32_t plane_shift(int32_t edge, int centre_x, int low, int high, int offset, int32_t *beyond) {
    enum { BEYOND_MAX = 256 * 65536 };
    /* The game adds the centre to the whole part as a 16-bit word. */
    int64_t whole = (int16_t)((edge >> 16) + centre_x);
    int64_t free_value = whole * 65536 + (edge & 0xFFFF), value = free_value;
    if (value > (int64_t)high * 65536) value = (int64_t)high * 65536;
    if (value < (int64_t)low * 65536) value = (int64_t)low * 65536;
    int64_t excess = free_value - value;
    *beyond = excess > BEYOND_MAX ? BEYOND_MAX : excess < -BEYOND_MAX ? -BEYOND_MAX : (int32_t)excess;
    return (int32_t)(value + (int64_t)offset * 65536);
}

void rr_road_shifts(const RrRoad *road, const uint8_t *rom, size_t rom_size, int scale,
                    RrRoadShifts *out) {
    static _Thread_local int32_t left[RR_SCREEN_H * RR_SCALE_MAX], right[RR_SCREEN_H * RR_SCALE_MAX];
    uint8_t have_left[RR_SCREEN_H] = {0}, have_right[RR_SCREEN_H] = {0};
    if (scale < 1) scale = 1;
    if (scale > RR_SCALE_MAX) scale = RR_SCALE_MAX;
    memset(out->have, 0, sizeof out->have);
    memset(out->beyond_a, 0, sizeof out->beyond_a);
    memset(out->beyond_b, 0, sizeof out->beyond_b);
    out->scale = scale;
    if (rom_size < RECIPROCALS + 512) return;
    interpolate(road, road->left, rom, scale, left, have_left);
    interpolate(road, road->right, rom, scale, right, have_right);
    for (int line = road->horizon_line; line < road->view_bottom; line++) {
        if (line < 0 || line >= RR_SCREEN_H || !have_left[line] || !have_right[line]) continue;
        out->have[line] = 1;
        for (int r = 0; r < scale; r++) {
            int at = line * scale + r;
            out->a[at] = plane_shift(left[at], road->centre_x, -0x160, 0x140, -0x2A0, &out->beyond_a[at]);
            out->b[at] = plane_shift(right[at], road->centre_x, 0, 0x180, 0x2A0, &out->beyond_b[at]);
        }
    }
}

int rr_road_matches_video(const RrRoadShifts *shifts, const RrVdp *v) {
    uint32_t table = (uint32_t)(v->reg[13] & 0x3F) << 10;
    int any = 0;
    if ((v->reg[11] & 3) != 3) return 0; /* not one shift per line */
    for (int line = 0; line < RR_SCREEN_H; line++) {
        if (!shifts->have[line]) continue;
        const uint8_t *entry = &v->vram[(table + (uint32_t)line * 4) & 0xFFFF];
        if (((uint32_t)line * 4 + table) > 0xFFFC) return 0;
        if ((uint16_t)(shifts->a[line * shifts->scale] >> 16) != u16(entry)) return 0;
        if ((uint16_t)(shifts->b[line * shifts->scale] >> 16) != u16(entry + 2)) return 0;
        any = 1;
    }
    return any;
}

/* One edge for every row that lies between two fine points; `covered` says which. */
static void fine_rows(const RrFineRoad *road, const int32_t *xs, int scale, int32_t *out,
                      uint8_t *covered) {
    rr_road_rows_fine(road, xs, scale, out, covered, NULL);
}

void rr_road_rows_fine(const RrFineRoad *road, const int32_t *xs, int scale, int32_t *out,
                       uint8_t *covered, uint8_t *band) {
    int rows = RR_SCREEN_H * scale;
    memset(covered, 0, (size_t)rows);
    for (int i = 0; i + 1 < road->count; i++) {
        /* In units of 1/65536 of a row: the far point's line, where the run is cut off
         * (the nearer point, or a crest in front of it), and the whole run's height. */
        int64_t top = (int64_t)road->line[i + 1] * scale, cut = (int64_t)road->clipped_line[i] * scale;
        int64_t height = ((int64_t)road->line[i] - road->line[i + 1]) * scale;
        if (cut <= top) continue;
        int64_t first = top <= 0 ? 0 : (top + 0xFFFF) >> 16, end = cut <= 0 ? 0 : (cut + 0xFFFF) >> 16;
        if (end > rows) end = rows;
        if (height <= 0) {
            /* No run to speak of: the far point is not above the near one. The game
             * still draws these lines from this pair (stepping by the whole distance
             * between the points on every line), over whatever a nearer pair drew, so
             * the rows go back to the whole-pixel road. */
            for (int64_t row = first; row < end; row++) covered[row] = 0;
            continue;
        }
        for (int64_t row = first; row < end; row++) {
            int64_t down = (row << 16) - top;
            int64_t across = (int64_t)xs[i] - xs[i + 1];
            /* A run extended far past its points can leave any screen: keep it within
             * what 16.16 holds, on the side it left by. */
            int64_t x = xs[i + 1] + floor_div(across * down, height);
            if (x > 0x3FFF0000) x = 0x3FFF0000;
            if (x < -0x3FFF0000) x = -0x3FFF0000;
            out[row] = (int32_t)x;
            covered[row] = 1;
            if (band != NULL) band[row] = (uint8_t)i;
        }
    }
}

void rr_road_shifts_fine(const RrFineRoad *road, const RrRoadShifts *whole, RrRoadShifts *out) {
    static _Thread_local int32_t left[RR_SCREEN_H * RR_SCALE_MAX], right[RR_SCREEN_H * RR_SCALE_MAX];
    static _Thread_local uint8_t have_left[RR_SCREEN_H * RR_SCALE_MAX], have_right[RR_SCREEN_H * RR_SCALE_MAX];
    int scale = whole->scale;
    if (out != whole) *out = *whole;
    fine_rows(road, road->left, scale, left, have_left);
    fine_rows(road, road->right, scale, right, have_right);
    for (int line = 0; line < RR_SCREEN_H; line++) {
        if (!whole->have[line]) continue;
        for (int r = 0; r < scale; r++) {
            int at = line * scale + r;
            if (!have_left[at] || !have_right[at]) continue; /* keeps the whole-pixel value */
            out->a[at] = plane_shift(left[at], road->centre_x, -0x160, 0x140, -0x2A0, &out->beyond_a[at]);
            out->b[at] = plane_shift(right[at], road->centre_x, 0, 0x180, 0x2A0, &out->beyond_b[at]);
        }
    }
}

void rr_road_tracker_reset(RrRoadTracker *t) {
    int mode = t->mode;
    memset(t, 0, sizeof *t);
    t->mode = mode;
}

void rr_road_tracker_note_inputs(RrRoadTracker *t, const uint8_t *ram) {
    int next = (t->inputs_newest + 1) % RR_ROAD_RECENT;
    if (!rr_road_read_inputs(ram, &t->inputs[next])) return;
    memcpy(t->inputs_ring[next], ram + TRACK_RING, RR_ROAD_RING);
    t->inputs_newest = next;
    if (t->inputs_count < RR_ROAD_RECENT) t->inputs_count++;
}

void rr_road_tracker_note(RrRoadTracker *t, const uint8_t *ram, const uint8_t *rom, size_t rom_size) {
    int next = (t->newest + 1) % RR_ROAD_RECENT;
    if (!rr_road_read(ram, &t->recent[next])) return;
    t->newest = next;
    if (t->count < RR_ROAD_RECENT) t->count++;
    /* These points' inputs: the newest noted ones that project to exactly these points. */
    t->has_fine[next] = 0;
    t->dashes_ok[next] = 0;
    for (int age = 0; age < t->inputs_count; age++) {
        int which = (t->inputs_newest - age + RR_ROAD_RECENT) % RR_ROAD_RECENT;
        const RrRoadInputs *in = &t->inputs[which];
        RrRoad whole;
        if (!rr_road_project(in, rom, rom_size, &t->recent_fine[next], &whole)) break;
        if (memcmp(&whole, &t->recent[next], sizeof whole) == 0) {
            t->has_fine[next] = 1;
            t->recent_inputs[next] = *in;
            memcpy(t->recent_ring[next], t->inputs_ring[which], RR_ROAD_RING);
            break;
        }
    }
}

const RrRoadShifts *rr_road_tracker_choose(RrRoadTracker *t, const uint8_t *ram, const uint8_t *rom,
                                           size_t rom_size, const RrVdp *v, int scale) {
    RrRoad now;
    t->have_chosen = 0;
    if (!rr_road_read(ram, &now)) {
        t->count = 0; /* not a race any more: what was noted is stale */
        t->inputs_count = 0;
        return NULL;
    }
    t->race_frames++;
    for (int age = 0; age < t->count; age++) {
        int which = (t->newest - age + RR_ROAD_RECENT) % RR_ROAD_RECENT;
        rr_road_shifts(&t->recent[which], rom, rom_size, scale, &t->whole);
        if (!rr_road_matches_video(&t->whole, v)) continue;
        t->crisp_frames++;
        t->chosen = which;
        t->have_chosen = 1;
        if (t->mode == RR_ROAD_FINE && t->has_fine[which]) {
            rr_road_shifts_fine(&t->recent_fine[which], &t->whole, &t->shifts);
            t->fine_frames++;
        } else {
            t->shifts = t->whole;
            t->whole_frames++;
        }
        return &t->shifts;
    }
    return NULL;
}

const RrRoadInputs *rr_road_tracker_chosen_inputs(const RrRoadTracker *t, const uint8_t **ring) {
    if (!t->have_chosen || !t->has_fine[t->chosen]) return NULL;
    if (ring != NULL) *ring = t->recent_ring[t->chosen];
    return &t->recent_inputs[t->chosen];
}
