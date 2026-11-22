/* Unit tests for the game's object drawing repeated in C, on synthetic data.
 * The expected sprite positions are those the Python reference
 * (src/rr2port/objects.py) gives for the same input.
 * Run with: make -C port test */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rr_objects.h"
#include "rr_run.h"

/* The generated code normally provides these. */
const uint32_t rr_code_rom_crc32 = 0x12345678;
RrInsnFn rr_lookup(uint32_t pc) { (void)pc; return NULL; }

enum {
    PICTURE_TABLE = 0x56A0A, SPRITE_SHAPES = 0x7C58, PICTURES_AT = 0x58000, ROM_SIZE = 0x60000,
    SPRITE_LIST = 0x3E40, PICTURE_BUFFER = 0x4742, FIRST_TILE = 0x634
};

static uint8_t rom[ROM_SIZE];
static uint8_t ram[0x10000], work[0x10000];
static RrObjectSet set;
static uint32_t next_picture;
static int records;

static void put16(uint8_t *at, int value) {
    at[0] = (uint8_t)(value >> 8);
    at[1] = (uint8_t)value;
}

static void put32(uint8_t *at, uint32_t value) {
    put16(at, (int)(value >> 16));
    put16(at + 2, (int)(value & 0xFFFF));
}

static int get16(const uint8_t *at) { return at[0] << 8 | at[1]; }

static void fresh(void) {
    memset(rom, 0, sizeof rom);
    memset(ram, 0, sizeof ram);
    /* Sprite shapes: per height and width in tiles, the height in pixels, the
     * size bits, the tiles used and the width in pixels. */
    for (int high = 1; high <= 4; high++) {
        for (int wide = 1; wide <= 4; wide++) {
            uint8_t *at = rom + SPRITE_SHAPES + (high - 1) * 0x20 + (wide - 1) * 8;
            put16(at, high * 8);
            put16(at + 2, ((wide - 1) << 2 | (high - 1)) << 8);
            put16(at + 4, wide * high);
            put16(at + 6, wide * 8);
        }
    }
    next_picture = PICTURES_AT;
    records = 0;
    put32(ram + 0x27A6, 0xFF0000 | SPRITE_LIST);
    put32(ram + 0x27CE, 0xFF0000 | PICTURE_BUFFER);
    put16(ram + 0xCE86, FIRST_TILE);
    put16(ram + 0x08D6, 0xFFFF);
}

/* Picture `index`: every byte different, so a wrong byte shows. anchor_x < 0 for none. */
static uint32_t picture(int index, int anchor_x, int anchor_y, int width, int height) {
    uint32_t at = next_picture;
    put32(rom + PICTURE_TABLE + 4 * index, at);
    uint8_t *p = rom + at;
    if (anchor_x >= 0) {
        *p++ = (uint8_t)anchor_x;
        *p++ = (uint8_t)anchor_y;
    }
    uint32_t data = (uint32_t)(p - rom);
    *p++ = (uint8_t)width;
    *p++ = (uint8_t)height;
    *p++ = 0x20;
    *p++ = 0;
    int row_bytes = (width + 1) / 2;
    for (int r = 0; r < height; r++) {
        for (int c = 0; c < row_bytes; c++) *p++ = (uint8_t)(((r * row_bytes + c) % 15 + 1) * 0x11);
        *p++ = 0;
    }
    next_picture = (uint32_t)(p - rom);
    return data;
}

static void record(int x, int foot, int size, int hidden, int index, int flags, int offset, int second) {
    uint8_t *at = ram + 0xCEB8 + 22 * records;
    put16(at, x);
    put16(at + 2, foot);
    put16(at + 4, size);
    put16(at + 6, hidden);
    put16(at + 8, index);
    put16(at + 0x0A, flags);
    put16(at + 0x0C, offset);
    put16(at + 0x12, second);
    put16(ram + 0x08D6 + 2 * records, records);
    records++;
    put16(ram + 0x08D6 + 2 * records, 0xFFFF);
}

static void draw(void) {
    memset(&set, 0xAA, sizeof set);
    memset(work, 0xAA, sizeof work);
    rr_objects_draw(ram, rom, sizeof rom, &set, work);
}

/* Entry `i` of the staged list: x, y, cells wide, cells high, tile from the first, link. */
static void expect_sprite(int i, int x, int y, int wide, int high, int tile, int link) {
    const uint8_t *e = set.list + 8 * i;
    assert((int16_t)(get16(e + 6) - 0x80) == x);
    assert((int16_t)(get16(e) - 0x80) == y);
    assert(((e[2] >> 2) & 3) + 1 == wide && (e[2] & 3) + 1 == high);
    assert((get16(e + 4) & 0x7FF) == FIRST_TILE + tile);
    assert((e[3] & 0x7F) == link);
}

static void test_one_record_stages_the_sprites_python_stages(void) {
    fresh();
    uint32_t data = picture(1, -1, 0, 16, 16);
    record(100, 120, 0x4000, 0x98, 1, 0x1000, 0, 0);
    draw();
    assert(set.ok && set.first_sprite == 0 && set.sprites == 1 && set.first_tile == 0 && set.tiles == 6);
    assert(set.tile_base == FIRST_TILE && set.records == 1 && set.dropped == 0);
    expect_sprite(0, 92, 104, 3, 2, 0, 0);
    assert(get16(set.list + 4) & 0x8000);          /* high priority */
    assert(((get16(set.list + 4) >> 13) & 3) == 1); /* the record's colour set */
    /* Tiles go down the sprite's columns; a tile is 8 rows of 4 bytes; rows are 9 bytes in the ROM. */
    const uint8_t *source = rom + data + 4;
    assert(memcmp(set.tile_bytes, source, 4) == 0);
    assert(memcmp(set.tile_bytes + 4, source + 9, 4) == 0);
    assert(memcmp(set.tile_bytes + 2 * 32, source + 4, 4) == 0);
    assert(memcmp(set.tile_bytes + 32, source + 8 * 9, 4) == 0);
    /* What the game leaves in RAM is in `work`. */
    assert(get16(work + 0xDC30) == 6 && get16(work + 0xDC34) == 1);
    assert(memcmp(work + SPRITE_LIST, set.list, 8) == 0);
    assert(memcmp(work + PICTURE_BUFFER, set.tile_bytes, sizeof set.tile_bytes) == 0);
    /* And the description the game throws away. */
    assert(set.pictures == 1 && set.fine.pictures == set.picture);
    const RrFinePicture *p = &set.picture[0];
    assert(p->rows == source && p->width == 16 && p->height == 16 && p->stride == 9);
    assert(p->size == 0x10000 && p->x == 92 && p->y == 104 && !p->mirrored);
    assert(p->first_column == 0 && p->first_row == 0 && p->columns == 16 && p->rows_shown == 16);
    assert(set.fine.picture[0] == 0 && set.fine.dx[0] == 0 && set.fine.picture[1] == -1);
}

static void test_half_size_keeps_the_whole_picture_in_fewer_pixels(void) {
    fresh();
    picture(1, -1, 0, 16, 16);
    record(100, 120, 0x2000, 0x98, 1, 0x1000, 0, 0);
    draw();
    expect_sprite(0, 96, 112, 2, 1, 0, 0);
    const RrFinePicture *p = &set.picture[0];
    assert(p->size == 0x8000 && p->x == 96 && p->y == 112 && p->columns == 8 && p->rows_shown == 8);
}

static void test_mirrored_record_measures_from_its_right_end(void) {
    fresh();
    picture(1, -1, 0, 16, 16);
    record(100, 120, 0x4000, 0x98, 1, 0x9000, 0, 0);
    draw();
    expect_sprite(0, 84, 104, 3, 2, 0, 0);
    assert(get16(set.list + 4) & 0x0800);
    assert(set.picture[0].mirrored && set.picture[0].x == 108 && set.picture[0].columns == 16);
}

static void test_copies_share_the_picture_with_a_sideways_step(void) {
    fresh();
    picture(1, -1, 0, 16, 16);
    record(100, 120, 0x4000, 0x98, 1, 0x1200, 40, 0);
    draw();
    assert(set.sprites == 3 && set.pictures == 1 && set.tiles == 6);
    expect_sprite(0, 92, 104, 3, 2, 0, 1);
    expect_sprite(1, 132, 104, 3, 2, 0, 2);
    expect_sprite(2, 172, 104, 3, 2, 0, 0);
    assert(set.fine.picture[1] == 0 && set.fine.dx[1] == 40);
    assert(set.fine.picture[2] == 0 && set.fine.dx[2] == 80);
}

static void test_two_part_record_makes_two_pictures(void) {
    fresh();
    picture(1, 8, 20, 16, 16);
    uint32_t second = picture(2, 8, 30, 16, 12);
    record(100, 120, 0x4000, 0x98, 1, 0x1080, 0, 2);
    draw();
    assert(set.sprites == 2 && set.pictures == 2 && set.tiles == 12);
    expect_sprite(0, 92, 101, 3, 2, 0, 1);
    expect_sprite(1, 92, 91, 3, 2, 6, 0);
    assert(set.fine.picture[0] == 0 && set.fine.picture[1] == 1);
    assert(set.picture[0].y == 101 && set.picture[1].y == 91);
    assert(set.picture[1].rows == rom + second + 4 && set.picture[1].height == 12);
}

static void test_left_clip_sets_first_column(void) {
    fresh();
    picture(1, -1, 0, 32, 16);
    record(5, 120, 0x4000, 0x98, 1, 0x1000, 0, 0);
    draw();
    expect_sprite(0, 0, 104, 3, 2, 0, 0);
    /* 11 of its 32 pixels are off the left edge; the game drops whole pairs: 6 of them. */
    const RrFinePicture *p = &set.picture[0];
    assert(p->x == 0 && p->first_column == 12 && p->columns == 20);
}

static void test_hidden_line_cuts_rows_shown(void) {
    fresh();
    picture(1, -1, 0, 16, 16);
    record(100, 120, 0x4000, 110, 1, 0x1000, 0, 0);
    draw();
    expect_sprite(0, 92, 104, 3, 1, 0, 0);
    assert(set.picture[0].rows_shown == 6 && set.picture[0].y == 104);
}

static void test_rows_above_the_view_set_first_row(void) {
    fresh();
    picture(1, -1, 0, 16, 40);
    record(100, 20, 0x4000, 0x98, 1, 0x1000, 0, 0);
    draw();
    expect_sprite(0, 92, 0, 3, 3, 0, 0);
    assert(set.picture[0].y == 0 && set.picture[0].first_row == 20 && set.picture[0].rows_shown == 20);
}

static void test_dropped_record_leaves_no_picture(void) {
    fresh();
    picture(1, -1, 0, 16, 16);
    picture(2, -1, 0, 64, 64);
    record(60, 120, 0x4000, 0x98, 1, 0x1000, 0, 0);
    record(200, 120, 0x4000, 0x98, 2, 0x1000, 0, 0); /* 80 tiles; 6 + 80 fit, so use the sprite limit */
    put16(ram + 0xDC34, 79);                          /* one entry left: the first record takes it */
    draw();
    assert(set.ok && set.first_sprite == 79 && set.sprites == 80);
    assert(set.records == 2 && set.dropped == 1 && set.pictures == 1);
    assert(set.fine.picture[79] == 0);
    assert(get16(work + 0xDC34) == 80 && get16(work + 0xDC30) == 6);
}

static void test_entries_past_the_list_get_no_picture(void) {
    /* The game only looks at the limit before a picture: one that starts at
     * entry 79 and needs six runs past the end of the list. */
    fresh();
    picture(1, -1, 0, 64, 64);
    record(100, 120, 0x4000, 0x98, 1, 0x1000, 0, 0);
    put16(ram + 0xDC34, 79);
    draw();
    assert(get16(work + 0xDC34) == 85);
    assert(set.ok && set.first_sprite == 79 && set.sprites == 80);
    assert(set.fine.picture[79] == 0);
}

static void test_a_record_dropped_after_its_first_part_loses_that_part_too(void) {
    /* Two parts of 6 tiles each with room for 8: the first is drawn and described, the second
     * runs out, and the game then takes the whole record back. */
    fresh();
    picture(1, 8, 20, 16, 16);
    picture(2, 8, 30, 16, 12);
    record(100, 120, 0x4000, 0x98, 1, 0x1080, 0, 2);
    put16(ram + 0xDC30, 0x150 - 8);
    draw();
    assert(set.ok && set.dropped == 1 && set.sprites == 0 && set.pictures == 0);
    for (int i = 0; i < RR_SPRITES_MAX; i++) assert(set.fine.picture[i] == -1);
}

static void test_entries_left_by_copies_that_ran_out_are_not_described(void) {
    /* Entry 77 with copies: 78 is made, 79 is written but the list is then full, so the game
     * does not count it. */
    fresh();
    picture(1, -1, 0, 16, 16);
    record(40, 120, 0x4000, 0x98, 1, 0x1700, 20, 0);
    put16(ram + 0xDC34, 77);
    draw();
    assert(set.ok && set.sprites == 79 && set.fine.picture[78] == 0 && set.fine.picture[79] == -1);
    /* The next record takes entry 79 for a picture that is not in the ROM: the entry must
     * not keep the description the copy left there. */
    put32(rom + PICTURE_TABLE + 8, ROM_SIZE - 8);
    rom[ROM_SIZE - 8] = 16;
    rom[ROM_SIZE - 7] = 16;
    record(200, 120, 0x4000, 0x98, 2, 0x1000, 0, 0);
    draw();
    assert(set.sprites == 80 && set.pictures == 1 && set.fine.picture[79] == -1);
}

static void test_picture_outside_rom_is_plain(void) {
    fresh();
    put32(rom + PICTURE_TABLE + 4, ROM_SIZE - 8);
    rom[ROM_SIZE - 8] = 16;
    rom[ROM_SIZE - 7] = 16;
    record(100, 120, 0x4000, 0x98, 1, 0x1000, 0, 0);
    draw();
    assert(set.ok && set.sprites == 1 && set.pictures == 0 && set.fine.picture[0] == -1);
}

static void test_wild_pointers_and_counts_do_not_escape(void) {
    /* Guard bytes round the buffers the model may write. */
    static struct {
        uint8_t before[64];
        uint8_t work[0x10000];
        uint8_t after[64];
    } guarded;
    fresh();
    for (int k = 0; k < 128; k++) put16(ram + 0x08D6 + 2 * k, k * 517); /* no terminator */
    for (int i = 0; i < 0x1000; i++) ram[0xCEB8 + i] = (uint8_t)(i * 37 + 11);
    put32(ram + 0x27A6, 0xFFFFFFFF);
    put32(ram + 0x27CE, 0xFFFFFFFF);
    put16(ram + 0xDC34, 0xFFFF);
    put16(ram + 0xDC30, 0xFFFF);
    memset(&guarded, 0x5A, sizeof guarded);
    rr_objects_draw(ram, rom, sizeof rom, &set, guarded.work);
    for (int i = 0; i < 64; i++) assert(guarded.before[i] == 0x5A && guarded.after[i] == 0x5A);
    assert(!set.ok);
    /* Every picture number wild, the counters sane. */
    fresh();
    for (int n = 0; n < 100; n++) record(100, 120, 0xFFFF, 0x98, 0x3FFF - n, 0x1780, 9, 0x2000 + n);
    memset(&guarded, 0x5A, sizeof guarded);
    rr_objects_draw(ram, rom, sizeof rom, &set, guarded.work);
    for (int i = 0; i < 64; i++) assert(guarded.before[i] == 0x5A && guarded.after[i] == 0x5A);
    /* A ROM too short to hold the tables. */
    assert(!rr_objects_draw(ram, rom, 0x1000, &set, work));
}

/* The model's description and the renderer's rule agree with the game: a
 * picture at its natural size, drawn fine, is the game's own sprites with
 * each pixel repeated. (Two things the game does are left out of the rule on
 * purpose and skipped here: the first row of a picture whose top is cut shows
 * the picture's row 0, and likewise its first pixel pair when its side is
 * cut.) */
static RrVdp chip;
static uint32_t fine_line[RR_SCALE_MAX * RR_SCALE_MAX * RR_SCREEN_W], plain_line[RR_SCALE_MAX * RR_SCALE_MAX * RR_SCREEN_W];

/* Video memory as the game's block copies leave it for a staged set. */
static void show(const RrObjectSet *staged) {
    chip.reg[1] = 0x44;  /* display on */
    chip.reg[5] = 0x62;  /* sprite table at 0xC400, where the game has it */
    chip.reg[12] = 0x81; /* 40 cells */
    for (int i = 1; i < 64; i++) chip.cram[i] = (uint16_t)(((i & 7) << 1) | (((i >> 3) & 7) << 5) | 0x200);
    memcpy(&chip.vram[0xC400], staged->list, sizeof staged->list);
    memcpy(&chip.vram[staged->tile_base * 32], staged->tile_bytes, (size_t)staged->tiles * 32);
}

static long compare_fine_with_plain(int scale) {
    rr_vdp_reset(&chip);
    show(&set);
    long compared = 0, pictured = 0;
    for (int line = 0; line < RR_SCREEN_H; line++) {
        rr_vdp_render_line_fine(&chip, line, scale, NULL, NULL, &set.fine, fine_line);
        rr_vdp_render_line_fine(&chip, line, scale, NULL, NULL, NULL, plain_line);
        for (int x = 0; x < RR_SCREEN_W; x++) {
            int skip = 0;
            for (int i = 0; i < set.pictures; i++) {
                const RrFinePicture *p = &set.picture[i];
                for (int e = set.first_sprite; e < set.sprites; e++) {
                    if (set.fine.picture[e] != i) continue;
                    int first = p->x + set.fine.dx[e];
                    if (p->first_row > 0 && line == p->y) skip = 1;
                    if (p->first_column > 0 && (p->mirrored ? (x == first - 1 || x == first - 2) : (x == first || x == first + 1)))
                        skip = 1;
                }
            }
            if (skip) continue;
            for (int r = 0; r < scale; r++)
                for (int k = 0; k < scale; k++) {
                    int at = r * RR_SCREEN_W * scale + x * scale + k;
                    assert(fine_line[at] == plain_line[at]);
                    compared++;
                    pictured += plain_line[at] != rr_vdp_colour(0);
                }
        }
    }
    assert(pictured > 0);
    return compared;
}

static void test_natural_size_fine_picture_is_the_plain_enlargement(void) {
    for (int scale = 2; scale <= 3; scale++) {
        fresh();
        picture(1, -1, 0, 16, 16);
        picture(2, -1, 0, 15, 10);
        picture(3, 8, 20, 16, 16);
        picture(4, 8, 30, 16, 12);
        picture(5, -1, 0, 32, 40);
        record(100, 120, 0x4000, 0x98, 1, 0x1000, 0, 0);  /* plain */
        record(200, 120, 0x4000, 0x98, 1, 0x9000, 0, 0);  /* mirrored */
        record(40, 60, 0x4000, 0x98, 2, 0x2200, 40, 0);   /* an odd width, with two copies */
        record(250, 90, 0x4000, 0x98, 3, 0x1080, 0, 4);   /* two parts from one anchor */
        record(150, 150, 0x4000, 140, 1, 0xB000, 0, 0);   /* mirrored, its foot hidden by a hill */
        draw();
        assert(set.ok && set.pictures == 6 && set.sprites >= 8);
        compare_fine_with_plain(scale);
        /* Cut at the left edge and at the top of the view. */
        fresh();
        picture(1, -1, 0, 32, 40);
        record(5, 120, 0x4000, 0x98, 1, 0x1000, 0, 0);
        record(200, 20, 0x4000, 0x98, 1, 0x1000, 0, 0);
        record(8, 140, 0x4000, 0x98, 1, 0x9000, 0, 0);
        draw();
        assert(set.pictures == 3 && set.picture[0].first_column > 0 && set.picture[1].first_row > 0);
        compare_fine_with_plain(scale);
    }
}

/* Which set is on screen: the game shows a pass's objects a frame or more
 * after drawing them, from one of two alternating tile areas. */
static RrObjectTracker tracker;

static void note_scene(int x, int tile_base) {
    fresh();
    picture(1, -1, 0, 16, 16);
    record(x, 120, 0x2000, 0x98, 1, 0x1000, 0, 0);
    put16(ram + 0xCE86, tile_base);
    rr_object_tracker_note(&tracker, ram, rom, sizeof rom);
    draw(); /* the same set, in `set`, to put in video memory */
}

static void test_tracker_picks_the_set_in_video_memory(void) {
    static RrObjectSet first;
    rr_object_tracker_reset(&tracker);
    rr_vdp_reset(&chip);
    assert(rr_object_tracker_choose(&tracker, &chip) == NULL && tracker.frames == 0);
    note_scene(100, 0x634);
    first = set;
    note_scene(150, 0x434);
    assert(tracker.noted == 2);
    show(&first); /* the older one is on screen */
    const RrFineSprites *fine = rr_object_tracker_choose(&tracker, &chip);
    assert(fine != NULL && fine->picture[0] == 0 && fine->pictures[0].x == 96 && tracker.chosen >= 0);
    show(&set); /* now the newer one (the older one's tiles are still in their own area) */
    fine = rr_object_tracker_choose(&tracker, &chip);
    assert(fine != NULL && fine->pictures[0].x == 146);
    assert(tracker.frames == 2 && tracker.fine_frames == 2);
}

static void test_tracker_ignores_the_link_byte(void) {
    rr_object_tracker_reset(&tracker);
    rr_vdp_reset(&chip);
    note_scene(100, 0x634);
    show(&set);
    chip.vram[0xC400 + 3] = 0x15; /* the game linked more sprites after the objects */
    assert(rr_object_tracker_choose(&tracker, &chip) != NULL);
}

static void test_tracker_returns_null_when_video_memory_holds_something_else(void) {
    rr_object_tracker_reset(&tracker);
    rr_vdp_reset(&chip);
    note_scene(100, 0x634);
    for (int which = 0; which < 3; which++) {
        show(&set);
        if (which == 0) chip.vram[0x634 * 32 + 5] ^= 0x10; /* one pixel of a tile */
        if (which == 1) chip.vram[0xC400 + 7] ^= 1;        /* the sprite's x */
        if (which == 2) chip.vram[0xC400 + 2] ^= 1;        /* its size */
        assert(rr_object_tracker_choose(&tracker, &chip) == NULL && tracker.chosen == -1);
    }
    assert(tracker.frames == 3 && tracker.fine_frames == 0);
    chip.reg[5] = 0x60; /* the sprite table somewhere else */
    show(&set);
    chip.reg[5] = 0x60;
    assert(rr_object_tracker_choose(&tracker, &chip) == NULL);
}

static void test_tracker_returns_null_with_no_objects(void) {
    rr_object_tracker_reset(&tracker);
    rr_vdp_reset(&chip);
    fresh(); /* a pass that draws nothing: empty video memory would "match" it */
    rr_object_tracker_note(&tracker, ram, rom, sizeof rom);
    assert(rr_object_tracker_choose(&tracker, &chip) == NULL);
    /* An image the model does not cover is not kept at all. */
    rr_object_tracker_note(&tracker, ram, rom, 0x1000);
    assert(tracker.noted == 1);
}

static void test_tracker_keeps_the_last_four_and_reset_forgets(void) {
    static RrObjectSet first;
    rr_object_tracker_reset(&tracker);
    rr_vdp_reset(&chip);
    note_scene(60, 0x634);
    first = set;
    for (int n = 1; n <= RR_OBJECT_RECENT; n++) note_scene(60 + 20 * n, 0x434);
    show(&first);
    assert(rr_object_tracker_choose(&tracker, &chip) == NULL); /* five passes ago */
    show(&set);
    assert(rr_object_tracker_choose(&tracker, &chip) != NULL);
    rr_object_tracker_reset(&tracker);
    assert(rr_object_tracker_choose(&tracker, &chip) == NULL);
    assert(tracker.noted == 0 && tracker.frames == 0 && tracker.fine_frames == 0 && tracker.chosen == -1);
}

static void test_a_picture_cut_at_the_right_runs_on_to_the_screen_edge(void) {
    /* The game stops a picture four pixels short of the right edge; the fine picture runs
     * on. So at natural size the two agree wherever the game drew, and differ only in the
     * last four columns, where the game drew nothing. */
    fresh();
    picture(1, -1, 0, 32, 40);
    record(309, 120, 0x4000, 0x98, 1, 0x1000, 0, 0);
    draw();
    assert(set.ok && set.pictures == 1);
    rr_vdp_reset(&chip);
    show(&set);
    long more = 0;
    for (int line = 0; line < RR_SCREEN_H; line++) {
        rr_vdp_render_line_fine(&chip, line, 2, NULL, NULL, &set.fine, fine_line);
        rr_vdp_render_line_fine(&chip, line, 2, NULL, NULL, NULL, plain_line);
        for (int at = 0; at < 4 * RR_SCREEN_W; at++) {
            if (fine_line[at] == plain_line[at]) continue;
            assert((at % (2 * RR_SCREEN_W)) / 2 >= RR_SCREEN_W - 4 && plain_line[at] == rr_vdp_colour(0));
            more++;
        }
    }
    assert(more > 0);
}

static void test_noting_a_new_set_leaves_the_one_on_screen_alone(void) {
    /* The set chosen for the frame being drawn is still in use while the game draws its
     * next pass: noting that pass must not overwrite it, however many are kept. */
    static RrObjectSet first;
    rr_object_tracker_reset(&tracker);
    rr_vdp_reset(&chip);
    note_scene(60, 0x634);
    first = set;
    for (int n = 1; n < RR_OBJECT_RECENT; n++) note_scene(60 + 20 * n, 0x434);
    show(&first); /* the oldest of the four is on screen */
    const RrFineSprites *fine = rr_object_tracker_choose(&tracker, &chip);
    assert(fine != NULL && fine->pictures[0].x == 56);
    note_scene(200, 0x434);
    note_scene(220, 0x434);
    assert(fine->picture[0] == 0 && fine->pictures[0].x == 56); /* still the set on screen */
    assert(rr_object_tracker_chosen(&tracker) != NULL);
    assert(rr_object_tracker_choose(&tracker, &chip) == fine);  /* and still found */
    show(&set);
    fine = rr_object_tracker_choose(&tracker, &chip);
    assert(fine != NULL && fine->pictures[0].x == 216);          /* the newest too */
}

static void test_tracker_keeps_the_ram_each_set_was_noted_from(void) {
    static uint8_t first_ram[0x10000];
    static RrObjectSet first;
    rr_object_tracker_reset(&tracker);
    rr_vdp_reset(&chip);
    assert(rr_object_tracker_chosen_ram(&tracker) == NULL);
    note_scene(100, 0x634);
    first = set;
    memcpy(first_ram, ram, sizeof ram);
    note_scene(150, 0x434);
    show(&first);
    assert(rr_object_tracker_choose(&tracker, &chip) != NULL);
    assert(memcmp(rr_object_tracker_chosen_ram(&tracker), first_ram, sizeof first_ram) == 0);
    show(&set);
    assert(rr_object_tracker_choose(&tracker, &chip) != NULL);
    assert(memcmp(rr_object_tracker_chosen_ram(&tracker), ram, sizeof ram) == 0);
}

/* --- another view, and what became of each record ----------------------- */

static RrObjectSet view_set;
static uint8_t view_work[0x10000];
static const RrObjectView game_view = {0, 0x140}, wide_view = {-39, 0x140 + 39};

static void draw_view(const RrObjectView *view, int tiles) {
    memset(&view_set, 0xAA, sizeof view_set);
    memset(view_work, 0xAA, sizeof view_work);
    rr_objects_draw_view(ram, rom, sizeof rom, view, tiles, &view_set, view_work);
}

static void test_the_games_view_is_todays_drawing(void) {
    fresh();
    picture(1, -1, 0, 32, 40);
    picture(2, 8, 30, 16, 32);
    picture(3, 4, 12, 16, 12);
    picture(4, -1, 0, 64, 64);
    record(309, 120, 0x4000, 0x98, 1, 0x1000, 0, 0);   /* cut at the right */
    record(5, 120, 0x4000, 0x98, 1, 0x9000, 0, 0);     /* cut at the left, mirrored */
    record(100, 120, 0x2000, 0x98, 1, 0x1002, 40, 0);  /* two copies */
    record(160, 140, 0x4000, 0x98, 2, 0x1080, 0, 3);   /* two parts */
    record(900, 120, 0x4000, 0x98, 1, 0x1000, 0, 0);   /* off the view */
    record(200, 20, 0x4000, 0x98, 4, 0x1000, 0, 0);    /* partly above it */
    for (int room = 0; room < 2; room++) {
        if (room == 1) put16(ram + 0xDC34, 74); /* the list nearly full: something is dropped */
        draw();
        draw_view(&game_view, 1);
        assert(set.ok && view_set.ok && set.pictures > 3);
        memset(set.outcome, 0, sizeof set.outcome);
        memset(view_set.outcome, 0, sizeof view_set.outcome);
        view_set.fine.pictures = set.fine.pictures; /* each points at its own set's pictures */
        assert(memcmp(&set, &view_set, sizeof set) == 0);
        assert(memcmp(work, view_work, sizeof work) == 0);
    }
}

static void test_outcomes_are_recorded(void) {
    fresh();
    picture(1, -1, 0, 16, 16);
    picture(2, -1, 0, 64, 64);
    picture(3, -1, 0, 16, 1);
    record(60, 120, 0x4000, 0x98, 1, 0x1000, 0, 0);
    record(200, 120, 0x4000, 0x98, 2, 0x1000, 0, 0); /* more sprites than are left */
    record(900, 120, 0x4000, 0x98, 1, 0x1000, 0, 0); /* off the view's side */
    record(100, 120, 0x4000, 50, 1, 0x1000, 0, 0);   /* wholly under the line a hill hides it below */
    record(100, 120, 0x4000, 0x98, 3, 0x1000, 0, 0); /* one row: the game makes nothing of it */
    put16(ram + 0xDC34, 79);
    draw();
    assert(set.ok && set.records == 5);
    assert(set.outcome[0] == RR_RECORD_DRAWN && set.outcome[1] == RR_RECORD_NO_ROOM);
    assert(set.outcome[2] == RR_RECORD_OFF_VIEW && set.outcome[3] == RR_RECORD_HIDDEN);
    assert(set.outcome[4] == RR_RECORD_NO_PICTURE);
    /* The outcome is kept by the record's slot, whatever the order they are drawn in. */
    put16(ram + 0x08D6, 2);
    put16(ram + 0x08D6 + 4, 0);
    draw();
    assert(set.outcome[2] == RR_RECORD_OFF_VIEW && set.outcome[0] == RR_RECORD_NO_ROOM && set.outcome[1] == RR_RECORD_DRAWN);
    /* A slot past the last the drawing looks at is drawn as ever and not recorded. */
    put16(ram + 0x08D6, 300);
    draw();
    assert(set.ok && set.records == 5);
}

static void test_a_wider_view_keeps_a_picture_the_game_cut(void) {
    fresh();
    picture(1, -1, 0, 32, 16);
    record(309, 120, 0x4000, 0x98, 1, 0x1000, 0, 0); /* 293 to 324: the game cuts it at 316 */
    record(5, 120, 0x4000, 0x98, 1, 0x1000, 0, 0);   /* -11 to 20: the game cuts it at 0 */
    record(340, 120, 0x4000, 0x98, 1, 0x1000, 0, 0); /* 324 to 355: not in the game's view at all */
    record(-30, 120, 0x4000, 0x98, 1, 0x1000, 0, 0); /* -46 to -15: cut at the wide view's left side */
    record(500, 120, 0x4000, 0x98, 1, 0x1000, 0, 0); /* in neither */
    draw();
    assert(set.pictures == 2 && set.picture[1].first_column == 12 && set.picture[1].x == 0);
    assert(set.outcome[2] == RR_RECORD_OFF_VIEW && set.outcome[3] == RR_RECORD_OFF_VIEW);
    draw_view(&wide_view, 0);
    assert(view_set.ok && view_set.pictures == 4 && view_set.records == 5);
    assert(view_set.picture[0].x == 293 && view_set.picture[0].first_column == 0 && view_set.picture[0].columns == 32);
    assert(view_set.picture[1].x == -11 && view_set.picture[1].first_column == 0 && view_set.picture[1].columns == 32);
    assert(view_set.picture[2].x == 324 && view_set.picture[2].first_column == 0);
    assert(view_set.picture[3].x == -39 && view_set.picture[3].first_column == 8 && view_set.picture[3].columns == 24);
    assert(view_set.outcome[2] == RR_RECORD_DRAWN && view_set.outcome[3] == RR_RECORD_DRAWN);
    assert(view_set.outcome[4] == RR_RECORD_OFF_VIEW);
    /* Every sprite entry shows one of the four pictures, and each picture has an entry. */
    int shown[4] = {0, 0, 0, 0};
    for (int i = 0; i < view_set.sprites; i++) {
        assert(view_set.fine.picture[i] >= 0 && view_set.fine.picture[i] < 4);
        shown[view_set.fine.picture[i]]++;
    }
    assert(view_set.sprites >= 4 && shown[0] && shown[1] && shown[2] && shown[3]);
}

/* The size byte of the first sprite of one 32-pixel picture centred on x. */
static int cut_size(int x, const RrObjectView *view) {
    fresh();
    picture(1, -1, 0, 32, 16);
    record(x, 120, 0x4000, 0x98, 1, 0x1000, 0, 0);
    draw_view(view, view == &game_view);
    assert(view_set.ok && view_set.pictures == 1);
    return view_set.list[2];
}

static void test_a_wider_view_has_no_four_pixel_rule(void) {
    /* A picture that reaches k pixels over the game's right edge is cut as if it reached
     * k + 4 over. Over the wide view's right edge it is cut at the edge: as the game
     * cuts one that is four pixels further left. */
    int differ = 0;
    for (int x = 306; x < 330; x++) {
        int game = cut_size(x, &game_view), game_less = cut_size(x - 4, &game_view);
        int wide = cut_size(x + 39, &wide_view);
        assert(wide == game_less);
        differ += game != game_less;
    }
    assert(differ > 0); /* so the rule would have shown */
    /* And where the picture stands is not moved by the cut. */
    (void)cut_size(314 + 39, &wide_view);
    assert(view_set.picture[0].x == 337 && get16(&view_set.list[6]) == 0x80 + 337);
}

static void test_no_tile_budget_in_a_view_without_tiles(void) {
    fresh();
    picture(1, -1, 0, 16, 16);
    record(60, 120, 0x4000, 0x98, 1, 0x1000, 0, 0);
    put16(ram + 0xDC30, 0x14F); /* one tile left in the game's buffer: this picture needs six */
    draw();
    assert(set.pictures == 0 && set.outcome[0] == RR_RECORD_NO_ROOM);
    draw_view(&game_view, 1);
    assert(view_set.pictures == 0 && view_set.outcome[0] == RR_RECORD_NO_ROOM);
    draw_view(&game_view, 0);
    assert(view_set.ok && view_set.pictures == 1 && view_set.outcome[0] == RR_RECORD_DRAWN);
    /* Nothing is written into the picture buffer, wherever the counter stands. */
    assert(memcmp(view_work + PICTURE_BUFFER, ram + PICTURE_BUFFER, 0x400 * 32) == 0); /* and well past its end */
    assert(memcmp(view_work, ram, SPRITE_LIST) == 0);
    /* The sprite limit is still 80. */
    put16(ram + 0xDC34, 80);
    draw_view(&wide_view, 0);
    assert(view_set.ok && view_set.pictures == 0 && view_set.outcome[0] == RR_RECORD_NO_ROOM);
}

/* --- the margin set ------------------------------------------------------ */

static RrObjectSet margin_set;
static uint8_t margin_table[RR_SPRITES_MAX * 8], margin_work[0x10000], wide_ram[0x10000];
static RrFineSprites margin_fine;

/* The narrow set from `ram`, then the margin set for a margin of 39 from `from`. */
static void margins_from(const uint8_t *from) {
    draw();
    assert(set.ok);
    memset(&margin_set, 0xAA, sizeof margin_set);
    memset(margin_table, 0xAA, sizeof margin_table);
    memset(&margin_fine, 0xAA, sizeof margin_fine);
    assert(rr_objects_margin(&set, ram, from, rom, sizeof rom, 39, 152, &margin_set, margin_table, &margin_fine,
                             margin_work) == 1);
    assert(margin_fine.table == margin_table && margin_fine.pictures == margin_set.picture);
    assert(margin_fine.strips == NULL && margin_fine.soft == 0);
    for (int i = 0; i < RR_SPRITES_MAX; i++) assert(margin_fine.skip[i] == 0);
}

static void margins(void) { margins_from(ram); }

/* Entries of the margin table, following its links from entry 0. */
static int margin_entries(void) {
    int n = 0;
    if (get16(margin_table) == 0 && margin_table[2] == 0 && margin_fine.picture[0] < 0) return 0;
    for (int i = 0;; i = margin_table[8 * i + 3] & 0x7F) {
        n++;
        if ((margin_table[8 * i + 3] & 0x7F) == 0 || n > RR_SPRITES_MAX) break;
    }
    return n;
}

static int margin_entry_x(int i) { return (int16_t)(get16(margin_table + 8 * i + 6) - 0x80); }

static const RrFinePicture *margin_picture(int entry) {
    assert(margin_fine.picture[entry] >= 0);
    return &margin_set.picture[margin_fine.picture[entry]];
}

static void test_a_record_wholly_right_of_319_is_in_the_margin_set(void) {
    fresh();
    picture(1, -1, 0, 32, 16);
    record(340, 120, 0x4000, 0x98, 1, 0x1000, 0, 0); /* 324 to 355 */
    margins();
    assert(set.pictures == 0 && set.outcome[0] == RR_RECORD_OFF_VIEW);
    assert(margin_entries() >= 1 && margin_entry_x(0) == 324);
    const RrFinePicture *p = margin_picture(0);
    assert(p->x == 324 && p->y == 104 && p->columns == 32 && p->first_column == 0 && p->rows_shown == 16);
    assert(margin_set.margin_pictures == 1 && margin_set.margin_off_view == 1 && margin_set.margin_uncut == 0);
    /* The entries keep the record's colour set and stand in front of the planes as the game's do. */
    assert((get16(margin_table + 4) & 0xE000) == 0xA000);
    /* And one wholly left of 0. */
    fresh();
    picture(1, -1, 0, 32, 16);
    record(-20, 120, 0x4000, 0x98, 1, 0x1000, 0, 0); /* -36 to -5 */
    margins();
    assert(margin_entries() >= 1 && margin_picture(0)->x == -36 && margin_set.margin_off_view == 1);
}

static void test_a_record_wholly_in_the_centre_is_not(void) {
    fresh();
    picture(1, -1, 0, 32, 16);
    record(100, 120, 0x4000, 0x98, 1, 0x1000, 0, 0);
    record(600, 120, 0x4000, 0x98, 1, 0x1000, 0, 0); /* nor one beyond the wide view */
    margins();
    assert(set.pictures == 1);
    assert(margin_entries() == 0 && margin_set.margin_pictures == 0);
    for (int i = 0; i < RR_SPRITES_MAX; i++) assert(margin_fine.picture[i] == -1);
}

static void test_a_record_dropped_for_room_stays_dropped(void) {
    fresh();
    picture(1, -1, 0, 32, 16);
    record(5, 120, 0x4000, 0x98, 1, 0x1000, 0, 0);
    put16(ram + 0xDC30, 0x14F); /* the game's tile buffer is full: it draws nothing of this one */
    margins();
    assert(set.outcome[0] == RR_RECORD_NO_ROOM);
    assert(margin_entries() == 0 && margin_set.margin_pictures == 0);
}

static void test_a_left_cut_picture_is_the_narrow_one_uncut(void) {
    fresh();
    picture(1, -1, 0, 32, 16);
    record(5, 120, 0x4000, 0x98, 1, 0x1000, 0, 0); /* -11 to 20: 11 pixels off, the game drops 12 */
    margins();
    const RrFinePicture *narrow = &set.picture[0];
    assert(narrow->x == 0 && narrow->first_column == 12 && narrow->columns == 20);
    assert(margin_entries() >= 1 && margin_set.margin_uncut == 1 && margin_set.margin_off_view == 0);
    const RrFinePicture *p = margin_picture(0);
    /* The same art on the same lines; the column at x = -1 is the one just left of the centre's first. */
    assert(p->rows == narrow->rows && p->y == narrow->y && p->first_row == narrow->first_row);
    assert(p->rows_shown == narrow->rows_shown && p->size == narrow->size && p->mirrored == 0);
    assert(p->first_column == 0 && p->columns == 32 && p->x == -12);
    /* Its entries reach from the picture's left end to the seam at least. */
    int left = 9999, right = -9999;
    for (int i = 0; i < margin_entries(); i++) {
        int x = margin_entry_x(i), wide = (((margin_table[8 * i + 2] >> 2) & 3) + 1) * 8;
        if (x < left) left = x;
        if (x + wide > right) right = x + wide;
    }
    assert(left <= -12 && right >= 0);
}

static void test_a_mirrored_picture_cut_at_the_right_is_uncut(void) {
    fresh();
    picture(1, -1, 0, 32, 16);
    record(309, 120, 0x4000, 0x98, 1, 0x9000, 0, 0); /* 293 to 324, mirrored */
    margins();
    const RrFinePicture *narrow = &set.picture[0];
    assert(set.pictures == 1 && narrow->mirrored && narrow->first_column > 0);
    assert(narrow->x >= 315 && narrow->x <= 316); /* its right-hand end: where the game stopped */
    const RrFinePicture *p = margin_picture(0);
    assert(margin_set.margin_uncut == 1);
    assert(p->mirrored && p->first_column == 0 && p->x == narrow->x + narrow->first_column);
    assert(p->columns == narrow->columns + narrow->first_column && p->columns == 32);
    assert(p->rows == narrow->rows && p->y == narrow->y && p->rows_shown == narrow->rows_shown);
    int right = -9999;
    for (int i = 0; i < margin_entries(); i++) {
        int x = margin_entry_x(i), wide = (((margin_table[8 * i + 2] >> 2) & 3) + 1) * 8;
        if (x + wide > right) right = x + wide;
    }
    assert(right >= p->x);
}

static void test_a_two_part_record_whose_first_part_is_off_the_narrow_view(void) {
    fresh();
    uint32_t first = picture(1, 0, 20, 16, 16);  /* its left end stands on x */
    uint32_t second = picture(2, 40, 30, 16, 12); /* 40 to the left of x */
    record(318, 120, 0x4000, 0x98, 1, 0x1080, 0, 2);
    margins();
    /* The game draws the second part alone (278 to 293); the first (318 to 333) is off its view. */
    assert(set.pictures == 1 && set.picture[0].rows == rom + second + 4 && set.outcome[0] == RR_RECORD_DRAWN);
    assert(margin_entries() >= 1 && margin_set.margin_pictures == 1);
    for (int i = 0; i < margin_entries(); i++) {
        const RrFinePicture *p = margin_picture(i);
        assert(p->rows == rom + first + 4 && p->x == 318 && p->y == 101);
    }
    assert(margin_set.margin_uncut == 0 && margin_set.margin_off_view == 1);
}

static void test_a_record_below_the_view_is_left_out(void) {
    fresh();
    picture(1, -1, 0, 32, 16);
    record(340, 180, 0x4000, 0xE0, 1, 0x1000, 0, 0); /* lines 164 to 179: a rider in a mirror */
    record(340, 160, 0x4000, 0xE0, 1, 0x1000, 0, 0); /* lines 144 to 159: its top is in the view */
    margins();
    assert(margin_set.margin_pictures == 1 && margin_picture(0)->y == 144);
}

static void test_an_entry_without_a_picture_is_left_out(void) {
    fresh();
    put32(rom + PICTURE_TABLE + 4, ROM_SIZE - 8);
    rom[ROM_SIZE - 8] = 16;
    rom[ROM_SIZE - 7] = 16;
    record(340, 120, 0x4000, 0x98, 1, 0x1000, 0, 0);
    margins();
    assert(margin_entries() == 0 && margin_set.margin_pictures == 0);
}

static void test_a_straddling_copy_is_in_the_margin_only(void) {
    fresh();
    picture(1, -1, 0, 16, 16);
    record(240, 120, 0x4000, 0x98, 1, 0x1300, 40, 0); /* copies at 280, 320 and 360 */
    margins();
    /* The game draws the picture and its first copy; the second stands across its right edge. */
    assert(set.sprites == 2 && set.fine.dx[1] == 40);
    assert(margin_entries() == 1 && margin_fine.dx[0] == 80 && margin_entry_x(0) == 312);
    assert(margin_picture(0)->x == 232 && margin_picture(0)->rows == set.picture[0].rows);
    assert(margin_set.margin_copies == 1 && margin_set.margin_uncut == 0 && margin_set.margin_off_view == 0);
}

static void test_a_second_ram_supplies_extra_records(void) {
    fresh();
    picture(1, -1, 0, 32, 16);
    record(100, 120, 0x4000, 0x98, 1, 0x1000, 0, 0);
    memcpy(wide_ram, ram, sizeof ram);
    /* One more roadside record, in the copy only. */
    uint8_t *at = wide_ram + 0xCEB8 + 22 * 1;
    put16(at, 345);
    put16(at + 2, 120);
    put16(at + 4, 0x4000);
    put16(at + 6, 0x98);
    put16(at + 8, 1);
    put16(at + 0x0A, 0x1000);
    put16(wide_ram + 0x08D6 + 2, 1);
    put16(wide_ram + 0x08D6 + 4, 0xFFFF);
    margins_from(wide_ram);
    assert(set.records == 1 && margin_set.records == 2);
    assert(margin_entries() >= 1 && margin_picture(0)->x == 329 && margin_set.margin_off_view == 1);
}

static void test_a_margin_set_needs_a_narrow_set(void) {
    fresh();
    picture(1, -1, 0, 32, 16);
    record(340, 120, 0x4000, 0x98, 1, 0x1000, 0, 0);
    draw();
    set.ok = 0;
    memset(&margin_set, 0xAA, sizeof margin_set);
    assert(rr_objects_margin(&set, ram, ram, rom, sizeof rom, 39, 152, &margin_set, margin_table, &margin_fine,
                             margin_work) == 0);
    assert(margin_set.ok == 0);
}

/* Which screen columns the margin table's entries for the first margin picture cover. */
static void margin_cover(uint8_t *covered /* [-64, 448) at covered[x + 64] */) {
    memset(covered, 0, 512);
    for (int i = 0; i < margin_entries(); i++) {
        int x = margin_entry_x(i), wide = (((margin_table[8 * i + 2] >> 2) & 3) + 1) * 8;
        for (int c = x; c < x + wide; c++)
            if (c >= -64 && c < 448) covered[c + 64] = 1;
    }
}

/* A picture wider than the margin, cut by the game's view and by the wide view too. */
static void test_a_picture_wider_than_the_margin_has_no_hole(void) {
    static uint8_t covered[512];
    static const int widths[3] = {160, 200, 66};
    static const int centres[3] = {-30, -50, -31}; /* true spans -110..50, -150..50, -64..2 */
    for (int i = 0; i < 3; i++) {
        for (int mirrored = 0; mirrored < 2; mirrored++) {
            /* Plain pictures leaving at the left; the same, mirrored, leaving at the right. */
            int centre = mirrored ? 315 - centres[i] : centres[i]; /* the game's right edge for a cut is 316 */
            fresh();
            picture(1, -1, 0, widths[i], 16);
            record(centre, 120, 0x4000, 0x98, 1, mirrored ? 0x9000 : 0x1000, 0, 0);
            margins();
            assert(set.pictures == 1 && margin_set.margin_uncut == 1);
            const RrFinePicture *narrow = &set.picture[0], *p = margin_picture(0);
            assert(p->first_column == 0 && p->columns == narrow->columns + narrow->first_column);
            assert(p->x == (mirrored ? narrow->x + narrow->first_column : narrow->x - narrow->first_column));
            margin_cover(covered);
            /* Every margin column the picture stands on, and the seam, has an entry over it. */
            int left, right;
            if (mirrored) {
                left = 315;
                right = p->x < 359 ? p->x : 359;
            } else {
                left = p->x > -39 ? p->x : -39;
                right = 0;
            }
            assert(right > left);
            for (int x = left; x < right; x++) assert(covered[x + 64]);
            assert(rr_objects_margin_gaps(&margin_set, margin_table, &margin_fine, 39) == 0);
        }
    }
    /* The gap count sees a hole: take away the entry over the column beside the screen. */
    fresh();
    picture(1, -1, 0, 160, 16);
    record(-30, 120, 0x4000, 0x98, 1, 0x1000, 0, 0);
    margins();
    assert(rr_objects_margin_gaps(&margin_set, margin_table, &margin_fine, 39) == 0);
    for (int i = 0; i < margin_entries(); i++) {
        int x = margin_entry_x(i), wide = (((margin_table[8 * i + 2] >> 2) & 3) + 1) * 8;
        if (x <= -1 && x + wide > -1) put16(margin_table + 8 * i + 6, 0x80 + 200);
    }
    assert(rr_objects_margin_gaps(&margin_set, margin_table, &margin_fine, 39) > 0);
}

static void test_the_margin_set_is_built_once_per_update(void) {
    fresh();
    picture(1, -1, 0, 32, 16);
    record(100, 120, 0x4000, 0x98, 1, 0x1000, 0, 0);
    record(340, 120, 0x4000, 0x98, 1, 0x1000, 0, 0);
    put16(ram + 0xF2B2, 152); /* the bottom of the view, which the tracker reads from the image */
    rr_object_tracker_reset(&tracker);
    tracker.margin = 39;
    tracker.margins_made = 0;
    rr_object_tracker_note(&tracker, ram, rom, sizeof rom);
    assert(tracker.margins_made == 1);
    rr_vdp_reset(&chip);
    for (int frame = 0; frame < 5; frame++) {
        /* Find the set that was just noted: the only one. */
        int slot = -1;
        for (int i = 0; i < RR_OBJECT_RECENT; i++)
            if (tracker.age[i] != 0) slot = i;
        show(&tracker.recent[slot]);
        assert(rr_object_tracker_choose(&tracker, &chip) != NULL);
        const RrFineSprites *beside = rr_object_tracker_chosen_margin(&tracker, 39);
        assert(beside != NULL && beside->picture[0] >= 0 && beside->pictures[beside->picture[0]].x == 324);
        assert(rr_object_tracker_chosen_margin(&tracker, 20) == NULL); /* made for another margin */
    }
    assert(tracker.margins_made == 1);
    /* Without a margin none is made, and none is handed out. */
    tracker.margin = 0;
    rr_object_tracker_note(&tracker, ram, rom, sizeof rom);
    assert(tracker.margins_made == 1);
    rr_object_tracker_reset(&tracker);
    assert(rr_object_tracker_chosen_margin(&tracker, 39) == NULL);
}

int main(void) {
    test_a_picture_wider_than_the_margin_has_no_hole();
    test_the_margin_set_is_built_once_per_update();
    test_a_record_wholly_right_of_319_is_in_the_margin_set();
    test_a_record_wholly_in_the_centre_is_not();
    test_a_record_dropped_for_room_stays_dropped();
    test_a_left_cut_picture_is_the_narrow_one_uncut();
    test_a_mirrored_picture_cut_at_the_right_is_uncut();
    test_a_two_part_record_whose_first_part_is_off_the_narrow_view();
    test_a_record_below_the_view_is_left_out();
    test_an_entry_without_a_picture_is_left_out();
    test_a_straddling_copy_is_in_the_margin_only();
    test_a_second_ram_supplies_extra_records();
    test_a_margin_set_needs_a_narrow_set();
    test_the_games_view_is_todays_drawing();
    test_outcomes_are_recorded();
    test_a_wider_view_keeps_a_picture_the_game_cut();
    test_a_wider_view_has_no_four_pixel_rule();
    test_no_tile_budget_in_a_view_without_tiles();
    test_tracker_keeps_the_ram_each_set_was_noted_from();
    test_noting_a_new_set_leaves_the_one_on_screen_alone();
    test_a_picture_cut_at_the_right_runs_on_to_the_screen_edge();
    test_a_record_dropped_after_its_first_part_loses_that_part_too();
    test_entries_left_by_copies_that_ran_out_are_not_described();
    test_tracker_picks_the_set_in_video_memory();
    test_tracker_ignores_the_link_byte();
    test_tracker_returns_null_when_video_memory_holds_something_else();
    test_tracker_returns_null_with_no_objects();
    test_tracker_keeps_the_last_four_and_reset_forgets();
    test_natural_size_fine_picture_is_the_plain_enlargement();
    test_one_record_stages_the_sprites_python_stages();
    test_half_size_keeps_the_whole_picture_in_fewer_pixels();
    test_mirrored_record_measures_from_its_right_end();
    test_copies_share_the_picture_with_a_sideways_step();
    test_two_part_record_makes_two_pictures();
    test_left_clip_sets_first_column();
    test_hidden_line_cuts_rows_shown();
    test_rows_above_the_view_set_first_row();
    test_dropped_record_leaves_no_picture();
    test_entries_past_the_list_get_no_picture();
    test_picture_outside_rom_is_plain();
    test_wild_pointers_and_counts_do_not_escape();
    puts("all objects tests passed");
    return 0;
}
