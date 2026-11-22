/* The game's object drawing (0xA4EE), routine for routine as
 * src/rr2port/objects.py has it. The arithmetic keeps the 68000's widths and
 * oddities, because the result is compared with the game's for equality.
 * Each routine names the 68000 address it follows. */
#include "rr_objects.h"

#include <string.h>

enum {
    PICTURE_TABLE = 0x56A0A, /* ROM: pointers to object pictures */
    SPRITE_SHAPES = 0x7C58,  /* ROM: per sprite shape, height in pixels, size bits, tiles, width */
    COPY_SPACING = 0x16E2C,  /* ROM: irregular spacing for copies of an object */
    DRAW_RECORDS = 0xCEB8, DRAW_RECORD_SIZE = 22, DRAW_ORDER = 0x08D6,
    MAX_SPRITES = 0x50, MAX_TILES = 0x150,
    VIEW_TOP = 0,
    MAX_STEPS = 1032 /* a 255-pixel picture at four times its size makes 1,024 */
};

typedef struct {
    uint8_t *ram; /* the copy being worked on */
    const uint8_t *rom;
    size_t rom_size;
    RrObjectSet *out;
    uint32_t list, buffer;
    unsigned next_tile, next_sprite, first_tile, kept_tile, kept_sprite;
    int kept_pictures;
    /* The record being drawn. */
    uint32_t record;
    unsigned slot; /* its number in the table of draw records */
    unsigned x, y, size, hidden, flags, palette, sprite_word;
    int mirror;
    unsigned across, down, clip, skip_rows;
    unsigned failed, out_of_room;
    int below; /* a picture of the record was wholly below its hidden line */
    /* The view: pictures are cut to [view_left, view_right); the game stops
     * `short_right` pixels short of its own right edge; without `use_tiles`
     * there is no tile budget and no tile is written. */
    int view_left, view_right, short_right, use_tiles;
    /* Left by the last picture drawn. */
    int produced, skipped, stride;
    unsigned groups, gap, short_gap, band_gap;
    int32_t columns[MAX_STEPS + 8], rows[MAX_STEPS + 8];
    int column_count, row_count;
} Drawer;

static int s16(unsigned value) {
    value &= 0xFFFF;
    return (value & 0x8000) ? (int)value - 0x10000 : (int)value;
}

static int s8(unsigned value) {
    value &= 0xFF;
    return (value & 0x80) ? (int)value - 0x100 : (int)value;
}

/* The 68000's N flag for a word result. */
static int negative(unsigned value) { return (value & 0x8000) != 0; }

/* value x size, size being the record's size x 4 (0x10000 = natural), as the game does. */
static unsigned scaled(unsigned value, unsigned size) {
    uint64_t product = (uint64_t)(value & 0xFFFF) * ((size >> 2) & 0xFFFF);
    return (unsigned)(((product << 2) >> 16) & 0xFFFF);
}

static unsigned romb(const Drawer *d, uint64_t address) {
    return address < d->rom_size ? d->rom[address] : 0;
}

static unsigned romw(const Drawer *d, uint64_t address) {
    return romb(d, address) << 8 | romb(d, address + 1);
}

static uint32_t roml(const Drawer *d, uint64_t address) {
    return (uint32_t)romw(d, address) << 16 | romw(d, address + 2);
}

static unsigned rb(const Drawer *d, uint32_t address) { return d->ram[address & 0xFFFF]; }
static void wb(Drawer *d, uint32_t address, unsigned value) { d->ram[address & 0xFFFF] = (uint8_t)value; }

static unsigned rw(const Drawer *d, uint32_t address) {
    return rb(d, address) << 8 | rb(d, address + 1);
}

static void ww(Drawer *d, uint32_t address, unsigned value) {
    wb(d, address, value >> 8);
    wb(d, address + 1, value);
}

static uint32_t rl(const Drawer *d, uint32_t address) {
    return (uint32_t)rw(d, address) << 16 | rw(d, address + 2);
}

static uint32_t picture_address(const Drawer *d, int index) {
    /* A negative index reads before the table, as the game would; never outside the ROM. */
    int64_t at = (int64_t)PICTURE_TABLE + 4 * (int64_t)index;
    return at < 0 ? 0 : roml(d, (uint64_t)at);
}

/* How far to move through a source row after each output byte (0xA21C). A
 * byte is two pixels. Sets columns, produced, skipped, groups and stride.
 * Returns 0 if the steps do not fit (no real picture comes near). */
static int column_steps(Drawer *d, unsigned width, unsigned size, unsigned clip) {
    int n = 0, pending = 0, made = 0, skip, keep, stopped = 0;
    uint32_t total = 0;
    if (clip == 0) {
        skip = 0;
        keep = 0x7FFF;
    } else if (negative(clip)) {
        skip = s16((unsigned)-(int)clip);
        keep = 0x7FFF;
    } else {
        skip = 0;
        keep = s16(clip);
    }
    d->skipped = skip > 0 ? skip : 0;
    int left = (int)(width >> 1) + (int)(width & 1) + 1;
    d->stride = left;
    left -= 2;
    for (;;) {
        pending += 1;
        total += size;
        while (total >= 0x10000) {
            skip -= 1;
            if (skip < 0) {
                if (n >= MAX_STEPS) return 0;
                d->columns[n++] = pending;
                pending = 0;
                made += 1;
                keep -= 1;
                if (keep < 0) {
                    pending = left & 0xFFFF;
                    stopped = 1;
                    break;
                }
            }
            total -= 0x10000;
        }
        if (stopped) break;
        left -= 1;
        if (left == -1) break;
        if (left < -1) return 0; /* a picture with no width: the game's count would run on */
    }
    d->produced = made;
    d->columns[n++] = pending;
    if ((made & 3) != 3) {
        for (int i = 3 - (made & 3); i > 0; i--) {
            d->columns[n++] = 0;
            made += 1;
        }
    }
    d->column_count = n;
    d->groups = made == 0 ? 0xFFFF : (unsigned)(((((made + 1) & 0xFFFF) >> 2) - 1) & 0xFFFF);
    return 1;
}

/* How far to move through the picture after each output row (0xA2CA). */
static int row_steps(Drawer *d, unsigned height, int stride, unsigned size, unsigned skip_word) {
    int n = 0, pending = 1, skip = s16(skip_word);
    int left = height > 0 ? (int)height - 1 : 0;
    uint32_t total = 0;
    for (;;) {
        if (skip > 0) {
            skip -= 1;
            pending += stride;
        } else {
            total += size;
            if (total < 0x10000) {
                pending += stride;
            } else {
                for (;;) {
                    if (n >= MAX_STEPS) return 0;
                    d->rows[n++] = pending;
                    pending = 1;
                    total -= 0x10000;
                    if (total < 0x10000) break;
                    d->rows[n - 1] -= stride;
                }
            }
        }
        left -= 1;
        if (left == -1) break;
    }
    d->row_count = n;
    return 1;
}

static void anchor(const Drawer *d, uint32_t picture, unsigned *across, unsigned *down) { /* 0xA9A0 */
    unsigned a = (unsigned)s8(romb(d, picture)) & 0xFFFF;
    unsigned b = (unsigned)s8(romb(d, picture + 1)) & 0xFFFF;
    if (d->mirror) a = (unsigned)(s8(romb(d, (uint64_t)picture + 2)) - s16(a)) & 0xFFFF;
    *across = scaled(a, d->size);
    *down = scaled(b, d->size);
}

static unsigned clip_amount(int amount) {
    int odd = amount & 1;
    return (unsigned)((s16((unsigned)amount) >> 1) + odd) & 0xFFFF;
}

/* Clip an anchored picture against the view's sides (0xA9E4). Returns the
 * anchor's distance from the left of what is drawn; *clip is for column_steps. */
static unsigned edges(Drawer *d, uint32_t picture, unsigned across, unsigned *clip) {
    unsigned width = scaled((unsigned)s8(romb(d, (uint64_t)picture + 2)) & 0xFFFF, d->size);
    unsigned x = rw(d, d->record);
    unsigned limit = (unsigned)(d->mirror ? d->view_right : d->view_right - d->short_right) & 0xFFFF;
    unsigned over = (width - across + x - limit) & 0xFFFF;
    if (!negative(over)) {
        if (!negative(over - width)) {
            d->failed = 1;
            *clip = over;
            return across;
        }
        *clip = clip_amount(d->mirror ? -(int)over : (int)width - (int)over);
        return across;
    }
    unsigned under = (x - across - (unsigned)d->view_left) & 0xFFFF;
    if (!negative(under)) {
        *clip = 0;
        return across;
    }
    if (!negative(((0u - under) & 0xFFFF) - width)) {
        d->failed = 1;
        *clip = (0u - under) & 0xFFFF;
        return across;
    }
    across = (across + under) & 0xFFFF;
    *clip = clip_amount(d->mirror ? (int)under + (int)width : (int)under);
    return across;
}

/* Place and clip a picture without an anchor: centred on x, standing on y (0xAA78). */
static void centre(Drawer *d, uint32_t picture) {
    unsigned width = scaled(romb(d, picture), d->size);
    d->across = width >> 1;
    d->down = scaled(romb(d, (uint64_t)picture + 1), d->size);
    unsigned x = rw(d, d->record);
    unsigned over = (d->across + x - (unsigned)(d->view_right - d->short_right)) & 0xFFFF;
    if (!negative(over)) {
        if (!negative(over - width)) {
            d->failed = 1;
            return;
        }
        int amount = d->mirror ? -(int)over : (int)width - (int)over;
        d->clip = (unsigned)(s16((unsigned)amount) >> 1) & 0xFFFF;
        return;
    }
    unsigned under = (x - (width >> 1) - (unsigned)d->view_left) & 0xFFFF;
    if (!negative(under)) {
        d->clip = 0;
        return;
    }
    if (!negative(((0u - under) & 0xFFFF) - width)) {
        d->failed = 1;
        return;
    }
    d->x = (unsigned)d->view_left & 0xFFFF;
    d->across = 0;
    int amount = d->mirror ? (int)under + (int)width : (int)under;
    d->clip = (unsigned)(s16((unsigned)amount) >> 1) & 0xFFFF;
}

/* Cut a box of whole tiles into sprites of at most 32x32 and add them to the list (0x7A68). */
static void sprites(Drawer *d, unsigned width, unsigned height) {
#define SHAPE(offset, which) romw(d, (uint64_t)SPRITE_SHAPES + (offset) + (which))
    d->band_gap = 0x8000;
    uint32_t entry = d->list + d->next_sprite * 8;
    if (d->next_sprite != 0) {
        if (d->next_sprite >= MAX_SPRITES) {
            d->out_of_room = d->failed = 0xFFFF;
            return;
        }
        ww(d, entry - 6, rw(d, entry - 6) | d->next_sprite);
    }
    int across = (int)(width >> 5);
    unsigned last_width = width & 0x18;
    if (last_width == 0) {
        across -= 1;
        last_width = 0x20;
    }
    last_width -= 8;
    int down = (int)(height >> 5);
    unsigned last_height = height & 0x18;
    if (last_height == 0) {
        down -= 1;
        last_height = 0x20;
    }
    last_height = (last_height - 8) << 2;
    int per_row = across;
    d->gap = 0x7C;
    d->short_gap = last_height + 0x20 - 4;
    if (down == 0) d->gap = d->short_gap;
    unsigned y = (d->y + 0x80) & 0xFFFF;
    unsigned link = d->next_sprite + 1;
    unsigned tile = d->next_tile;
    for (;;) {
        unsigned band_start = tile;
        unsigned x = (d->x + 0x80) & 0xFFFF;
        if (d->mirror) x = (x + 2 * (unsigned)d->produced) & 0xFFFF;
        unsigned tall = down == 0 ? last_height : 0x60;
        unsigned offset = 0;
        for (;;) {
            offset = (across == 0 ? last_width : 0x18) + tall;
            link |= SHAPE(offset, 2);
            ww(d, entry, y);
            ww(d, entry + 2, link);
            unsigned word = negative(y - 0x118) ? tile | 0x8000 : tile;
            word = ((word | d->sprite_word) + d->first_tile) & 0xFFFF;
            if (d->mirror) {
                word |= 0x800;
                x = (x - SHAPE(offset, 6)) & 0xFFFF;
            }
            ww(d, entry + 4, word);
            ww(d, entry + 6, x);
            entry += 8;
            link = (link & 0xFF) + 1;
            tile = (tile + SHAPE(offset, 4)) & 0xFFFF;
            if (d->use_tiles && !negative(tile - MAX_TILES)) {
                d->out_of_room = d->failed = 0xFFFF;
                if (d->next_sprite != 0) {
                    uint32_t last = d->list + d->next_sprite * 8;
                    ww(d, last - 6, rw(d, last - 6) & 0xFF00);
                }
                return;
            }
            if (!d->mirror) x = (x + SHAPE(offset, 6)) & 0xFFFF;
            across -= 1;
            if (across == -1) break;
        }
        y = (y + SHAPE(offset, 0)) & 0xFFFF;
        across = per_row;
        if (d->band_gap == 0x8000) d->band_gap = ((tile - band_start - 4) << 5) & 0xFFFF;
        down -= 1;
        if (down == -1) break;
    }
    ww(d, entry - 6, rw(d, entry - 6) & 0xFF00);
    d->next_sprite = link - 1;
    d->next_tile = tile;
#undef SHAPE
}

/* Copy the scaled picture into the tile buffer (0xA3CA). Tiles run down each
 * sprite's columns, so the next 8 pixels of a row are a whole column of tiles
 * further on. Returns where the row after the last would go. */
static uint32_t copy(Drawer *d, uint32_t source, unsigned tile_offset, unsigned last_row) {
    uint32_t at = d->buffer + (uint32_t)s16(tile_offset);
    int rows_left_in_band = 0x1F;
    unsigned gap = d->gap;
    int count = (int)last_row + 1;
    for (int r = 0; r < count; r++) {
        uint32_t to = at;
        int step = 0;
        for (unsigned group = 0; group <= d->groups; group++) {
            if (group) to += gap;
            for (int i = 0; i < 4; i++) {
                wb(d, to, romb(d, source));
                to += 1;
                source += (uint32_t)s16((unsigned)d->columns[step]);
                step += 1;
            }
        }
        at += 4;
        rows_left_in_band -= 1;
        if (rows_left_in_band == -1) {
            if (count - 1 - r < 0x1F) gap = d->short_gap;
            rows_left_in_band = 0x1F;
            at += (uint32_t)s16(d->band_gap);
        }
        if (r < d->row_count) source += (uint32_t)s16((unsigned)d->rows[r]);
    }
    return at;
}

/* Note which picture the sprite entries [from, to) show. */
static void describe(Drawer *d, uint32_t data, unsigned from, unsigned to, unsigned last_row) {
    RrObjectSet *out = d->out;
    unsigned width = romb(d, data), height = romb(d, (uint64_t)data + 1);
    uint64_t end = (uint64_t)data + 4 + (uint64_t)d->stride * height;
    for (unsigned i = from; i < to && i < RR_SPRITES_MAX; i++) out->fine.picture[i] = -1;
    if (out->pictures >= RR_SPRITES_MAX || end > d->rom_size || d->produced < 0 || width == 0 || height == 0) return;
    RrFinePicture *p = &out->picture[out->pictures];
    p->rows = d->rom + data + 4;
    /* The game copies whole bytes, so a picture of odd width shows its last
     * byte's second pixel too (it is see-through in the ROM's pictures). */
    width = (width + 1) & ~1u;
    p->width = (uint16_t)width;
    p->height = (uint16_t)height;
    p->stride = (uint16_t)d->stride;
    p->size = d->size;
    p->mirrored = (uint8_t)d->mirror;
    p->x = (int16_t)s16(d->mirror ? d->x + 2 * (unsigned)d->produced : d->x);
    p->y = (int16_t)s16(d->y);
    p->first_column = (uint16_t)(2 * d->skipped);
    p->first_row = (uint16_t)d->skip_rows;
    /* The game makes whole pixel pairs, rounding down, so up to two pixels of
     * the picture's far side are lost (and where it cuts a picture at the
     * view's edge it stops four pixels short of the screen's). The picture
     * itself reaches to its true scaled width; the sprites, which always have
     * room for one more pair, and the screen's edge end it. */
    int64_t full = (((int64_t)width * d->size + 0xFFFF) >> 16) - 2 * d->skipped;
    int shown = 2 * d->produced;
    if (full > shown) shown = (int)full;
    /* (A picture scaled to less than one pixel pair still gets its sprite
     * from the game, showing the picture's first pair; here it is the whole
     * picture, however small.) */
    if (shown <= 0) return;
    p->columns = (uint16_t)shown;
    p->rows_shown = (uint16_t)(last_row + 1);
    for (unsigned i = from; i < to && i < RR_SPRITES_MAX; i++) {
        out->fine.picture[i] = (int16_t)out->pictures;
        out->fine.dx[i] = 0;
    }
    out->picture_record[out->pictures] = (uint16_t)d->slot;
    out->pictures++;
}

/* Scale the picture whose width byte is at `data` and stage its sprites (0xA08E). */
static void draw(Drawer *d, uint32_t data) {
    unsigned tile_offset = (d->next_tile << 5) & 0xFFFF;
    if (!column_steps(d, romb(d, data), d->size, d->clip)) {
        d->out_of_room = d->failed = 0xFFFF;
        return;
    }
    if (negative(d->groups)) {
        d->failed = 1;
        return;
    }
    if (!row_steps(d, romb(d, (uint64_t)data + 1), d->stride, d->size, d->skip_rows)) {
        d->out_of_room = d->failed = 0xFFFF;
        return;
    }
    unsigned last_row = (unsigned)(d->row_count - 1) & 0xFFFF;
    if (!(s16(last_row) > 0)) {
        d->failed = 1;
        return;
    }
    d->sprite_word = d->palette; /* the picture's own colour-set word is never used */
    d->x = (d->x - d->across) & 0xFFFF;
    d->y = (d->y - d->down) & 0xFFFF;
    if (!negative(d->y - d->hidden)) { /* wholly below the line a hill hides it under */
        d->below = 1;
        return;
    }
    unsigned height = (last_row + 1) & 0xFFFF;
    unsigned bottom = (height + d->y) & 0xFFFF;
    if (!negative(bottom - d->hidden)) {
        unsigned cut = (bottom - d->hidden) & 0xFFFF;
        last_row = (last_row - cut) & 0xFFFF;
        height = (height - cut) & 0xFFFF;
    }
    unsigned tiles_high = (height & 0xFFF8) + ((height & 7) ? 8 : 0);
    unsigned first = d->next_sprite;
    sprites(d, ((d->groups + 1) << 3) & 0xFFFF, tiles_high);
    if (d->failed == 0xFFFF) return;
    describe(d, data, first, d->next_sprite, last_row);
    if (!d->use_tiles) return;
    uint32_t at = copy(d, data + 4, tile_offset, last_row);
    /* Blank the rows left over in the last row of tiles. */
    int spare = 7 - (int)((last_row + 1) & 7);
    if (spare == 7) return;
    for (int i = 0; i <= spare; i++) {
        uint32_t to = at;
        for (unsigned group = 0; group <= d->groups; group++) {
            if (group) to += d->short_gap;
            for (int k = 0; k < 4; k++) wb(d, to + (uint32_t)k, 0);
            to += 4;
        }
        at += 4;
    }
}

/* Extra copies of an object: its sprite entries repeated with a sideways step (0xA8AA). */
static void copies(Drawer *d, uint32_t record, unsigned first) {
    RrObjectSet *out = d->out;
    unsigned count = rb(d, record + 0x0A) & 7;
    if (count == 0 || d->clip != 0) return;
    int made = (int)d->next_sprite - (int)first;
    if (made <= 0) return;
    uint32_t source = d->list + first * 8;
    unsigned index = d->next_sprite;
    uint32_t to = d->list + index * 8;
    int irregular = (d->flags & 0x0020) != 0;
    uint32_t spacing = irregular ? roml(d, (uint64_t)COPY_SPACING + (rw(d, record + 0x14) & 7) * 4) : 0;
    int offset = 0;
#define STEP()                                                              \
    do {                                                                    \
        offset = s16(rw(d, record + 0x0C));                                 \
        if (irregular) {                                                    \
            int part = offset, shift = s16(romw(d, spacing));               \
            spacing += 2;                                                   \
            if (shift < 0) {                                                \
                shift = -shift;                                             \
                part = -part;                                               \
            }                                                               \
            offset += (int)((int64_t)s16((unsigned)part) >> (shift & 63)); \
        }                                                                   \
    } while (0)
    STEP();
    for (unsigned n = 0; n < count; n++) {
        for (int i = 0; i < made; i++) {
            wb(d, to - 5, index & 0xFF);
            uint8_t bytes[8];
            for (int k = 0; k < 8; k++) bytes[k] = (uint8_t)rb(d, source + (uint32_t)k);
            for (int k = 0; k < 8; k++) wb(d, to + (uint32_t)k, bytes[k]);
            uint32_t from_entry = (source - d->list) / 8, to_entry = (to - d->list) / 8;
            if (to_entry < RR_SPRITES_MAX) {
                int known = from_entry < RR_SPRITES_MAX;
                out->fine.picture[to_entry] = known ? out->fine.picture[from_entry] : -1;
                out->fine.dx[to_entry] = (int16_t)((known ? out->fine.dx[from_entry] : 0) + offset);
            }
            source += 8;
            to += 8;
            ww(d, to - 2, rw(d, to - 2) + (unsigned)offset);
            unsigned x = rw(d, to - 2);
            unsigned width = ((rb(d, to - 6) & 0x0C) << 1) + 8;
            int inside = !negative((unsigned)(d->view_right + 0x80) - width - x)
                         && negative((unsigned)(d->view_left + 0x80) - x);
            if (!inside) {
                source -= 8;
                to -= 8;
                continue;
            }
            index += 1;
            if (index >= MAX_SPRITES) {
                wb(d, to - 5, 0);
                return;
            }
        }
        d->next_sprite = index;
        if (irregular && n != count - 1) STEP();
    }
    wb(d, to - 5, 0);
#undef STEP
}

static void outcome(Drawer *d, int what) {
    if (d->slot < RR_OBJECT_RECORDS) d->out->outcome[d->slot] = (uint8_t)what;
}

static void drop(Drawer *d, int why) {
    d->out->dropped++;
    outcome(d, why);
}

/* Forget what a dropped record described. */
static void forget(Drawer *d) {
    d->out->pictures = d->kept_pictures;
    for (unsigned i = d->kept_sprite; i < RR_SPRITES_MAX; i++) d->out->fine.picture[i] = -1;
}

static void draw_record(Drawer *d, uint32_t record) { /* 0xA5C8 */
    d->out->records++;
    d->failed = 0;
    d->below = 0;
    d->record = record;
    d->x = rw(d, record);
    d->y = rw(d, record + 2);
    unsigned start_x = d->x, start_y = d->y;
    d->size = rw(d, record + 4) << 2;
    if (d->size < 4) d->size = 4;
    d->hidden = rw(d, record + 6);
    unsigned flags = d->flags = rw(d, record + 0x0A);
    d->mirror = (flags & 0x8000) != 0;
    d->palette = ((flags & 0x3000) << 1) & 0xFFFF;
    unsigned second = rw(d, record + 0x12);
    uint32_t picture = picture_address(d, s16(rw(d, record + 8) << 2) >> 2);
    uint32_t data, later = 0; /* later: the second picture, drawn after the first */
    unsigned across2 = 0, down2 = 0, clip2 = 0;
    d->skip_rows = 0;
    if (flags & 0x0080) {
        for (;;) {
            d->clip = 0;
            unsigned across, down;
            anchor(d, picture, &across, &down);
            d->down = down;
            d->across = edges(d, picture, across, &d->clip);
            if (!d->failed) break;
            /* The first picture is wholly off the screen: try the second alone. */
            d->failed = 0;
            if (second == 0) {
                drop(d, RR_RECORD_OFF_VIEW);
                return;
            }
            picture = picture_address(d, (int)second);
            second = 0;
        }
        data = picture + 2;
        if (second != 0) {
            uint32_t other = picture_address(d, (int)second);
            anchor(d, other, &across2, &down2);
            uint32_t second_data = other + 2;
            down2 = (down2 - 1) & 0xFFFF;
            across2 = edges(d, second_data, across2, &clip2);
            later = second_data;
            if (d->failed) {
                d->failed = 0;
                later = 0;
            }
            /* Rows of the first picture that the second one covers are skipped. */
            unsigned covered = (romb(d, (uint64_t)second_data - 1) - romb(d, (uint64_t)second_data + 1)) & 0xFF;
            covered = ((down2 & 0xFF00) | covered) + 1;
            int rows = s8(romb(d, (uint64_t)data - 1) - (covered & 0xFF));
            if (rows >= 0) {
                d->skip_rows = (unsigned)rows;
                d->down = (d->down - scaled((unsigned)rows, d->size)) & 0xFFFF;
            }
        }
    } else {
        data = picture;
        centre(d, picture);
        if (d->failed) {
            drop(d, RR_RECORD_OFF_VIEW);
            return;
        }
    }
    unsigned first_sprite = d->next_sprite;
    for (;;) {
        d->out_of_room = 0;
        int wanted = 1;
        unsigned above = (d->y - d->down - VIEW_TOP) & 0xFFFF;
        if (negative(above)) {
            /* Part of it is above the view: skip the source rows that would land there. */
            above = (0u - above) & 0xFFFF;
            uint32_t quotient = ((uint32_t)above << 16) / ((d->size >> 2) & 0xFFFF);
            unsigned rows = (quotient & 0xFFFF) >> 2;
            if (!negative(rows - d->skip_rows)) {
                unsigned more = (rows - d->skip_rows) & 0xFFFF;
                d->skip_rows = rows;
                d->down = (d->down - scaled(more, d->size)) & 0xFFFF;
                if (!(s16(d->down) > 0)) wanted = 0;
            }
        }
        if (wanted) draw(d, data);
        if (!wanted || d->out_of_room) {
            d->next_tile = d->kept_tile;
            d->next_sprite = d->kept_sprite;
            forget(d);
            drop(d, wanted ? RR_RECORD_NO_ROOM : RR_RECORD_OFF_VIEW);
            return;
        }
        if (later == 0) break;
        data = later;
        d->x = start_x;
        d->y = start_y;
        d->across = across2;
        d->down = down2;
        d->clip = clip2;
        d->skip_rows = 0;
        later = 0;
    }
    outcome(d, d->next_sprite != first_sprite ? RR_RECORD_DRAWN : d->below ? RR_RECORD_HIDDEN : RR_RECORD_NO_PICTURE);
    copies(d, record, first_sprite);
}

int rr_objects_draw(const uint8_t *ram, const uint8_t *rom, size_t rom_size, RrObjectSet *out,
                    uint8_t *work) {
    static const RrObjectView game = {0, 0x140};
    return rr_objects_draw_view(ram, rom, rom_size, &game, 1, out, work);
}

int rr_objects_draw_view(const uint8_t *ram, const uint8_t *rom, size_t rom_size,
                         const RrObjectView *view, int tiles, RrObjectSet *out, uint8_t *work) {
    static _Thread_local Drawer drawer;
    Drawer *d = &drawer;
    memset(d, 0, sizeof *d);
    d->view_left = view->left;
    d->view_right = view->right;
    d->short_right = view->left == 0 && view->right == 0x140 ? 4 : 0;
    d->use_tiles = tiles != 0;
    memset(out, 0, sizeof *out);
    out->fine.pictures = out->picture;
    for (int i = 0; i < RR_SPRITES_MAX; i++) out->fine.picture[i] = -1;
    memcpy(work, ram, 0x10000);
    d->ram = work;
    d->rom = rom;
    d->rom_size = rom_size;
    d->out = out;
    d->next_tile = rw(d, 0xDC30);
    d->next_sprite = rw(d, 0xDC34);
    d->list = rl(d, 0x27A6);
    d->buffer = rl(d, 0x27CE);
    d->first_tile = rw(d, 0xCE86);
    /* Not covered: a ROM without the tables, or counters already past their limits. */
    if (rom_size < PICTURE_TABLE + 4 || d->next_sprite > MAX_SPRITES || d->next_tile > MAX_TILES) return 0;
    unsigned first_sprite = d->next_sprite, first_tile = d->next_tile;
    for (int k = 0; k < 128; k++) {
        unsigned index = rw(d, DRAW_ORDER + 2 * (uint32_t)k);
        if (index == 0xFFFF) break;
        d->kept_tile = d->next_tile;
        d->kept_sprite = d->next_sprite;
        d->kept_pictures = out->pictures;
        d->slot = index;
        draw_record(d, DRAW_RECORDS + DRAW_RECORD_SIZE * index);
    }
    ww(d, 0xDC30, d->next_tile);
    ww(d, 0xDC34, d->next_sprite);
    out->first_sprite = (int)first_sprite;
    out->sprites = d->next_sprite < RR_SPRITES_MAX ? (int)d->next_sprite : RR_SPRITES_MAX;
    out->first_tile = (int)first_tile;
    out->tiles = d->next_tile < RR_OBJECT_TILES ? (int)d->next_tile : RR_OBJECT_TILES;
    out->tile_base = (uint16_t)d->first_tile;
    for (int i = out->sprites; i < RR_SPRITES_MAX; i++) out->fine.picture[i] = -1;
    for (uint32_t i = 0; i < sizeof out->list; i++) out->list[i] = (uint8_t)rb(d, d->list + i);
    for (uint32_t i = 0; i < sizeof out->tile_bytes; i++) out->tile_bytes[i] = (uint8_t)rb(d, d->buffer + i);
    out->ok = 1;
    return 1;
}

/* The screen pixels a picture stands on: [*left, *right). */
static void picture_span(const RrFinePicture *p, int *left, int *right) {
    if (p->mirrored) {
        *right = p->x;
        *left = p->x - p->columns;
    } else {
        *left = p->x;
        *right = p->x + p->columns;
    }
}

int rr_objects_margin(const RrObjectSet *narrow, const uint8_t *narrow_ram, const uint8_t *wide_ram,
                      const uint8_t *rom, size_t rom_size, int margin, int bottom,
                      RrObjectSet *out, uint8_t *table, RrFineSprites *fine, uint8_t *work) {
    /* The game stops a picture it cuts four pixels short of its right edge, and a
     * mirrored one at 315 or 316 as its pairs of pixels fall: from 315 on is the seam. */
    enum { SEAM = 0x140 - 5 };
    /* One pixel more than the margin each side: an entry moved by a pixel (below) still reaches the end. */
    const RrObjectView view = {(int16_t)(-margin - 1), (int16_t)(0x140 + margin + 1)};
    memset(table, 0, RR_SPRITES_MAX * 8);
    memset(fine, 0, sizeof *fine);
    fine->pictures = out->picture;
    fine->table = table;
    for (int i = 0; i < RR_SPRITES_MAX; i++) fine->picture[i] = -1;
    if (narrow == NULL || !narrow->ok) {
        memset(out, 0, sizeof *out);
        return 0;
    }
    if (!rr_objects_draw_view(wide_ram, rom, rom_size, &view, 0, out, work)) return 0;
    fine->pictures = out->picture;
    /* Which slots the narrow drawing went through, from its draw order. */
    uint8_t in_narrow[RR_OBJECT_RECORDS] = {0};
    for (int k = 0; k < 128; k++) {
        unsigned index = (unsigned)(narrow_ram[DRAW_ORDER + 2 * k] << 8 | narrow_ram[DRAW_ORDER + 2 * k + 1]);
        if (index == 0xFFFF) break;
        if (index < RR_OBJECT_RECORDS) in_narrow[index] = 1;
    }
    /* Each wide picture: is it one the narrow set shows cut (then it becomes that
     * picture uncut, moved by `moved`), one it shows whole, or one it does not show? */
    enum { NOT_SHOWN, WHOLE, CUT, LEFT_OUT };
    uint8_t state[RR_SPRITES_MAX], counted[RR_SPRITES_MAX] = {0};
    int moved[RR_SPRITES_MAX];
    for (int k = 0; k < out->pictures; k++) {
        RrFinePicture *p = &out->picture[k];
        unsigned slot = out->picture_record[k];
        state[k] = NOT_SHOWN;
        moved[k] = 0;
        if (p->y >= bottom) {
            state[k] = LEFT_OUT;
            continue;
        }
        if (slot >= RR_OBJECT_RECORDS || !in_narrow[slot]) continue;
        if (narrow->outcome[slot] == RR_RECORD_OFF_VIEW) continue;
        if (narrow->outcome[slot] != RR_RECORD_DRAWN) {
            state[k] = LEFT_OUT; /* no room, nothing to draw, or hidden */
            continue;
        }
        for (int n = 0; n < narrow->pictures; n++) {
            const RrFinePicture *q = &narrow->picture[n];
            if (narrow->picture_record[n] != slot || q->rows != p->rows) continue;
            int left, right;
            picture_span(q, &left, &right);
            if (q->first_column == 0 && left > 0 && right < SEAM) {
                state[k] = WHOLE;
                break;
            }
            /* The narrow picture with what was cut off put back. */
            /* Where the wide drawing's own picture would stand uncut (the wide view may have cut it too). */
            int was = p->mirrored ? p->x + p->first_column : p->x - p->first_column;
            *p = *q;
            p->x = (int16_t)(q->mirrored ? q->x + q->first_column : q->x - q->first_column);
            p->columns = (uint16_t)(q->columns + q->first_column);
            p->first_column = 0;
            moved[k] = p->x - was;
            state[k] = CUT;
            break;
        }
    }
    /* The entries: those that reach a margin (or the seam) and show something the
     * narrow picture does not. */
    int made = 0;
    for (int e = out->first_sprite; e < out->sprites && made < RR_SPRITES_MAX; e++) {
        int k = out->fine.picture[e];
        if (k < 0 || k >= out->pictures || state[k] == LEFT_OUT) continue;
        int dx = out->fine.dx[e];
        if (dx == 0 && state[k] == CUT) {
            /* The narrow picture put back whole: entries of its own over the parts of it
             * beyond the screen's sides and in the seam (the wide drawing's entries stand
             * where the wide drawing put the picture, which can be many pixels away). */
            if (counted[k]) continue;
            const RrFinePicture *p = &out->picture[k];
            int left, right;
            picture_span(p, &left, &right);
            for (int side = 0; side < 2; side++) {
                int from_x = side == 0 ? -margin : SEAM, to_x = side == 0 ? 0 : 0x140 + margin;
                if (left > from_x) from_x = left;
                if (right < to_x) to_x = right;
                for (int x = from_x; x < to_x; x += 32) {
                    for (int y = p->y; y < p->y + p->rows_shown && made < RR_SPRITES_MAX; y += 32) {
                        int wide = to_x - x > 32 ? 32 : to_x - x, high = p->y + p->rows_shown - y;
                        if (high > 32) high = 32;
                        uint8_t *to = &table[8 * made];
                        memcpy(to, &out->list[8 * e], 8); /* its colour set and priority */
                        to[0] = (uint8_t)((unsigned)(y + 0x80) >> 8 & 1);
                        to[1] = (uint8_t)(y + 0x80);
                        to[2] = (uint8_t)(((wide + 7) / 8 - 1) << 2 | ((high + 7) / 8 - 1));
                        to[3] = (uint8_t)(made + 1);
                        to[6] = (uint8_t)((unsigned)(x + 0x80) >> 8 & 1);
                        to[7] = (uint8_t)(x + 0x80);
                        fine->picture[made] = (int16_t)k;
                        fine->dx[made] = 0;
                        made++;
                        if (!counted[k]) {
                            out->margin_uncut++;
                            out->margin_pictures++;
                        }
                        counted[k] = 1;
                    }
                }
            }
            continue;
        }
        if (dx != 0) {
            /* A copy: the narrow set's own copies are on the screen already. */
            int has = 0;
            for (int n = narrow->first_sprite; n < narrow->sprites && !has; n++) {
                int q = narrow->fine.picture[n];
                has = q >= 0 && narrow->fine.dx[n] == dx && narrow->picture_record[q] == out->picture_record[k]
                      && narrow->picture[q].rows == out->picture[k].rows;
            }
            if (has) continue;
        } else if (state[k] == WHOLE) {
            continue;
        }
        const uint8_t *from = &out->list[8 * e];
        int x = (int16_t)((from[6] << 8 | from[7]) - 0x80) + moved[k];
        int wide = (((from[2] >> 2) & 3) + 1) * 8;
        if (!(x < 0 || x + wide > SEAM)) continue; /* wholly on the screen: the narrow picture's business */
        if (x + wide <= -margin || x >= 0x140 + margin) continue;
        uint8_t *to = &table[8 * made];
        memcpy(to, from, 8);
        to[3] = (uint8_t)(made + 1);
        to[6] = (uint8_t)((unsigned)(x + 0x80) >> 8);
        to[7] = (uint8_t)(x + 0x80);
        fine->picture[made] = (int16_t)k;
        fine->dx[made] = (int16_t)dx;
        made++;
        if (dx != 0) {
            out->margin_copies++;
        } else if (!counted[k]) {
            if (state[k] == CUT) out->margin_uncut++;
            else out->margin_off_view++;
        }
        if (!counted[k]) out->margin_pictures++;
        counted[k] = 1;
    }
    if (made > 0) table[8 * (made - 1) + 3] = 0;
    /* Records the wide drawing had no sprites left for, that the narrow one did not drop for room. */
    for (int k = 0; k < 128; k++) {
        unsigned index = (unsigned)(wide_ram[DRAW_ORDER + 2 * k] << 8 | wide_ram[DRAW_ORDER + 2 * k + 1]);
        if (index == 0xFFFF) break;
        if (index >= RR_OBJECT_RECORDS || out->outcome[index] != RR_RECORD_NO_ROOM) continue;
        if (!in_narrow[index] || narrow->outcome[index] != RR_RECORD_NO_ROOM) out->margin_left_out++;
    }
    return 1;
}

unsigned rr_objects_margin_gaps(const RrObjectSet *out, const uint8_t *table, const RrFineSprites *fine,
                                int margin) {
    enum { LOW = -128, SPAN = 640 };
    static _Thread_local uint8_t covered[RR_SPRITES_MAX][SPAN / 8];
    uint8_t has[RR_SPRITES_MAX] = {0};
    unsigned gaps = 0;
    memset(covered, 0, sizeof covered);
    for (int e = 0, seen = 0; seen < RR_SPRITES_MAX; seen++) {
        int k = fine->picture[e];
        if (k >= 0 && k < RR_SPRITES_MAX && fine->dx[e] == 0) {
            const uint8_t *entry = &table[8 * e];
            int x = (int16_t)((entry[6] << 8 | entry[7]) - 0x80), wide = (((entry[2] >> 2) & 3) + 1) * 8;
            has[k] = 1;
            for (int c = x; c < x + wide; c++)
                if (c >= LOW && c < LOW + SPAN) covered[k][(c - LOW) / 8] |= (uint8_t)(1 << ((c - LOW) & 7));
        }
        e = table[8 * e + 3] & 0x7F;
        if (e == 0 || e >= RR_SPRITES_MAX) break;
    }
    for (int k = 0; k < out->pictures && k < RR_SPRITES_MAX; k++) {
        if (!has[k]) continue;
        int left, right;
        picture_span(&out->picture[k], &left, &right);
        if (left < -margin) left = -margin;
        if (right > 0x140 + margin) right = 0x140 + margin;
        for (int c = left; c < right; c++) {
            if (c >= 0 && c < 0x140 - 5) continue; /* the screen's own business */
            if (!(covered[k][(c - LOW) / 8] & (1 << ((c - LOW) & 7)))) gaps++;
        }
    }
    return gaps;
}

void rr_object_tracker_reset(RrObjectTracker *t) {
    for (int slot = 0; slot < RR_OBJECT_RECENT; slot++) {
        t->age[slot] = 0;
        t->recent[slot].ok = 0;
        t->margin_of[slot] = 0;
    }
    t->clock = 0;
    t->chosen = -1;
    t->noted = t->frames = t->fine_frames = 0;
}

void rr_object_tracker_note(RrObjectTracker *t, const uint8_t *ram, const uint8_t *rom, size_t rom_size) {
    /* The oldest place, but never the chosen set's: the frame in progress is drawn from it. */
    int slot = -1;
    for (int i = 0; i < RR_OBJECT_RECENT; i++) {
        if (i == t->chosen) continue;
        if (slot < 0 || t->age[i] < t->age[slot]) slot = i;
    }
    t->age[slot] = ++t->clock;
    memcpy(t->ram[slot], ram, sizeof t->ram[slot]);
    /* An image that is not covered leaves the place unusable (ok = 0) until it is taken again. */
    t->margin_of[slot] = 0;
    if (!rr_objects_draw(ram, rom, rom_size, &t->recent[slot], t->work)) return;
    t->noted++;
    if (t->margin <= 0) return;
    int bottom = (int16_t)(ram[0xF2B2] << 8 | ram[0xF2B3]);
    if (rr_objects_margin(&t->recent[slot], ram, ram, rom, rom_size, t->margin, bottom, &t->margin_set[slot],
                          t->margin_table[slot], &t->margin_fine[slot], t->work)) {
        t->margin_of[slot] = t->margin;
        t->margins_made++;
    }
}

const RrFineSprites *rr_object_tracker_chosen_margin(const RrObjectTracker *t, int margin) {
    if (t->chosen < 0 || margin <= 0 || t->margin_of[t->chosen] != margin) return NULL;
    return &t->margin_fine[t->chosen];
}

static int in_video_memory(const RrObjectSet *set, const RrVdp *v) {
    if (!set->ok || set->sprites <= set->first_sprite) return 0;
    uint32_t table = (uint32_t)(v->reg[5] & 0x7E) << 9;
    for (int e = set->first_sprite; e < set->sprites; e++) {
        const uint8_t *staged = set->list + 8 * e;
        for (int k = 0; k < 8; k++) {
            if (k == 3) continue; /* the link */
            if (v->vram[(table + 8 * (uint32_t)e + (uint32_t)k) & 0xFFFF] != staged[k]) return 0;
        }
    }
    for (int tile = set->first_tile; tile < set->tiles; tile++) {
        uint32_t at = ((uint32_t)set->tile_base + (uint32_t)tile) * 32;
        const uint8_t *staged = set->tile_bytes + 32 * tile;
        for (int k = 0; k < 32; k++)
            if (v->vram[(at + (uint32_t)k) & 0xFFFF] != staged[k]) return 0;
    }
    return 1;
}

const RrFineSprites *rr_object_tracker_choose(RrObjectTracker *t, const RrVdp *v) {
    t->chosen = -1;
    int any = 0, best = -1;
    for (int slot = 0; slot < RR_OBJECT_RECENT; slot++) {
        if (t->age[slot] == 0 || !t->recent[slot].ok) continue;
        any = 1;
        /* The newest that is in video memory. */
        if (best >= 0 && t->age[slot] < t->age[best]) continue;
        if (in_video_memory(&t->recent[slot], v)) best = slot;
    }
    if (!any) return NULL;
    t->frames++;
    if (best < 0) return NULL;
    t->chosen = best;
    t->fine_frames++;
    return &t->recent[best].fine;
}

const RrObjectSet *rr_object_tracker_chosen(const RrObjectTracker *t) {
    return t->chosen >= 0 ? &t->recent[t->chosen] : NULL;
}

const uint8_t *rr_object_tracker_chosen_ram(const RrObjectTracker *t) {
    return t->chosen >= 0 ? t->ram[t->chosen] : NULL;
}
