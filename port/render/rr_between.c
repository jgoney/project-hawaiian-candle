#include "rr_between.h"

#include "rr_dashes.h"

#include <string.h>

enum {
    DRAW_RECORDS = 0xCEB8, DRAW_RECORD_SIZE = 22, RECORD_COUNT = 0xDC42, RECORD_DISTANCES = 0x06D6,
    VEHICLE_LIST = 0x04F8, VEHICLES_MAX = 32, CAMERA_PLACE = 0x0A0A
};

/* Every read is masked to the 64 KiB image, whatever the image holds. */
static unsigned rb(const uint8_t *ram, uint32_t at) { return ram[at & 0xFFFF]; }
static unsigned rw(const uint8_t *ram, uint32_t at) { return rb(ram, at) << 8 | rb(ram, at + 1); }
static uint32_t rl(const uint8_t *ram, uint32_t at) { return (uint32_t)rw(ram, at) << 16 | rw(ram, at + 2); }
static void ww(uint8_t *ram, uint32_t at, unsigned value) {
    ram[at & 0xFFFF] = (uint8_t)(value >> 8);
    ram[(at + 1) & 0xFFFF] = (uint8_t)value;
}

void rr_between_read(const uint8_t *ram, RrBetweenRecords *out) {
    memset(out, 0, sizeof *out);
    unsigned count = rw(ram, RECORD_COUNT);
    if (count > RR_BETWEEN_RECORDS) return;
    /* The vehicles' distances from the camera, by which their records are known. */
    uint32_t address[VEHICLES_MAX], distance[VEHICLES_MAX];
    uint32_t camera = rl(ram, CAMERA_PLACE);
    int vehicles = -1;
    for (int i = 0; i <= VEHICLES_MAX; i++) {
        if (rw(ram, VEHICLE_LIST + 4 * (uint32_t)i) == 0xFFFF) {
            vehicles = i;
            break;
        }
        if (i == VEHICLES_MAX) break;
        address[i] = rl(ram, VEHICLE_LIST + 4 * (uint32_t)i) & 0xFFFF;
        distance[i] = rl(ram, address[i] + 4) - camera;
    }
    if (vehicles < 0) return;
    for (unsigned slot = 0; slot < count; slot++) {
        uint32_t at = DRAW_RECORDS + DRAW_RECORD_SIZE * slot;
        RrBetweenRecord *r = &out->record[slot];
        r->x = (int16_t)rw(ram, at);
        r->foot = (int16_t)rw(ram, at + 2);
        r->size = (uint16_t)rw(ram, at + 4);
        r->hidden = (int16_t)rw(ram, at + 6);
        r->picture = (uint16_t)rw(ram, at + 8);
        r->flags = (uint16_t)rw(ram, at + 0x0A);
        r->offset = (int16_t)rw(ram, at + 0x0C);
        r->roadside = (uint16_t)rw(ram, at + 0x10);
        r->second = (uint16_t)rw(ram, at + 0x12);
        r->segment = (uint16_t)rw(ram, at + 0x14);
        if (rw(ram, at + 0x0E) != 0 || r->roadside != 0) {
            r->key = 1ull << 60 | (uint64_t)r->segment << 40 | (uint64_t)r->picture << 24
                     | (uint64_t)r->roadside << 8 | ((r->flags >> 14) & 1);
        } else {
            uint32_t from_camera = rl(ram, RECORD_DISTANCES + 4 * slot);
            int found = -1, twice = 0;
            for (int i = 0; i < vehicles; i++) {
                if (distance[i] != from_camera) continue;
                if (found >= 0) twice = 1;
                found = i;
            }
            r->key = (found >= 0 && !twice) ? (2ull << 60 | address[found]) : 0;
        }
    }
    /* A key held by two records says which is which of neither. */
    for (unsigned i = 0; i < count; i++) {
        if (out->record[i].key == 0) continue;
        int twice = 0;
        for (unsigned j = i + 1; j < count; j++) {
            if (out->record[j].key != out->record[i].key) continue;
            out->record[j].key = 0;
            twice = 1;
        }
        if (twice) out->record[i].key = 0;
    }
    out->count = (int)count;
    out->ok = 1;
}

/* a + (b - a) * t, as 16.16. */
static int64_t line_through(int a, int b, int32_t t) { return (int64_t)a * 65536 + ((int64_t)b - a) * t; }

/* The size at t when 1 / size runs in a straight line from a's to b's: what a
 * steady change of distance does to it. Carried past b it stays within half
 * and twice b's. */
static unsigned size_through(unsigned a, unsigned b, int32_t t) {
    if (a == b || a == 0 || b == 0) return t < 0x8000 ? a : b;
    /* 1/s = 1/a + t (1/b - 1/a)  =>  s = a b / (b + t (a - b)) */
    int64_t below = (int64_t)b * 65536 + ((int64_t)a - b) * t;
    int64_t top = (int64_t)a * b * 65536;
    int64_t low = t > 0x10000 ? b / 2 : (a < b ? a : b), high = t > 0x10000 ? 2 * (int64_t)b : (a < b ? b : a);
    if (high > 0xFFFF) high = 0xFFFF;
    if (below <= 0) return (unsigned)high;
    int64_t size = (top + below / 2) / below;
    if (size < low) size = low;
    if (size > high) size = high;
    return (unsigned)size;
}

static int moved_too_far(const RrBetweenRecord *a, const RrBetweenRecord *b) {
    int across = b->x - a->x, down = b->foot - a->foot;
    if (across < 0) across = -across;
    if (down < 0) down = -down;
    if (across > RR_BETWEEN_JUMP_X || down > RR_BETWEEN_JUMP_FOOT) return 1;
    return (uint32_t)b->size > 2u * a->size || (uint32_t)a->size > 2u * b->size;
}

int rr_between_objects(const uint8_t *ram_before, int32_t carry, const uint8_t *ram_a,
                       const uint8_t *ram_b, const uint8_t *rom, size_t rom_size, int32_t t,
                       RrObjectSet *out, uint8_t *scratch, uint8_t *work) {
    static _Thread_local uint16_t fx[RR_BETWEEN_RECORDS], fy[RR_BETWEEN_RECORDS];
    return rr_between_objects_fine(ram_before, carry, ram_a, ram_b, rom, rom_size, t, out, scratch, work, fx, fy);
}

int rr_between_objects_fine(const uint8_t *ram_before, int32_t carry, const uint8_t *ram_a,
                            const uint8_t *ram_b, const uint8_t *rom, size_t rom_size, int32_t t,
                            RrObjectSet *out, uint8_t *scratch, uint8_t *work, uint16_t *fx, uint16_t *fy) {
    static _Thread_local RrBetweenRecords a, b, before;
    memset(fx, 0, RR_BETWEEN_RECORDS * sizeof *fx);
    memset(fy, 0, RR_BETWEEN_RECORDS * sizeof *fy);
    out->ok = 0;
    rr_between_read(ram_a, &a);
    rr_between_read(ram_b, &b);
    if (!a.ok || !b.ok) return 0;
    before.ok = 0;
    if (ram_before != NULL && carry > 0) rr_between_read(ram_before, &before);
    if (t < 0) t = 0;
    int from_b = t >= 0x10000;
    const RrBetweenRecords *base = from_b ? &b : &a, *other = from_b ? &a : &b;
    memcpy(scratch, from_b ? ram_b : ram_a, 0x10000);
    for (int slot = 0; slot < base->count; slot++) {
        const RrBetweenRecord *mine = &base->record[slot], *theirs = NULL;
        if (mine->key == 0) continue;
        for (int j = 0; j < other->count && theirs == NULL; j++)
            if (other->record[j].key == mine->key) theirs = &other->record[j];
        const RrBetweenRecord *ra, *rb_;
        int32_t place = t;
        if (theirs != NULL) {
            ra = from_b ? theirs : mine;
            rb_ = from_b ? mine : theirs;
        } else {
            /* Gone by b: carry on from the update before a, if it was there too. */
            if (from_b || !before.ok || t == 0) continue;
            const RrBetweenRecord *earlier = NULL;
            for (int j = 0; j < before.count && earlier == NULL; j++)
                if (before.record[j].key == mine->key) earlier = &before.record[j];
            if (earlier == NULL) continue;
            ra = earlier;
            rb_ = mine;
            int64_t on = ((int64_t)t * carry) >> 16;
            place = (int32_t)(0x10000 + (on > 0x20000 ? 0x20000 : on));
        }
        if (moved_too_far(ra, rb_)) continue;
        uint32_t at = DRAW_RECORDS + DRAW_RECORD_SIZE * (uint32_t)slot;
        int64_t x = line_through(ra->x, rb_->x, place), foot = line_through(ra->foot, rb_->foot, place);
        ww(scratch, at, (unsigned)(x >> 16) & 0xFFFF);
        ww(scratch, at + 2, (unsigned)(foot >> 16) & 0xFFFF);
        fx[slot] = (uint16_t)(x & 0xFFFF);
        fy[slot] = (uint16_t)(foot & 0xFFFF);
        ww(scratch, at + 4, size_through(ra->size, rb_->size, place));
        ww(scratch, at + 6, (unsigned)((line_through(ra->hidden, rb_->hidden, place) + 0x8000) >> 16) & 0xFFFF);
        ww(scratch, at + 0x0C, (unsigned)((line_through(ra->offset, rb_->offset, place) + 0x8000) >> 16) & 0xFFFF);
        if (t >= 0x8000 && !from_b && theirs != NULL) {
            /* The second half of the way: the pose the object is arriving at. */
            ww(scratch, at + 8, rb_->picture);
            ww(scratch, at + 0x12, rb_->second);
            ww(scratch, at + 0x0A, (unsigned)(ra->flags & 0x7FFF) | (rb_->flags & 0x8000));
        }
    }
    if (!rr_objects_draw(scratch, rom, rom_size, out, work)) return 0;
    for (int e = out->first_sprite; e < out->sprites; e++) {
        if (out->fine.picture[e] >= 0) continue;
        out->ok = 0; /* an entry only its tiles could show, and they are not in video memory */
        return 0;
    }
    for (int i = 0; i < out->pictures; i++) {
        unsigned slot = out->picture_record[i];
        if (slot >= RR_BETWEEN_RECORDS) continue;
        out->picture[i].fx = fx[slot];
        out->picture[i].fy = fy[slot];
    }
    return 1;
}

int rr_between_sprite_table(const RrObjectSet *set, const RrObjectSet *on_screen, const RrVdp *v,
                            uint8_t *table, RrFineSprites *fine) {
    int count = set->sprites;
    if (count < 0 || count > RR_SPRITES_MAX) return 0;
    memcpy(table, set->list, (size_t)count * 8);
    uint32_t at = (uint32_t)(v->reg[5] & 0x7E) << 9;
    unsigned index = 0;
    for (int visited = 0; visited < RR_SPRITES_MAX; visited++) {
        uint32_t s = at + index * 8;
        if ((int)index >= on_screen->sprites) {
            if (count >= RR_SPRITES_MAX) return 0;
            for (uint32_t k = 0; k < 8; k++) table[count * 8 + (int)k] = v->vram[(s + k) & 0xFFFF];
            count++;
        }
        unsigned link = v->vram[(s + 3) & 0xFFFF] & 0x7F;
        if (link == 0 || link >= RR_SPRITES_MAX) break;
        index = link;
    }
    for (int i = 0; i < count; i++) table[i * 8 + 3] = (uint8_t)(i + 1 < count ? i + 1 : 0);
    memset(table + count * 8, 0, (size_t)(RR_SPRITES_MAX - count) * 8);
    fine->pictures = set->picture;
    fine->table = table;
    fine->strips = NULL;
    memset(fine->skip, 0, sizeof fine->skip);
    for (int i = 0; i < RR_SPRITES_MAX; i++) {
        fine->picture[i] = i < set->sprites ? set->fine.picture[i] : -1;
        fine->dx[i] = i < set->sprites ? set->fine.dx[i] : 0;
    }
    return 1;
}

/* ---- which moment to draw, and the scene for it ---- */

enum { DELAY_STEP = 0x4000 }; /* a quarter of a frame per frame */

void rr_between_forget(RrBetween *b) {
    b->count = 0;
    b->lengths = 0;
    b->delay = -1;
}

void rr_between_reset(RrBetween *b) {
    rr_between_forget(b);
    b->frames = b->between_frames = b->guessed_frames = b->returned = b->margin_fell_back = 0;
    memset(b->fell_back, 0, sizeof b->fell_back);
}

void rr_between_set_mode(RrBetween *b, int mode) {
    if (mode < RR_BETWEEN_OFF || mode > RR_BETWEEN_LATE || mode == b->mode) return;
    b->mode = mode;
    rr_between_forget(b);
}

int32_t rr_between_target_delay(const RrBetween *b) {
    if (b->lengths <= 0 || (b->mode != RR_BETWEEN_BLEND && b->mode != RR_BETWEEN_LATE)) return 0;
    int sum = 0, longest = 0;
    for (int i = 0; i < b->lengths; i++) {
        sum += b->length[i];
        if (b->length[i] > longest) longest = b->length[i];
    }
    return b->mode == RR_BETWEEN_LATE ? longest << 16 : (int32_t)(((int64_t)sum << 16) / b->lengths);
}

void rr_between_note(RrBetween *b, uint64_t frame, const RrRoadTracker *road,
                     const RrObjectTracker *objects, const RrVdp *v) {
    const RrObjectSet *set = rr_object_tracker_chosen(objects);
    const uint8_t *object_ram = rr_object_tracker_chosen_ram(objects);
    if (set == NULL || object_ram == NULL || !road->have_chosen) {
        /* What is on screen was not described: nothing kept may be paired with what comes next. */
        rr_between_forget(b);
    } else {
        uint64_t id = objects->age[objects->chosen];
        const RrBetweenUpdate *last = b->count > 0 ? &b->update[b->count - 1] : NULL;
        if (last != NULL && id < last->id) {
            b->returned++; /* an earlier update is on screen again: not a new one */
        } else if (last == NULL || last->id != id) {
            /* How long the update before this one was on screen. */
            uint64_t lasted = b->count > 0 ? frame - b->update[b->count - 1].stamp : 0;
            if (b->count > 0 && lasted > RR_BETWEEN_GAP) rr_between_forget(b);
            if (b->count > 0) {
                if (b->lengths == RR_BETWEEN_LENGTHS) {
                    memmove(b->length, b->length + 1, RR_BETWEEN_LENGTHS - 1);
                    b->lengths--;
                }
                b->length[b->lengths++] = (uint8_t)(lasted > 255 ? 255 : lasted);
            }
            if (b->count == RR_BETWEEN_UPDATES) {
                memmove(&b->update[0], &b->update[1], (RR_BETWEEN_UPDATES - 1) * sizeof b->update[0]);
                b->count--;
            }
            RrBetweenUpdate *u = &b->update[b->count++];
            const uint8_t *ring = NULL;
            const RrRoadInputs *inputs = rr_road_tracker_chosen_inputs(road, &ring);
            memset(&u->inputs, 0, sizeof u->inputs);
            u->good = inputs != NULL;
            u->dashes = rr_dashes_proven(road);
            if (inputs != NULL) {
                u->inputs = *inputs;
                memcpy(u->ring, ring, sizeof u->ring);
            }
            u->id = id;
            u->stamp = frame;
            u->road_top = RR_SCREEN_H;
            for (int line = RR_SCREEN_H - 1; line >= 0; line--)
                if (road->whole.have[line]) u->road_top = line;
            memcpy(u->object_ram, object_ram, sizeof u->object_ram);
            uint32_t table = (uint32_t)(v->reg[13] & 0x3F) << 10;
            for (uint32_t i = 0; i < sizeof u->shifts; i++) u->shifts[i] = v->vram[(table + i) & 0xFFFF];
        }
    }
    /* The delay follows its target slowly, so that the moment drawn never steps back. */
    int32_t target = rr_between_target_delay(b);
    if (b->delay < 0) {
        if (b->lengths > 0 || b->mode == RR_BETWEEN_PREDICT) b->delay = target;
    } else if (b->delay < target) {
        b->delay = target - b->delay > DELAY_STEP ? b->delay + DELAY_STEP : target;
    } else if (b->delay > target) {
        b->delay = b->delay - target > DELAY_STEP ? b->delay - DELAY_STEP : target;
    }
}

int rr_between_clock(const RrBetween *b, uint64_t frame, int *older, int *newer, int32_t *t) {
    if (b->count < 2) return 0;
    int64_t moment = ((int64_t)frame << 16) - (b->delay > 0 ? b->delay : 0);
    int first = b->count - 2; /* past the newest: the two newest */
    for (int i = 0; i + 1 < b->count; i++) {
        if (moment < ((int64_t)b->update[i + 1].stamp << 16)) {
            first = i;
            break;
        }
    }
    const RrBetweenUpdate *a = &b->update[first], *z = &b->update[first + 1];
    if (!a->good || !z->good || z->stamp <= a->stamp) return 0;
    int64_t place = (moment - ((int64_t)a->stamp << 16)) / (int64_t)(z->stamp - a->stamp);
    if (place < 0) place = 0;
    if (place > 0x20000) place = 0x20000;
    *older = first;
    *newer = first + 1;
    *t = (int32_t)place;
    return 1;
}

static int fall_back(RrBetween *b, int why) {
    b->fell_back[why]++;
    return 0;
}

int rr_between_scene(RrBetween *b, int older, int newer, int32_t t, const RrObjectSet *on_screen,
                     const RrVdp *v, const RrRoadTracker *screen, const uint8_t *rom, size_t rom_size, int scale) {
    static _Thread_local RrFineRoad fine_road;
    static _Thread_local RrRoad whole_road;
    static _Thread_local RrRoadShifts whole_shifts;
    const RrBetweenUpdate *a = &b->update[older], *z = &b->update[newer];
    /* The road, projected from the camera part-way. */
    RrRoadInputs inputs;
    if (!rr_road_inputs_between(&a->inputs, &z->inputs, z->ring, t, &inputs)
        || !rr_road_project(&inputs, rom, rom_size, &fine_road, &whole_road))
        return RR_BETWEEN_ROAD;
    rr_road_shifts(&whole_road, rom, rom_size, scale, &whole_shifts);
    rr_road_shifts_fine(&fine_road, &whole_shifts, &b->shifts);
    /* The horizon on screen is the game's: the video chip turns from sky and
     * hills to ground at the line the game last gave it, whatever is drawn
     * here. Where this road's top is not that line, the lines between are
     * drawn as the screen has them: hills above the horizon on screen (as
     * video memory holds them now), the road on screen below it. A line
     * counts as possibly having held road from the highest road top of the
     * two updates on, down to the last line of the road on screen (below
     * that are the dashboard and the mirrors, which are slid as the sky is). */
    int doubtful_from = RR_SCREEN_H, doubtful_to = 0;
    if (screen != NULL) {
        doubtful_from = a->road_top < z->road_top ? a->road_top : z->road_top;
        for (int line = 0; line < RR_SCREEN_H; line++) {
            if (b->shifts.have[line] && line < doubtful_from) doubtful_from = line;
            if (screen->whole.have[line]) doubtful_to = line;
            if (b->shifts.have[line] && !screen->whole.have[line]) b->shifts.have[line] = 0;
        }
    }
    memcpy(b->road_have, b->shifts.have, sizeof b->road_have);
    /* The lines the road does not reach (sky and hills), and what video memory holds now. */
    uint32_t table = (uint32_t)(v->reg[13] & 0x3F) << 10;
    const uint8_t *base = t < 0x10000 ? a->shifts : z->shifts;
    for (int line = 0; line < RR_SCREEN_H; line++) {
        for (int plane = 0; plane < 2; plane++) {
            int32_t *to = plane == 0 ? b->shifts.a : b->shifts.b, *expect = plane == 0 ? b->expect.a : b->expect.b;
            uint32_t at = (uint32_t)line * 4 + (uint32_t)plane * 2;
            expect[line * scale] = (int32_t)((uint32_t)(v->vram[(table + at) & 0xFFFF] << 8 | v->vram[(table + at + 1) & 0xFFFF]) << 16);
            if (b->shifts.have[line]) continue;
            int32_t *beyond = plane == 0 ? b->shifts.beyond_a : b->shifts.beyond_b;
            if (screen != NULL && screen->whole.have[line]) {
                /* Ground on screen that this road does not reach: the road on screen. */
                const int32_t *shown = plane == 0 ? screen->shifts.a : screen->shifts.b;
                const int32_t *further = plane == 0 ? screen->shifts.beyond_a : screen->shifts.beyond_b;
                for (int r = 0; r < scale; r++) {
                    to[line * scale + r] = shown[line * scale + r];
                    beyond[line * scale + r] = further[line * scale + r];
                }
                continue;
            }
            /* Not a line of this road: no edge of it lies beyond anything. */
            for (int r = 0; r < scale; r++) beyond[line * scale + r] = 0;
            if (line >= doubtful_from && line < doubtful_to) {
                /* Hills on screen where one of the roads was: as video memory holds the line now. */
                int32_t now = (int32_t)(((uint32_t)expect[line * scale] >> 16 & 0x3FF) << 16);
                for (int r = 0; r < scale; r++) to[line * scale + r] = now;
                continue;
            }
            int from = (a->shifts[at] << 8 | a->shifts[at + 1]) & 0x3FF, towards = (z->shifts[at] << 8 | z->shifts[at + 1]) & 0x3FF;
            int moved = ((towards - from + 0x200) & 0x3FF) - 0x200; /* the short way round */
            int64_t value;
            if (moved > RR_BETWEEN_JUMP_X || moved < -RR_BETWEEN_JUMP_X)
                value = (int64_t)((base[at] << 8 | base[at + 1]) & 0x3FF) << 16;
            else
                value = ((int64_t)from << 16) + (int64_t)moved * t;
            value &= 0x3FFFFFF; /* within the 1,024-pixel plane */
            for (int r = 0; r < scale; r++) to[line * scale + r] = (int32_t)value;
        }
    }
    for (int line = 0; line < RR_SCREEN_H; line++) b->shifts.have[line] = 1;
    b->expect.scale = scale;
    /* The objects part-way, and the sprite table that shows them. */
    /* The update before the pair, if kept, says how objects that leave the view were moving. */
    const uint8_t *before = NULL;
    int32_t carry = 0;
    if (older > 0 && b->update[older - 1].good && a->stamp > b->update[older - 1].stamp && z->stamp > a->stamp) {
        before = b->update[older - 1].object_ram;
        int64_t share = ((int64_t)(z->stamp - a->stamp) << 16) / (int64_t)(a->stamp - b->update[older - 1].stamp);
        carry = (int32_t)(share > 0x40000 ? 0x40000 : share);
    }
    b->margin_ok = 0;
    if (!rr_between_objects_fine(before, carry, a->object_ram, z->object_ram, rom, rom_size, t, &b->set, b->scratch,
                                 b->work, b->fx, b->fy))
        return RR_BETWEEN_OBJECTS;
    if (!rr_between_sprite_table(&b->set, on_screen, v, b->table, &b->fine)) return RR_BETWEEN_TABLE;
    /* The objects of the margins, from the same part-way image. */
    if (b->margin > 0) {
        int bottom = (int16_t)(b->scratch[0xF2B2] << 8 | b->scratch[0xF2B3]);
        if (rr_objects_margin(&b->set, b->scratch, b->scratch, rom, rom_size, b->margin, bottom, &b->margin_set,
                              b->margin_table, &b->margin_fine, b->work)) {
            for (int i = 0; i < b->margin_set.pictures; i++) {
                unsigned slot = b->margin_set.picture_record[i];
                if (slot >= RR_BETWEEN_RECORDS) continue;
                b->margin_set.picture[i].fx = b->fx[slot];
                b->margin_set.picture[i].fy = b->fy[slot];
            }
            b->margin_ok = 1;
        } else {
            b->margin_fell_back++;
        }
    }
    /* The centre-line dashes follow the road, in place of the sprites the game made of them. */
    if (a->dashes && z->dashes) {
        rr_dashes_strips_wide(&fine_road, inputs.place, b->road_have, scale, b->margin > 0 ? b->margin : 0, &b->strips);
        rr_dashes_mark(b->table, b->set.sprites, b->fine.skip);
        b->fine.strips = &b->strips;
    }
    b->t = t;
    b->older = older;
    b->newer = newer;
    return -1;
}

int rr_between_frame(RrBetween *b, uint64_t frame, const RrRoadTracker *road,
                     const RrObjectTracker *objects, const RrVdp *v, const uint8_t *rom,
                     size_t rom_size, int scale) {
    if (b->mode == RR_BETWEEN_OFF) return 0;
    b->frames++;
    const RrObjectSet *on_screen = rr_object_tracker_chosen(objects);
    int older, newer;
    int32_t t;
    if (on_screen == NULL || !road->have_chosen || !rr_between_clock(b, frame, &older, &newer, &t))
        return fall_back(b, RR_BETWEEN_NO_PAIR);
    int why = rr_between_scene(b, older, newer, t, on_screen, v, road, rom, rom_size, scale);
    if (why >= 0) return fall_back(b, why);
    b->between_frames++;
    if (t > 0x10000) b->guessed_frames++;
    return 1;
}

/* Whether the scene just made shows this road on every row of every road line. */
int rr_between_same_road(const RrBetween *b, const RrRoadShifts *road) {
    int scale = road->scale;
    for (int line = 0; line < RR_SCREEN_H; line++) {
        if (b->road_have[line] != road->have[line]) return 0;
        if (!road->have[line]) continue;
        if (memcmp(&b->shifts.a[line * scale], &road->a[line * scale], (size_t)scale * sizeof(int32_t)) != 0) return 0;
        if (memcmp(&b->shifts.b[line * scale], &road->b[line * scale], (size_t)scale * sizeof(int32_t)) != 0) return 0;
    }
    return 1;
}

/* Whether the scene's objects are this set: every sprite entry (the link aside) and every picture. */
int rr_between_same_objects(const RrObjectSet *made, const RrObjectSet *set) {
    if (!made->ok || !set->ok || made->sprites != set->sprites || made->first_sprite != set->first_sprite) return 0;
    if (made->pictures != set->pictures) return 0;
    for (int e = 0; e < set->sprites; e++)
        for (int k = 0; k < 8; k++)
            if (k != 3 && made->list[8 * e + k] != set->list[8 * e + k]) return 0;
    if (memcmp(made->picture, set->picture, (size_t)set->pictures * sizeof set->picture[0]) != 0) return 0;
    return memcmp(made->fine.picture, set->fine.picture, sizeof set->fine.picture) == 0 &&
           memcmp(made->fine.dx, set->fine.dx, sizeof set->fine.dx) == 0;
}

/* Whether the scene's sprite table is video memory's own chain, entry for entry. */
int rr_between_same_table(const RrBetween *b, const RrVdp *v) {
    uint32_t table = (uint32_t)(v->reg[5] & 0x7E) << 9;
    unsigned index = 0, made = 0;
    for (int visited = 0; visited < RR_SPRITES_MAX; visited++) {
        uint32_t s = table + index * 8;
        for (uint32_t k = 0; k < 8; k++)
            if (k != 3 && b->table[made * 8 + k] != v->vram[(s + k) & 0xFFFF]) return 0;
        unsigned link = v->vram[(s + 3) & 0xFFFF] & 0x7F, made_link = b->table[made * 8 + 3] & 0x7F;
        if (link == 0 || link >= RR_SPRITES_MAX) return made_link == 0;
        if (made_link != made + 1) return 0;
        made = made_link;
        index = link;
    }
    return 1;
}
