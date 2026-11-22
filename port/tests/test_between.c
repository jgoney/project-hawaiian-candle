/* Unit tests for the scene between two updates, on synthetic data.
 * Run with: make -C port test */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rr_between.h"
#include "rr_dashes.h"

enum {
    PICTURE_TABLE = 0x56A0A, SPRITE_SHAPES = 0x7C58, PICTURES_AT = 0x58000, ROM_SIZE = 0x60000,
    SPRITE_LIST = 0x3E40, PICTURE_BUFFER = 0x4742, FIRST_TILE = 0x634,
    PLAYER = 0x0A62, RIDER = 0x0BE2, RIDER_2 = 0x0C62
};

static uint8_t rom[ROM_SIZE];
static uint8_t ram_a[0x10000], ram_b[0x10000], scratch[0x10000], work[0x10000];
static uint8_t *ram; /* the image being built */
static RrObjectSet set, expected;
static uint32_t next_picture;
static int records, vehicles;

static void put16(uint8_t *at, int value) {
    at[0] = (uint8_t)(value >> 8);
    at[1] = (uint8_t)value;
}
static void put32(uint8_t *at, uint32_t value) {
    put16(at, (int)(value >> 16));
    put16(at + 2, (int)(value & 0xFFFF));
}
static int get16(const uint8_t *at) { return at[0] << 8 | at[1]; }

static void fresh_rom(void) {
    memset(rom, 0, sizeof rom);
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
}

/* Picture `index`, every byte different. anchor_x < 0 for none. */
static void picture(int index, int anchor_x, int anchor_y, int width, int height) {
    uint32_t at = next_picture;
    put32(rom + PICTURE_TABLE + 4 * index, at);
    uint8_t *p = rom + at;
    if (anchor_x >= 0) {
        *p++ = (uint8_t)anchor_x;
        *p++ = (uint8_t)anchor_y;
    }
    *p++ = (uint8_t)width;
    *p++ = (uint8_t)height;
    *p++ = 0x20;
    *p++ = 0;
    int row_bytes = (width + 1) / 2;
    for (int r = 0; r < height; r++) {
        for (int c = 0; c < row_bytes; c++) *p++ = (uint8_t)(((r * row_bytes + c + index) % 15 + 1) * 0x11);
        *p++ = 0;
    }
    next_picture = (uint32_t)(p - rom);
}

/* Start building an image: a race with the camera at `place` and the player listed. */
static void image(uint8_t *which, uint32_t place) {
    ram = which;
    memset(ram, 0, 0x10000);
    records = vehicles = 0;
    put32(ram + 0x27A6, 0xFF0000 | SPRITE_LIST);
    put32(ram + 0x27CE, 0xFF0000 | PICTURE_BUFFER);
    put16(ram + 0xCE86, FIRST_TILE);
    put16(ram + 0x08D6, 0xFFFF);
    put32(ram + 0x0A0A, place);
    put16(ram + 0x04F8, 0xFFFF);
}

static void vehicle(int address, uint32_t place) {
    put32(ram + 0x04F8 + 4 * vehicles, 0xFF0000u | (uint32_t)address);
    put32(ram + address + 4, place);
    vehicles++;
    put16(ram + 0x04F8 + 4 * vehicles, 0xFFFF);
}

static uint8_t *raw_record(int x, int foot, int size, int index, int flags) {
    uint8_t *at = ram + 0xCEB8 + 22 * records;
    put16(at, x);
    put16(at + 2, foot);
    put16(at + 4, size);
    put16(at + 6, 0x98);
    put16(at + 8, index);
    put16(at + 0x0A, flags);
    put16(ram + 0x08D6 + 2 * records, records);
    records++;
    put16(ram + 0x08D6 + 2 * records, 0xFFFF);
    put16(ram + 0xDC42, records);
    return at;
}

/* A roadside object of `segment`, on the side `flags` bit 14 says. */
static void roadside(int x, int foot, int size, int index, int flags, int segment) {
    uint8_t *at = raw_record(x, foot, size, index, flags | 0x1000);
    put16(at + 0x0E, 500);
    put16(at + 0x10, 0x28);
    put16(at + 0x14, segment);
}

/* A record of the vehicle at `address` (which must be listed, with its place set). */
static void vehicle_record(int x, int foot, int size, int index, int flags, int address) {
    raw_record(x, foot, size, index, flags | 0x2080);
    uint32_t place = (uint32_t)get16(ram + address + 4) << 16 | (uint32_t)get16(ram + address + 6);
    uint32_t camera = (uint32_t)get16(ram + 0x0A0A) << 16 | (uint32_t)get16(ram + 0x0A0C);
    put32(ram + 0x06D6 + 4 * (records - 1), place - camera);
}

static RrBetweenRecords read_a, read_b;

/* ---- identifying records ---- */

static void test_roadside_record_is_keyed_by_segment_side_picture(void) {
    fresh_rom();
    image(ram_a, 0x00100000);
    roadside(100, 150, 0x2000, 1, 0, 0x14);
    roadside(200, 150, 0x2000, 1, 0x4000, 0x14); /* the other side */
    roadside(120, 140, 0x1800, 1, 0, 0x15);      /* the next segment */
    roadside(130, 140, 0x1800, 2, 0, 0x15);      /* another picture there */
    image(ram_b, 0x00108000);
    roadside(90, 152, 0x2400, 1, 0, 0x14);       /* slot 0 here, moved and grown */
    rr_between_read(ram_a, &read_a);
    rr_between_read(ram_b, &read_b);
    assert(read_a.ok && read_a.count == 4 && read_b.ok && read_b.count == 1);
    assert(read_a.record[0].key != 0 && read_a.record[0].key == read_b.record[0].key);
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++) assert(read_a.record[i].key != read_a.record[j].key);
    assert(read_a.record[0].x == 100 && read_a.record[0].foot == 150 && read_a.record[0].size == 0x2000);
    assert(read_a.record[1].flags == 0x5000 && read_a.record[0].segment == 0x14);
}

static void test_vehicle_record_is_keyed_by_its_vehicle(void) {
    fresh_rom();
    image(ram_a, 0x00100000);
    vehicle(PLAYER, 0x00108000);
    vehicle(RIDER, 0x00140000);
    vehicle_record(160, 200, 0x4000, 3, 0, PLAYER);
    vehicle_record(170, 120, 0x1000, 4, 0, RIDER);
    image(ram_b, 0x00109000);
    vehicle(PLAYER, 0x00111000);
    vehicle(RIDER, 0x00148000);
    vehicle_record(172, 121, 0x1100, 5, 0, RIDER); /* other slot, other picture */
    vehicle_record(161, 200, 0x4000, 3, 0, PLAYER);
    rr_between_read(ram_a, &read_a);
    rr_between_read(ram_b, &read_b);
    assert(read_a.record[0].key != 0 && read_a.record[1].key != 0);
    assert(read_a.record[0].key == read_b.record[1].key && read_a.record[1].key == read_b.record[0].key);
    assert(read_a.record[0].key != read_a.record[1].key);
    /* A record at no listed vehicle's distance, and not roadside, cannot be followed. */
    raw_record(10, 212, 0x3C00, 6, 0x6100);
    put32(ram + 0x06D6 + 4 * 2, 0x0004BFC0);
    rr_between_read(ram_b, &read_b);
    assert(read_b.count == 3 && read_b.record[2].key == 0);
}

static void test_duplicate_keys_are_not_followed(void) {
    fresh_rom();
    image(ram_a, 0x00100000);
    roadside(100, 150, 0x2000, 1, 0, 0x14);
    roadside(104, 150, 0x2000, 1, 0, 0x14);
    roadside(120, 140, 0x1800, 1, 0, 0x15);
    vehicle(RIDER, 0x00140000);
    vehicle_record(170, 120, 0x1000, 4, 0, RIDER);
    vehicle_record(171, 120, 0x1000, 4, 0, RIDER);
    rr_between_read(ram_a, &read_a);
    assert(read_a.record[0].key == 0 && read_a.record[1].key == 0 && read_a.record[2].key != 0);
    assert(read_a.record[3].key == 0 && read_a.record[4].key == 0);
}

static void test_wild_ram_is_refused(void) {
    fresh_rom();
    image(ram_a, 0x00100000);
    roadside(100, 150, 0x2000, 1, 0, 0x14);
    put16(ram_a + 0xDC42, 0xFFFF);
    rr_between_read(ram_a, &read_a);
    assert(!read_a.ok);
    put16(ram_a + 0xDC42, RR_BETWEEN_RECORDS + 1);
    rr_between_read(ram_a, &read_a);
    assert(!read_a.ok);
    put16(ram_a + 0xDC42, 1);
    for (int i = 0; i < 0x400; i += 4) put32(ram_a + 0x04F8 + i, 0x00FFFFFE); /* a list with no end */
    rr_between_read(ram_a, &read_a);
    assert(!read_a.ok);
    /* All ones: counts, pointers and the list are everything they should not be. */
    memset(ram_a, 0xFF, sizeof ram_a);
    memset(ram_b, 0xFF, sizeof ram_b);
    rr_between_read(ram_a, &read_a);
    assert(!read_a.ok);
    assert(rr_between_objects(NULL, 0, ram_a, ram_b, rom, sizeof rom, 0x8000, &set, scratch, work) == 0);
    assert(!set.ok);
}

/* ---- the objects part-way ---- */

static void same_set(const RrObjectSet *x, const RrObjectSet *y) {
    assert(x->ok && y->ok && x->sprites == y->sprites && x->first_sprite == y->first_sprite);
    assert(x->pictures == y->pictures && x->tiles == y->tiles);
    assert(memcmp(x->list, y->list, sizeof x->list) == 0);
    assert(memcmp(x->tile_bytes, y->tile_bytes, sizeof x->tile_bytes) == 0);
    assert(memcmp(x->picture, y->picture, sizeof x->picture) == 0);
    assert(memcmp(x->fine.picture, y->fine.picture, sizeof x->fine.picture) == 0);
    assert(memcmp(x->fine.dx, y->fine.dx, sizeof x->fine.dx) == 0);
}

/* a: a tree at 100, a rider and the player; b: the tree and rider moved, the rider in another pose. */
static void moving_scene(void) {
    fresh_rom();
    picture(1, -1, 0, 16, 16);
    picture(3, 8, 16, 16, 16);
    picture(4, 8, 16, 16, 16);
    picture(5, 8, 16, 16, 16);
    image(ram_a, 0x00100000);
    vehicle(PLAYER, 0x00108000);
    vehicle(RIDER, 0x00140000);
    roadside(100, 150, 0x2000, 1, 0, 0x14);
    vehicle_record(170, 120, 0x1000, 4, 0, RIDER);
    vehicle_record(160, 140, 0x4000, 3, 0, PLAYER);
    image(ram_b, 0x00108000);
    vehicle(PLAYER, 0x00110000);
    vehicle(RIDER, 0x00144000);
    vehicle_record(160, 140, 0x4000, 3, 0, PLAYER);
    vehicle_record(174, 122, 0x1400, 5, 0x8000, RIDER);
    roadside(105, 151, 0x4000, 1, 0, 0x14);
}

static void test_objects_at_zero_are_a(void) {
    moving_scene();
    assert(rr_objects_draw(ram_a, rom, sizeof rom, &expected, work) == 1);
    memset(&set, 0xAA, sizeof set);
    assert(rr_between_objects(NULL, 0, ram_a, ram_b, rom, sizeof rom, 0, &set, scratch, work) == 1);
    same_set(&set, &expected);
    assert(set.pictures == 3);
    for (int i = 0; i < set.pictures; i++) assert(set.picture[i].fx == 0 && set.picture[i].fy == 0);
}

static void test_objects_at_one_are_b(void) {
    moving_scene();
    assert(rr_objects_draw(ram_b, rom, sizeof rom, &expected, work) == 1);
    assert(rr_between_objects(NULL, 0, ram_a, ram_b, rom, sizeof rom, 0x10000, &set, scratch, work) == 1);
    same_set(&set, &expected);
}

/* The record in `scratch` at `slot`. */
static int field(int slot, int offset) { return (int16_t)get16(scratch + 0xCEB8 + 22 * slot + offset); }

/* The picture of the result drawn from draw record `slot` (first part). */
static const RrFinePicture *picture_of(int slot) {
    for (int i = 0; i < set.pictures; i++)
        if (set.picture_record[i] == slot) return &set.picture[i];
    assert(!"no picture for that record");
    return NULL;
}

static void test_matched_record_is_halfway(void) {
    moving_scene();
    assert(rr_between_objects(NULL, 0, ram_a, ram_b, rom, sizeof rom, 0x8000, &set, scratch, work) == 1);
    /* The tree, slot 0 in a: x 100 -> 105, foot 150 -> 151, size 0x2000 -> 0x4000. */
    assert(field(0, 0) == 102 && field(0, 2) == 150);
    assert(field(0, 4) == 0x2AAA || field(0, 4) == 0x2AAB); /* through 1 / size, not 0x3000 */
    assert(picture_of(0)->fx == 0x8000 && picture_of(0)->fy == 0x8000);
    /* The rider, slot 1 in a: x 170 -> 174, foot 120 -> 122. */
    assert(field(1, 0) == 172 && field(1, 2) == 121);
    assert(picture_of(1)->fx == 0 && picture_of(1)->fy == 0);
    /* The player did not move. */
    assert(field(2, 0) == 160 && field(2, 4) == 0x4000 && picture_of(2)->fx == 0);
    /* And the result is what the game's drawing makes of those records. */
    assert(rr_objects_draw(scratch, rom, sizeof rom, &expected, work) == 1);
    for (int i = 0; i < expected.pictures; i++) {
        expected.picture[i].fx = set.picture[i].fx;
        expected.picture[i].fy = set.picture[i].fy;
    }
    same_set(&set, &expected);
}

static void test_a_quarter_of_the_way(void) {
    moving_scene();
    assert(rr_between_objects(NULL, 0, ram_a, ram_b, rom, sizeof rom, 0x4000, &set, scratch, work) == 1);
    assert(field(0, 0) == 101 && picture_of(0)->fx == 0x4000); /* 101.25 */
    assert(field(0, 2) == 150 && picture_of(0)->fy == 0x4000); /* 150.25 */
}

static void test_motion_is_carried_past_b(void) {
    moving_scene();
    assert(rr_between_objects(NULL, 0, ram_a, ram_b, rom, sizeof rom, 0x18000, &set, scratch, work) == 1);
    /* b is the base: the tree is its slot 2, the rider its slot 1. */
    assert(field(2, 0) == 107 && picture_of(2)->fx == 0x8000); /* 105 + 2.5 */
    assert(field(2, 2) == 151 && picture_of(2)->fy == 0x8000); /* 151 + 0.5 */
    assert(field(1, 0) == 176 && field(1, 2) == 123);
    assert(field(1, 8) == 5); /* b's pose */
}

static void test_a_shrinking_object_carried_on_keeps_a_size(void) {
    moving_scene();
    /* Growing 0x2000 -> 0x4000 carried a whole update further would pass the
     * camera (1 / size reaches 0): the size stays within twice b's. */
    assert(rr_between_objects(NULL, 0, ram_a, ram_b, rom, sizeof rom, 0x20000, &set, scratch, work) == 1);
    assert((uint16_t)field(2, 4) == 0x8000);
}

static void test_unmatched_records_snap(void) {
    fresh_rom();
    picture(1, -1, 0, 16, 16);
    picture(2, -1, 0, 16, 16);
    image(ram_a, 0x00100000);
    roadside(100, 150, 0x2000, 1, 0, 0x14); /* P */
    roadside(40, 140, 0x1800, 2, 0, 0x13);  /* Q: gone in b */
    image(ram_b, 0x00108000);
    roadside(104, 150, 0x2000, 1, 0, 0x14); /* P */
    roadside(250, 130, 0x1000, 2, 0, 0x20); /* R: new in b */
    assert(rr_between_objects(NULL, 0, ram_a, ram_b, rom, sizeof rom, 0x8000, &set, scratch, work) == 1);
    assert(get16(scratch + 0xDC42) == 2);
    assert(field(0, 0) == 102 && field(1, 0) == 40 && field(1, 0x14) == 0x13);
    assert(picture_of(1)->fx == 0);
    assert(rr_between_objects(NULL, 0, ram_a, ram_b, rom, sizeof rom, 0x10000, &set, scratch, work) == 1);
    assert(field(0, 0) == 104 && field(1, 0) == 250 && field(1, 0x14) == 0x20);
}

static uint8_t ram_before[0x10000];

static void test_a_leaving_object_keeps_moving(void) {
    fresh_rom();
    picture(1, -1, 0, 16, 16);
    picture(2, -1, 0, 16, 16);
    image(ram_before, 0x000F8000);
    roadside(60, 138, 0x1800, 2, 0, 0x13);  /* Q, one update earlier */
    image(ram_a, 0x00100000);
    roadside(100, 150, 0x2000, 1, 0, 0x14); /* P */
    roadside(50, 140, 0x2000, 2, 0, 0x13);  /* Q: it has passed the camera by b */
    image(ram_b, 0x00108000);
    roadside(104, 150, 0x2000, 1, 0, 0x14); /* P */
    /* Without the update before, Q stands where a has it. */
    assert(rr_between_objects(NULL, 0, ram_a, ram_b, rom, sizeof rom, 0x8000, &set, scratch, work) == 1);
    assert(field(1, 0) == 50 && field(1, 2) == 140);
    /* With it, Q goes on as it was going: 10 left and 2 down an update. */
    assert(rr_between_objects(ram_before, 0x10000, ram_a, ram_b, rom, sizeof rom, 0x8000, &set, scratch, work) == 1);
    assert(field(1, 0) == 45 && field(1, 2) == 141 && field(0, 0) == 102);
    assert((uint16_t)field(1, 4) > 0x2000 && (uint16_t)field(1, 4) <= 0x4000); /* still growing, within twice a's */
    /* The update before lasted twice as long as this one: half the motion per update. */
    assert(rr_between_objects(ram_before, 0x8000, ram_a, ram_b, rom, sizeof rom, 0x8000, &set, scratch, work) == 1);
    assert(field(1, 0) == 47 && picture_of(1)->fx == 0x8000);
    /* At a itself nothing has moved; and from b on, Q is gone as before. */
    assert(rr_between_objects(ram_before, 0x10000, ram_a, ram_b, rom, sizeof rom, 0, &set, scratch, work) == 1);
    assert(field(1, 0) == 50 && picture_of(1)->fx == 0);
    assert(rr_between_objects(ram_before, 0x10000, ram_a, ram_b, rom, sizeof rom, 0x10000, &set, scratch, work) == 1);
    assert(get16(scratch + 0xDC42) == 1);
    /* An image that is not a race's does no harm. */
    memset(ram_before, 0xFF, sizeof ram_before);
    assert(rr_between_objects(ram_before, 0x10000, ram_a, ram_b, rom, sizeof rom, 0x8000, &set, scratch, work) == 1);
    assert(field(1, 0) == 50);
}

static void test_far_moved_object_snaps(void) {
    fresh_rom();
    picture(1, -1, 0, 16, 16);
    image(ram_a, 0x00100000);
    roadside(100, 150, 0x2000, 1, 0, 0x14);
    roadside(100, 100, 0x2000, 1, 0, 0x15);
    roadside(60, 150, 0x1000, 1, 0, 0x16);
    roadside(200, 150, 0x2000, 1, 0, 0x17);
    image(ram_b, 0x00108000);
    roadside(165, 150, 0x2000, 1, 0, 0x14); /* 65 pixels */
    roadside(100, 133, 0x2000, 1, 0, 0x15); /* 33 lines */
    roadside(60, 150, 0x2001, 1, 0, 0x16);  /* more than twice the size */
    roadside(264, 182, 0x4000, 1, 0, 0x17); /* 64 pixels, 32 lines, twice: still motion */
    assert(rr_between_objects(NULL, 0, ram_a, ram_b, rom, sizeof rom, 0x8000, &set, scratch, work) == 1);
    assert(field(0, 0) == 100 && field(1, 2) == 100 && field(2, 4) == 0x1000);
    assert(picture_of(0)->fx == 0 && picture_of(1)->fy == 0);
    assert(field(3, 0) == 232 && field(3, 2) == 166);
    assert(rr_between_objects(NULL, 0, ram_a, ram_b, rom, sizeof rom, 0x18000, &set, scratch, work) == 1);
    assert(field(0, 0) == 165 && field(1, 2) == 133 && field(2, 4) == 0x2001); /* b's, not carried on */
}

static void test_pose_changes_at_the_halfway_point(void) {
    moving_scene();
    assert(rr_between_objects(NULL, 0, ram_a, ram_b, rom, sizeof rom, 0x7FFF, &set, scratch, work) == 1);
    assert(field(1, 8) == 4 && (get16(scratch + 0xCEB8 + 22 + 0x0A) & 0x8000) == 0);
    assert(rr_between_objects(NULL, 0, ram_a, ram_b, rom, sizeof rom, 0x8000, &set, scratch, work) == 1);
    assert(field(1, 8) == 5 && (get16(scratch + 0xCEB8 + 22 + 0x0A) & 0x8000) != 0);
    assert((get16(scratch + 0xCEB8 + 22 + 0x0A) & 0x7FFF) == 0x2080); /* the other flags are a's */
    assert(picture_of(1)->mirrored == 1);
}

static void test_a_sprite_without_a_full_picture_refuses_the_set(void) {
    moving_scene();
    put32(rom + PICTURE_TABLE + 4 * 1, ROM_SIZE - 6); /* the tree's rows lie past the ROM's end */
    rom[ROM_SIZE - 6] = 16;
    rom[ROM_SIZE - 5] = 16;
    assert(rr_between_objects(NULL, 0, ram_a, ram_b, rom, sizeof rom, 0x8000, &set, scratch, work) == 0);
    assert(!set.ok);
}

/* ---- the sprite table ---- */

static RrVdp chip;
static uint8_t table[640];
static RrFineSprites fine;

static void entry(uint8_t *at, int x, int y, int link) {
    put16(at, y + 128);
    at[2] = 0x05;
    at[3] = (uint8_t)link;
    put16(at + 4, 0x0700);
    put16(at + 6, x + 128);
}

static void test_sprite_table_puts_the_games_other_sprites_after(void) {
    static RrObjectSet on_screen;
    moving_scene();
    assert(rr_between_objects(NULL, 0, ram_a, ram_b, rom, sizeof rom, 0x8000, &set, scratch, work) == 1);
    assert(set.sprites == 3 && set.first_sprite == 0);
    memset(&chip, 0, sizeof chip);
    chip.reg[5] = 0x62; /* sprite table at 0xC400 */
    memset(&on_screen, 0, sizeof on_screen);
    on_screen.ok = 1;
    on_screen.sprites = 2;
    /* Video memory: two object entries, then three dashes, chained 0 1 2 4 3. */
    entry(chip.vram + 0xC400 + 0, 10, 10, 1);
    entry(chip.vram + 0xC400 + 8, 20, 20, 2);
    entry(chip.vram + 0xC400 + 16, 30, 30, 4);
    entry(chip.vram + 0xC400 + 24, 40, 40, 0);
    entry(chip.vram + 0xC400 + 32, 50, 50, 3);
    memset(table, 0xAA, sizeof table);
    assert(rr_between_sprite_table(&set, &on_screen, &chip, table, &fine) == 1);
    for (int i = 0; i < 6; i++) assert(table[8 * i + 3] == (i == 5 ? 0 : i + 1));
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 8; k++)
            if (k != 3) assert(table[8 * i + k] == set.list[8 * i + k]);
    static const int order[3] = {2, 4, 3};
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 8; k++)
            if (k != 3) assert(table[8 * (3 + i) + k] == chip.vram[0xC400 + 8 * order[i] + k]);
    assert(fine.table == table && fine.pictures == set.picture);
    for (int i = 0; i < 3; i++) assert(fine.picture[i] == set.fine.picture[i] && fine.picture[i] >= 0);
    for (int i = 3; i < RR_SPRITES_MAX; i++) assert(fine.picture[i] == -1);
}

static void test_sprite_table_with_nothing_after_the_objects(void) {
    static RrObjectSet on_screen;
    moving_scene();
    assert(rr_between_objects(NULL, 0, ram_a, ram_b, rom, sizeof rom, 0, &set, scratch, work) == 1);
    memset(&chip, 0, sizeof chip);
    chip.reg[5] = 0x62;
    memset(&on_screen, 0, sizeof on_screen);
    on_screen.ok = 1;
    on_screen.sprites = 2;
    entry(chip.vram + 0xC400 + 0, 10, 10, 1);
    entry(chip.vram + 0xC400 + 8, 20, 20, 0);
    assert(rr_between_sprite_table(&set, &on_screen, &chip, table, &fine) == 1);
    assert(table[3] == 1 && table[8 + 3] == 2 && table[16 + 3] == 0);
}

static void test_sprite_table_refuses_more_than_80(void) {
    static RrObjectSet on_screen;
    moving_scene();
    assert(rr_between_objects(NULL, 0, ram_a, ram_b, rom, sizeof rom, 0, &set, scratch, work) == 1);
    memset(&chip, 0, sizeof chip);
    chip.reg[5] = 0x62;
    memset(&on_screen, 0, sizeof on_screen);
    on_screen.ok = 1;
    on_screen.sprites = 1;
    /* 79 entries chained after the on-screen set's one: 3 + 79 is too many. */
    for (int i = 0; i < 80; i++) entry(chip.vram + 0xC400 + 8 * i, i, i, i == 79 ? 0 : i + 1);
    assert(rr_between_sprite_table(&set, &on_screen, &chip, table, &fine) == 0);
    chip.vram[0xC400 + 8 * 77 + 3] = 0; /* entries 1 to 77 follow: 3 + 77 = 80 */
    assert(rr_between_sprite_table(&set, &on_screen, &chip, table, &fine) == 1);
    assert(table[8 * 79 + 3] == 0 && table[8 * 78 + 3] == 79);
}

/* ---- the clock ---- */

static RrBetween between;

/* Updates with these stamps, all good, each lasting until the next. */
static void stamped(int mode, int count, const int *stamps) {
    memset(&between, 0, sizeof between);
    between.mode = mode;
    between.count = count;
    for (int i = 0; i < count; i++) {
        between.update[i].good = 1;
        between.update[i].id = (uint64_t)i + 1;
        between.update[i].stamp = (uint64_t)stamps[i];
        if (i > 0) between.length[between.lengths++] = (uint8_t)(stamps[i] - stamps[i - 1]);
    }
    between.delay = rr_between_target_delay(&between);
}

static int older, newer;
static int32_t place_between;

static void test_predict_at_the_stamp_is_the_newest_update(void) {
    stamped(RR_BETWEEN_PREDICT, 2, (const int[]){10, 15});
    assert(rr_between_target_delay(&between) == 0);
    assert(rr_between_clock(&between, 15, &older, &newer, &place_between) == 1);
    assert(older == 0 && newer == 1 && place_between == 0x10000);
}

static void test_predict_carries_on_evenly(void) {
    stamped(RR_BETWEEN_PREDICT, 2, (const int[]){10, 15});
    for (int frame = 16; frame <= 20; frame++) {
        assert(rr_between_clock(&between, (uint64_t)frame, &older, &newer, &place_between) == 1);
        assert(older == 0 && newer == 1 && place_between == 0x10000 + ((frame - 15) << 16) / 5);
    }
}

static void test_blend_runs_one_mean_update_behind(void) {
    stamped(RR_BETWEEN_BLEND, 3, (const int[]){10, 15, 20});
    assert(rr_between_target_delay(&between) == 5 << 16);
    assert(rr_between_clock(&between, 22, &older, &newer, &place_between) == 1); /* the moment 17 */
    assert(older == 1 && newer == 2 && place_between == (2 << 16) / 5);
    assert(rr_between_clock(&between, 18, &older, &newer, &place_between) == 1); /* the moment 13 */
    assert(older == 0 && newer == 1 && place_between == (3 << 16) / 5);
    assert(rr_between_clock(&between, 20, &older, &newer, &place_between) == 1);
    assert(older == 1 && newer == 2 && place_between == 0); /* exactly the update at 15 */
}

static void test_blend_guesses_when_an_update_runs_long(void) {
    stamped(RR_BETWEEN_BLEND, 3, (const int[]){10, 15, 20});
    assert(rr_between_clock(&between, 27, &older, &newer, &place_between) == 1);
    assert(older == 1 && newer == 2 && place_between == (7 << 16) / 5);
}

static void test_blend_delay_is_the_mean_length(void) {
    stamped(RR_BETWEEN_BLEND, 4, (const int[]){10, 14, 19, 27}); /* lengths 4, 5, 8 */
    assert(rr_between_target_delay(&between) == (17 << 16) / 3);
}

static void test_late_blend_uses_the_longest_length(void) {
    stamped(RR_BETWEEN_LATE, 4, (const int[]){10, 14, 19, 27});
    assert(rr_between_target_delay(&between) == 8 << 16);
    assert(rr_between_clock(&between, 27, &older, &newer, &place_between) == 1);
    assert(older == 2 && newer == 3 && place_between == 0); /* the moment 19 */
}

static void test_clock_holds_when_no_update_arrives(void) {
    stamped(RR_BETWEEN_PREDICT, 2, (const int[]){10, 15});
    assert(rr_between_clock(&between, 20, &older, &newer, &place_between) == 1 && place_between == 0x20000);
    assert(rr_between_clock(&between, 115, &older, &newer, &place_between) == 1 && place_between == 0x20000);
}

static void test_before_the_oldest_update_is_the_oldest(void) {
    stamped(RR_BETWEEN_LATE, 2, (const int[]){10, 15});
    assert(rr_between_clock(&between, 12, &older, &newer, &place_between) == 1); /* the moment 7 */
    assert(older == 0 && newer == 1 && place_between == 0);
}

static void test_pair_never_spans_a_broken_update(void) {
    stamped(RR_BETWEEN_PREDICT, 3, (const int[]){10, 15, 20});
    between.update[1].good = 0;
    for (int frame = 10; frame < 40; frame++)
        assert(rr_between_clock(&between, (uint64_t)frame, &older, &newer, &place_between) == 0);
    stamped(RR_BETWEEN_BLEND, 4, (const int[]){10, 15, 20, 25});
    between.update[1].good = 0;
    assert(rr_between_clock(&between, 22, &older, &newer, &place_between) == 0); /* moment 17: 15 to 20 */
    assert(rr_between_clock(&between, 27, &older, &newer, &place_between) == 1); /* moment 22: 20 to 25 */
    assert(older == 2 && newer == 3);
    stamped(RR_BETWEEN_PREDICT, 1, (const int[]){10});
    assert(rr_between_clock(&between, 12, &older, &newer, &place_between) == 0);
}

/* ---- a whole synthetic race: road, objects, video memory, both trackers ---- */

enum { SCALE = 3 };
static uint8_t race[0x10000];
static RrRoadTracker road_tracker;
static RrObjectTracker object_tracker;
static int sky_a = 100, sky_b = 200, inputs_fit = 1, dashes_fit = 1, reference_line = 60, sideways = -40;

static void race_rom(void) {
    fresh_rom();
    for (int n = 2; n < 256; n++) put16(rom + 0x15472 + 2 * n, 65536 / n);
    for (int d = 0; d < 16384; d++) put16(rom + 0x27C84 + 2 * d, (int)(0x10000L * 64 / (d + 64) - 1));
    picture(1, -1, 0, 16, 16);
    picture(3, 8, 16, 16, 16);
}

static void new_race(int mode) {
    race_rom();
    rr_road_tracker_reset(&road_tracker);
    rr_object_tracker_reset(&object_tracker);
    memset(&between, 0, sizeof between);
    rr_between_reset(&between);
    rr_between_set_mode(&between, mode);
    memset(&chip, 0, sizeof chip);
    sky_a = 100;
    sky_b = 200;
    inputs_fit = 1;
    dashes_fit = 1;
    reference_line = 60;
    sideways = -40;
}

/* The trackers choose for this frame and the update history is told. */
static void frame_begins(int frame) {
    (void)rr_road_tracker_choose(&road_tracker, race, rom, sizeof rom, &chip, SCALE);
    (void)rr_object_tracker_choose(&object_tracker, &chip);
    rr_between_note(&between, (uint64_t)frame, &road_tracker, &object_tracker, &chip);
}

/* The game makes an update (camera at `place`, a tree at tree_x) and it shows from `frame` on. */
static void update_shows(int frame, uint32_t place, int tree_x) {
    static RrRoadInputs inputs;
    static RrFineRoad fine_road;
    static RrRoad whole;
    static RrRoadShifts table_shifts;
    static RrObjectSet staged;
    image(race, place);
    put16(race + 0x0302, 0xE038);
    put16(race + 0x1CA0, 32);
    put16(race + 0x0A02, sideways);
    put16(race + 0x0A08, 96);
    put16(race + 0xF2B2, 152);
    put16(race + 0xF2BC, reference_line);
    put16(race + 0xF2BE, 160);
    for (int i = 0x12; i < 0x1C; i++) race[0xD828 + 2 * i] = 2; /* a bend ahead, so the edges move as the camera does */
    vehicle(PLAYER, place + 0x8000);
    roadside(tree_x, 150, 0x2000, 1, 0, 0x14);
    vehicle_record(160, 140, 0x4000, 3, 0, PLAYER);
    rr_road_tracker_note_inputs(&road_tracker, race);
    if (!inputs_fit) put16(race + 0x0A02, -90); /* the points will be another projection's */
    assert(rr_road_read_inputs(race, &inputs) == 1);
    assert(rr_road_project(&inputs, rom, sizeof rom, &fine_road, &whole) == 1);
    for (int i = 0; i < whole.count; i++) {
        put16(race + 0x1E20 + 2 * i, whole.line[i]);
        put16(race + 0xD2B8 + 2 * i, whole.clipped_line[i]);
        put16(race + 0x1E84 + 2 * i, whole.left[i]);
        put16(race + 0x1F4C + 2 * i, whole.right[i]);
        put16(race + 0x1DBC + 2 * i, whole.centre[i]);
    }
    rr_road_tracker_note(&road_tracker, race, rom, sizeof rom);
    /* The game's own description of the dashes, as the port's model makes it. */
    static RrRoad as_read;
    static RrDashLines lines;
    assert(rr_road_read(race, &as_read) == 1);
    rr_dashes_lines(&as_read, place, rr_dashes_stop_line(race), rom, sizeof rom, &lines);
    for (int l = 0; l < RR_SCREEN_H; l++) {
        put16(race + 0x2EDE + 2 * l, lines.start[l]);
        put16(race + 0x309E + 2 * l, dashes_fit ? lines.width[l] : lines.width[l] + 1);
    }
    rr_dashes_note(&road_tracker, race, rom, sizeof rom);
    rr_object_tracker_note(&object_tracker, race, rom, sizeof rom);
    /* Video memory as the game's block copies leave it. */
    chip.reg[5] = 0x62;  /* sprite table at 0xC400 */
    chip.reg[11] = 0x03; /* a shift for every line */
    chip.reg[13] = 0x30; /* shift table at 0xC000 */
    rr_road_shifts(&whole, rom, sizeof rom, 1, &table_shifts);
    for (int l = 0; l < RR_SCREEN_H; l++) {
        put16(&chip.vram[0xC000 + l * 4], table_shifts.have[l] ? table_shifts.a[l] >> 16 : sky_a);
        put16(&chip.vram[0xC000 + l * 4 + 2], table_shifts.have[l] ? table_shifts.b[l] >> 16 : sky_b);
    }
    assert(rr_objects_draw(race, rom, sizeof rom, &staged, work) == 1);
    memcpy(&chip.vram[0xC400], staged.list, sizeof staged.list);
    memcpy(&chip.vram[staged.tile_base * 32], staged.tile_bytes, (size_t)staged.tiles * 32);
    /* One dash sprite after the objects, as the game adds them. */
    for (int i = 0; i < staged.sprites; i++) chip.vram[0xC400 + 8 * i + 3] = (uint8_t)(i + 1);
    entry(chip.vram + 0xC400 + 8 * staged.sprites, 150, 140, 0);
    put16(chip.vram + 0xC400 + 8 * staged.sprites + 4, 0x6000 | 0x700);
    frame_begins(frame);
}

static int scene(int frame) {
    return rr_between_frame(&between, (uint64_t)frame, &road_tracker, &object_tracker, &chip, rom, sizeof rom, SCALE);
}

static void test_note_adds_an_update_once(void) {
    new_race(RR_BETWEEN_PREDICT);
    update_shows(10, 0x00100000, 100);
    frame_begins(11);
    frame_begins(12);
    assert(between.count == 1 && between.update[0].stamp == 10 && between.update[0].good && between.lengths == 0);
    assert(between.update[0].inputs.place == 0x00100000);
    assert(memcmp(between.update[0].object_ram, race, sizeof race) == 0);
    assert(get16(between.update[0].shifts + 4 * 10) == 100 && get16(between.update[0].shifts + 4 * 10 + 2) == 200);
    update_shows(15, 0x00108000, 105);
    assert(between.count == 2 && between.update[1].stamp == 15 && between.lengths == 1 && between.length[0] == 5);
    assert(between.update[0].stamp == 10 && between.update[0].inputs.place == 0x00100000);
}

static void test_only_the_newest_updates_are_kept(void) {
    new_race(RR_BETWEEN_PREDICT);
    for (int i = 0; i < RR_BETWEEN_UPDATES + 2; i++) update_shows(10 + 5 * i, 0x00100000 + 0x8000u * (uint32_t)i, 100 + i);
    assert(between.count == RR_BETWEEN_UPDATES);
    assert(between.update[0].stamp == 20 && between.update[RR_BETWEEN_UPDATES - 1].stamp == 10 + 5 * (RR_BETWEEN_UPDATES + 1));
    for (int i = 0; i < 40; i++) update_shows(100 + 2 * i, 0x00200000, 100);
    assert(between.lengths == RR_BETWEEN_LENGTHS && between.length[RR_BETWEEN_LENGTHS - 1] == 2);
}

static void test_a_road_without_inputs_makes_a_broken_update(void) {
    new_race(RR_BETWEEN_PREDICT);
    update_shows(10, 0x00100000, 100);
    update_shows(15, 0x00108000, 105);
    assert(scene(15) == 1);
    inputs_fit = 0; /* the next road's points are not what any noted inputs project to */
    update_shows(20, 0x00110000, 110);
    inputs_fit = 1;
    assert(between.count == 3 && !between.update[2].good && between.update[2].stamp == 20);
    assert(scene(21) == 0 && between.fell_back[RR_BETWEEN_NO_PAIR] == 1);
    update_shows(25, 0x00118000, 115);
    assert(scene(26) == 0 && between.fell_back[RR_BETWEEN_NO_PAIR] == 2); /* one good update after it is not a pair */
    update_shows(30, 0x00120000, 120);
    assert(scene(31) == 1);
}

static void test_set_mode_forgets_updates(void) {
    new_race(RR_BETWEEN_PREDICT);
    update_shows(10, 0x00100000, 100);
    update_shows(15, 0x00108000, 105);
    rr_between_set_mode(&between, RR_BETWEEN_PREDICT); /* no change */
    assert(between.count == 2);
    rr_between_set_mode(&between, 99);
    assert(between.mode == RR_BETWEEN_PREDICT && between.count == 2);
    rr_between_set_mode(&between, RR_BETWEEN_BLEND);
    assert(between.mode == RR_BETWEEN_BLEND && between.count == 0 && between.lengths == 0);
    between.frames = 7;
    rr_between_reset(&between);
    assert(between.mode == RR_BETWEEN_BLEND && between.frames == 0);
}

static void test_off_makes_no_scene(void) {
    new_race(RR_BETWEEN_OFF);
    update_shows(10, 0x00100000, 100);
    update_shows(15, 0x00108000, 105);
    assert(scene(16) == 0 && between.frames == 0 && between.fell_back[RR_BETWEEN_NO_PAIR] == 0);
}

static void test_frame_at_an_update_is_that_updates_scene(void) {
    new_race(RR_BETWEEN_PREDICT);
    update_shows(10, 0x00100000, 100);
    update_shows(15, 0x00108000, 105);
    assert(scene(15) == 1);
    assert(between.t == 0x10000 && between.between_frames == 1 && between.guessed_frames == 0);
    int road_lines = 0;
    for (int l = 0; l < RR_SCREEN_H; l++) {
        assert(between.shifts.have[l] == 1);
        if (!road_tracker.shifts.have[l]) continue;
        road_lines++;
        assert(memcmp(&between.shifts.a[l * SCALE], &road_tracker.shifts.a[l * SCALE], SCALE * sizeof(int32_t)) == 0);
        assert(memcmp(&between.shifts.b[l * SCALE], &road_tracker.shifts.b[l * SCALE], SCALE * sizeof(int32_t)) == 0);
    }
    assert(road_lines > 40);
    /* A sky line: the update's own shift on every row; and what video memory held, to check against. */
    for (int r = 0; r < SCALE; r++) assert(between.shifts.a[10 * SCALE + r] == 100 << 16 && between.shifts.b[10 * SCALE + r] == 200 << 16);
    assert(between.expect.a[10 * SCALE] == 100 << 16 && between.expect.b[10 * SCALE] == 200 << 16);
    assert(between.expect.a[150 * SCALE] >> 16 == (int16_t)get16(&chip.vram[0xC000 + 150 * 4]));
    /* The objects are the set on screen. */
    const RrObjectSet *shown = rr_object_tracker_chosen(&object_tracker);
    assert(shown != NULL && between.set.sprites == shown->sprites && between.fine.table == between.table);
    for (int i = 0; i < shown->sprites; i++)
        for (int k = 0; k < 8; k++)
            if (k != 3) assert(between.table[8 * i + k] == shown->list[8 * i + k]);
    assert(memcmp(between.set.picture, shown->picture, sizeof shown->picture) == 0);
}

static void test_frame_past_an_update_moves_on(void) {
    static RrRoadShifts at_update;
    new_race(RR_BETWEEN_PREDICT);
    update_shows(10, 0x00100000, 100);
    update_shows(15, 0x00108000, 105);
    assert(scene(15) == 1);
    at_update = between.shifts;
    assert(scene(17) == 1);
    assert(between.t == 0x10000 + (2 << 16) / 5 && between.guessed_frames == 1);
    /* The tree: 105 + 5 * 2 / 5 = 107, less the sliver the clock's division drops. */
    long tree = ((long)(int16_t)get16(between.scratch + 0xCEB8) << 16) + between.set.picture[0].fx;
    assert(between.set.picture_record[0] == 0 && tree <= 107L << 16 && tree > (107L << 16) - 16);
    assert(memcmp(between.shifts.a, at_update.a, sizeof at_update.a) != 0);
}

static void test_no_motion_gives_no_motion(void) {
    static RrRoadShifts first;
    static uint8_t first_table[640];
    new_race(RR_BETWEEN_PREDICT);
    update_shows(10, 0x00100000, 100);
    update_shows(15, 0x00100000, 100);
    assert(scene(15) == 1);
    first = between.shifts;
    memcpy(first_table, between.table, sizeof first_table);
    for (int frame = 16; frame < 30; frame += 3) {
        assert(scene(frame) == 1);
        assert(memcmp(&first, &between.shifts, sizeof first) == 0);
        assert(memcmp(first_table, between.table, sizeof first_table) == 0);
        for (int i = 0; i < between.set.pictures; i++) assert(between.set.picture[i].fx == 0);
    }
}

static void test_sky_shift_goes_the_short_way_round(void) {
    new_race(RR_BETWEEN_BLEND);
    sky_a = 1020;
    sky_b = 100;
    update_shows(10, 0x00100000, 100);
    sky_a = 4;
    sky_b = 300; /* more than 64 pixels on: a jump */
    update_shows(14, 0x00108000, 105);
    between.delay = 2 << 16; /* frame 14 is the moment 12: half way */
    assert(scene(14) == 1 && between.t == 0x8000);
    assert(((between.shifts.a[10 * SCALE] >> 16) & 0x3FF) == 0 && (between.shifts.a[10 * SCALE] & 0xFFFF) == 0);
    assert(between.shifts.b[10 * SCALE + 1] == 100 << 16); /* not slid: the older update's */
    between.delay = 1 << 16; /* three quarters */
    assert(scene(14) == 1 && between.t == 0xC000);
    assert(((between.shifts.a[10 * SCALE] >> 16) & 0x3FF) == 2);
    assert(between.shifts.b[10 * SCALE] == 100 << 16);
    between.delay = 0;
    assert(scene(14) == 1 && between.shifts.b[10 * SCALE] == 300 << 16);
}

static int first_road_line(const uint8_t *have) {
    int line = 0;
    while (line < RR_SCREEN_H && !have[line]) line++;
    return line;
}

/* The horizon on screen (where the video chip turns from sky and hills to
 * ground) is the game's, and is not moved by the in-between picture. So the
 * in-between road must not be drawn on lines above it: they hold hills. */
static void test_the_road_is_not_drawn_above_the_horizon_on_screen(void) {
    new_race(RR_BETWEEN_BLEND);
    reference_line = 50;
    update_shows(10, 0x00100000, 100);
    int older_top = first_road_line(road_tracker.whole.have);
    reference_line = 80; /* the horizon has dropped: more sky */
    sky_a = 110;         /* and the hills have slid sideways */
    update_shows(14, 0x00108000, 105);
    int screen_top = first_road_line(road_tracker.whole.have);
    assert(screen_top >= older_top + 20);
    between.delay = 2 << 16; /* frame 14 is the moment 12: half way, where the road's own top is between the two */
    assert(scene(14) == 1 && between.t == 0x8000);
    for (int l = older_top; l < screen_top; l++) {
        assert(between.road_have[l] == 0);
        /* Drawn as video memory has the line now: the newest update's hills. */
        for (int r = 0; r < SCALE; r++) assert(between.shifts.a[l * SCALE + r] == 110 << 16 && between.shifts.b[l * SCALE + r] == 200 << 16);
    }
    assert(between.road_have[screen_top + 2] == 1);
    /* Lines that were never road are slid as before: the sky above both
     * horizons, and the dashboard and mirrors below the view. */
    assert(between.shifts.a[10 * SCALE] == 105 << 16);
    assert(between.shifts.a[200 * SCALE] == 105 << 16 && between.shifts.a[223 * SCALE + 1] == 105 << 16);
}

/* And where the horizon on screen is above the in-between road's top, those
 * lines are ground: they are drawn with the road on screen, not as sky. */
static void test_ground_above_the_in_between_road_is_the_road_on_screen(void) {
    new_race(RR_BETWEEN_BLEND);
    reference_line = 80;
    update_shows(10, 0x00100000, 100);
    reference_line = 50; /* the horizon has risen: more ground */
    update_shows(14, 0x00108000, 105);
    int screen_top = first_road_line(road_tracker.whole.have);
    between.delay = 2 << 16;
    assert(scene(14) == 1 && between.t == 0x8000);
    int own_top = first_road_line(between.road_have);
    assert(own_top >= screen_top + 5);
    for (int l = screen_top; l < own_top; l++) {
        assert(memcmp(&between.shifts.a[l * SCALE], &road_tracker.shifts.a[l * SCALE], SCALE * sizeof(int32_t)) == 0);
        assert(memcmp(&between.shifts.b[l * SCALE], &road_tracker.shifts.b[l * SCALE], SCALE * sizeof(int32_t)) == 0);
    }
}

static void test_delay_moves_a_quarter_frame_at_a_time(void) {
    new_race(RR_BETWEEN_LATE);
    update_shows(10, 0x00100000, 100);
    update_shows(14, 0x00108000, 105);
    assert(between.delay == 4 << 16); /* the first length sets it outright */
    update_shows(22, 0x00110000, 110); /* a length of 8: the target is now 8 */
    assert(rr_between_target_delay(&between) == 8 << 16 && between.delay == (4 << 16) + 0x4000);
    frame_begins(23);
    assert(between.delay == (4 << 16) + 0x8000);
    for (int frame = 24; frame < 60; frame++) frame_begins(frame);
    assert(between.delay == 8 << 16);
}

static void test_clock_never_steps_back(void) {
    static const int arrivals[] = {10, 15, 19, 26, 34, 36, 38, 47};
    for (int mode = RR_BETWEEN_PREDICT; mode <= RR_BETWEEN_LATE; mode++) {
        new_race(mode);
        int next = 0;
        double last = -1;
        for (int frame = 10; frame < 60; frame++) {
            if (next < 8 && arrivals[next] == frame) {
                update_shows(frame, 0x00100000 + 0x4000u * (uint32_t)next, 100 + next);
                next++;
            } else {
                frame_begins(frame);
            }
            if (!rr_between_clock(&between, (uint64_t)frame, &older, &newer, &place_between)) continue;
            double gap = (double)(between.update[newer].stamp - between.update[older].stamp);
            double moment = (double)between.update[older].stamp + gap * place_between / 65536.0;
            if (mode != RR_BETWEEN_PREDICT) assert(moment >= last - 0.001);
            assert(moment <= frame + 0.001); /* never ahead of the present by more than a guess allows */
            last = moment;
        }
        assert(last > 40);
    }
}

static void test_frame_falls_back_and_counts_why(void) {
    /* One update only. */
    new_race(RR_BETWEEN_PREDICT);
    update_shows(10, 0x00100000, 100);
    assert(scene(11) == 0 && between.fell_back[RR_BETWEEN_NO_PAIR] == 1 && between.frames == 1);
    /* The camera jumped five segments. */
    update_shows(15, 0x00150000, 100);
    assert(scene(16) == 0 && between.fell_back[RR_BETWEEN_ROAD] == 1);
    /* No set on screen that the tracker knows. */
    chip.vram[0xC400 + 6] ^= 0x01;
    frame_begins(17);
    assert(scene(17) == 0 && between.fell_back[RR_BETWEEN_NO_PAIR] == 2);
    chip.vram[0xC400 + 6] ^= 0x01;
    /* An object with no full picture. */
    new_race(RR_BETWEEN_PREDICT);
    put32(rom + PICTURE_TABLE + 4 * 1, ROM_SIZE - 6);
    rom[ROM_SIZE - 6] = 16;
    rom[ROM_SIZE - 5] = 16;
    update_shows(10, 0x00100000, 100);
    update_shows(15, 0x00108000, 105);
    assert(scene(16) == 0 && between.fell_back[RR_BETWEEN_OBJECTS] == 1);
    /* More of the game's other sprites than there is room for. */
    new_race(RR_BETWEEN_PREDICT);
    update_shows(10, 0x00100000, 100);
    update_shows(15, 0x00108000, 105);
    assert(scene(16) == 1);
    int objects = rr_object_tracker_chosen(&object_tracker)->sprites;
    for (int i = 0; i < 79; i++) chip.vram[0xC400 + 8 * i + 3] = (uint8_t)(i + 1);
    for (int i = objects; i < 80; i++) put16(&chip.vram[0xC400 + 8 * i], 128 + 200);
    chip.vram[0xC400 + 8 * 79 + 3] = (uint8_t)objects; /* and a loop back, for good measure */
    assert(scene(16) == 1); /* 80 in all still fit */
    assert(between.table[8 * 79 + 3] == 0);
    assert(between.fell_back[RR_BETWEEN_TABLE] == 0);
}

/* ---- the dashes ---- */

/* The row where the nearest dash begins: the topmost row of the unbroken run of strips at the bottom of the road. */
static int nearest_dash_top(void) {
    int row = 152 * SCALE - 1;
    assert(between.strips.to[row] > between.strips.from[row]);
    while (row > 0 && between.strips.to[row - 1] > between.strips.from[row - 1]) row--;
    return row;
}

static void test_the_scene_at_an_update_has_that_updates_dashes(void) {
    static RrStrips expected;
    new_race(RR_BETWEEN_PREDICT);
    update_shows(10, 0x00100000, 100);
    update_shows(15, 0x00108000, 105);
    assert(between.update[0].dashes && between.update[1].dashes);
    assert(scene(15) == 1);
    assert(between.fine.strips == &between.strips && between.strips.scale == SCALE);
    rr_dashes_strips(&road_tracker.recent_fine[road_tracker.chosen], 0x00108000, road_tracker.whole.have, SCALE,
                     &expected);
    assert(memcmp(expected.from, between.strips.from, sizeof expected.from) == 0);
    assert(memcmp(expected.to, between.strips.to, sizeof expected.to) == 0);
    /* The game's dash sprite is in the table, after the objects, and marked as not to be drawn. */
    int dash = between.set.sprites;
    assert((between.table[8 * dash + 4] & 0xE0) == 0x60 && between.fine.skip[dash] == 1);
    for (int i = 0; i < dash; i++) assert(between.fine.skip[i] == 0);
}

static void test_dashes_move_between_updates(void) {
    new_race(RR_BETWEEN_PREDICT);
    update_shows(10, 0x00100000, 100);
    update_shows(15, 0x00108000, 105); /* half a segment on */
    const RrObjectSet *shown = rr_object_tracker_chosen(&object_tracker);
    assert(rr_between_scene(&between, 0, 1, 0, shown, &chip, NULL, rom, sizeof rom, SCALE) < 0);
    int at_a = nearest_dash_top();
    assert(rr_between_scene(&between, 0, 1, 0x8000, shown, &chip, NULL, rom, sizeof rom, SCALE) < 0);
    int halfway = nearest_dash_top();
    assert(rr_between_scene(&between, 0, 1, 0x10000, shown, &chip, NULL, rom, sizeof rom, SCALE) < 0);
    int at_b = nearest_dash_top();
    /* The dash's far end comes down the screen as the camera nears it. */
    assert(at_a < halfway && halfway < at_b && at_b - at_a > 6);
}

static void test_unproven_dashes_keep_the_games_sprites(void) {
    new_race(RR_BETWEEN_PREDICT);
    update_shows(10, 0x00100000, 100);
    dashes_fit = 0; /* the game's arrays are not what the model makes for this update */
    update_shows(15, 0x00108000, 105);
    assert(between.update[0].dashes && !between.update[1].dashes);
    assert(scene(16) == 1);
    assert(between.fine.strips == NULL);
    for (int i = 0; i < RR_SPRITES_MAX; i++) assert(between.fine.skip[i] == 0);
    dashes_fit = 1;
    update_shows(20, 0x00110000, 110);
    assert(scene(21) == 1 && between.fine.strips == NULL); /* the pair still has the unproven one */
    update_shows(25, 0x00118000, 115);
    assert(scene(26) == 1 && between.fine.strips == &between.strips);
}

/* ---- what the final review found ---- */

static void test_a_frame_with_no_set_on_screen_forgets_the_updates(void) {
    new_race(RR_BETWEEN_PREDICT);
    update_shows(10, 0x00100000, 100);
    update_shows(15, 0x00108000, 105);
    assert(scene(15) == 1);
    chip.vram[0xC400 + 6] ^= 0x01; /* video memory holds a set nobody described */
    frame_begins(16);
    assert(between.count == 0 && scene(16) == 0);
    chip.vram[0xC400 + 6] ^= 0x01;
    update_shows(20, 0x00110000, 110);
    assert(between.count == 1 && scene(21) == 0); /* no pair across the gap */
    update_shows(25, 0x00118000, 115);
    assert(scene(26) == 1 && between.update[0].stamp == 20);
}

static void test_a_long_gap_starts_afresh(void) {
    new_race(RR_BETWEEN_LATE);
    update_shows(10, 0x00100000, 100);
    update_shows(15, 0x00108000, 105);
    for (int frame = 16; frame < 15 + RR_BETWEEN_GAP; frame++) frame_begins(frame); /* a pause */
    assert(between.count == 2);
    update_shows(15 + RR_BETWEEN_GAP, 0x00110000, 110); /* exactly the limit: still one race */
    assert(between.count == 3 && between.length[between.lengths - 1] == RR_BETWEEN_GAP);
    for (int frame = 46; frame < 100; frame++) frame_begins(frame);
    update_shows(100, 0x00118000, 115);
    /* The update before it lasted too long to be motion: nothing is kept from before, no length taken. */
    assert(between.count == 1 && between.lengths == 0 && between.update[0].stamp == 100);
    update_shows(105, 0x00120000, 120);
    assert(between.delay == 5 << 16 && scene(110) == 1);
}

static void test_an_earlier_set_returning_is_not_a_new_update(void) {
    static RrVdp first;
    new_race(RR_BETWEEN_PREDICT);
    update_shows(10, 0x00100000, 100);
    first = chip;
    update_shows(15, 0x00108000, 105);
    chip = first; /* video memory shows the earlier update again for a frame */
    frame_begins(16);
    assert(between.count == 2 && between.update[1].stamp == 15 && between.returned == 1);
    assert(between.update[1].inputs.place == 0x00108000);
    update_shows(20, 0x00110000, 110);
    assert(between.count == 3 && between.update[2].inputs.place == 0x00110000);
}

static void test_a_stopped_game_holds_within_one_update_in_every_mode(void) {
    for (int mode = RR_BETWEEN_PREDICT; mode <= RR_BETWEEN_LATE; mode++) {
        stamped(mode, 3, (const int[]){10, 15, 20});
        assert(rr_between_clock(&between, 620, &older, &newer, &place_between) == 1);
        assert(older == 1 && newer == 2 && place_between == 0x20000);
    }
}

static void test_the_comparisons_report_a_difference(void) {
    static RrObjectSet other;
    new_race(RR_BETWEEN_PREDICT);
    update_shows(10, 0x00100000, 100);
    update_shows(15, 0x00108000, 105);
    assert(scene(15) == 1);
    const RrObjectSet *shown = rr_object_tracker_chosen(&object_tracker);
    /* The road. */
    assert(rr_between_same_road(&between, &road_tracker.shifts) == 1);
    between.shifts.a[130 * SCALE + 2] ^= 1;
    assert(rr_between_same_road(&between, &road_tracker.shifts) == 0);
    between.shifts.a[130 * SCALE + 2] ^= 1;
    between.shifts.b[140 * SCALE] += 0x10000;
    assert(rr_between_same_road(&between, &road_tracker.shifts) == 0);
    between.shifts.b[140 * SCALE] -= 0x10000;
    between.road_have[130] = 0;
    assert(rr_between_same_road(&between, &road_tracker.shifts) == 0);
    between.road_have[130] = 1;
    assert(rr_between_same_road(&between, &road_tracker.shifts) == 1);
    /* The objects. */
    other = *shown;
    assert(rr_between_same_objects(&between.set, &other) == 1);
    other.list[3] ^= 0x7F; /* a link: not compared */
    assert(rr_between_same_objects(&between.set, &other) == 1);
    other.list[7] ^= 1;
    assert(rr_between_same_objects(&between.set, &other) == 0);
    other = *shown;
    other.picture[1].x++;
    assert(rr_between_same_objects(&between.set, &other) == 0);
    other = *shown;
    other.picture[0].fx = 1;
    assert(rr_between_same_objects(&between.set, &other) == 0);
    other = *shown;
    other.fine.dx[0]++;
    assert(rr_between_same_objects(&between.set, &other) == 0);
    other = *shown;
    other.sprites++;
    assert(rr_between_same_objects(&between.set, &other) == 0);
    other = *shown;
    other.ok = 0;
    assert(rr_between_same_objects(&between.set, &other) == 0);
    /* The sprite table against video memory's chain. */
    for (int i = 0; i + 1 < shown->sprites; i++) chip.vram[0xC400 + 8 * i + 3] = (uint8_t)(i + 1);
    chip.vram[0xC400 + 8 * (shown->sprites - 1) + 3] = 0;
    assert(scene(15) == 1 && rr_between_same_table(&between, &chip) == 1);
    between.table[8 + 6] ^= 1;
    assert(rr_between_same_table(&between, &chip) == 0);
    between.table[8 + 6] ^= 1;
    chip.vram[0xC400 + 3] = 0; /* video memory's chain ends after one entry; the table's does not */
    assert(rr_between_same_table(&between, &chip) == 0);
    chip.vram[0xC400 + 3] = 1;
    between.table[3] = 0;      /* and the other way round */
    assert(rr_between_same_table(&between, &chip) == 0);
}

/* --- the margins of an in-between scene --------------------------------- */

static RrObjectSet direct_set;
static uint8_t direct_table[RR_SPRITES_MAX * 8], direct_work[0x10000];
static RrFineSprites direct_fine;

static void test_the_scene_at_an_update_has_that_updates_margin_set(void) {
    new_race(RR_BETWEEN_PREDICT);
    between.margin = 39;
    update_shows(10, 0x00100000, 335);
    update_shows(15, 0x00108000, 340); /* the tree stands on 336 to 343: off the screen, in the margin */
    assert(scene(15) == 1 && between.t == 0x10000);
    const RrObjectSet *shown = rr_object_tracker_chosen(&object_tracker);
    assert(rr_objects_margin(shown, race, race, rom, sizeof rom, 39, 152, &direct_set, direct_table, &direct_fine,
                             direct_work) == 1);
    assert(between.margin_ok == 1 && between.margin_fell_back == 0);
    assert(direct_set.margin_pictures == 1 && between.margin_set.margin_pictures == 1);
    assert(memcmp(between.margin_table, direct_table, sizeof direct_table) == 0);
    assert(memcmp(between.margin_fine.picture, direct_fine.picture, sizeof direct_fine.picture) == 0);
    assert(between.margin_fine.table == between.margin_table && between.margin_fine.pictures == between.margin_set.picture);
    const RrFinePicture *p = &between.margin_set.picture[between.margin_fine.picture[0]];
    assert(p->x == 336 && p->fx == 0 && p->fy == 0);
    /* The tree is not among the screen's own objects. */
    for (int i = 0; i < between.set.pictures; i++) assert(between.set.picture_record[i] != 0);
}

static void test_margin_objects_move_between_updates(void) {
    new_race(RR_BETWEEN_PREDICT);
    between.margin = 39;
    update_shows(10, 0x00100000, 335);
    update_shows(15, 0x00108000, 340);
    assert(scene(17) == 1 && between.margin_ok == 1);
    /* Two fifths of an update past the second: 340 + 5 * 2 / 5 = 342, less the sliver the clock drops;
     * the picture is 8 wide about that, and the part of a pixel is in fx. */
    assert(between.margin_fine.picture[0] >= 0);
    const RrFinePicture *p = &between.margin_set.picture[between.margin_fine.picture[0]];
    assert(between.margin_set.picture_record[between.margin_fine.picture[0]] == 0);
    long tree = ((long)(p->x + 4) << 16) + p->fx;
    assert(tree <= 342L << 16 && tree > (342L << 16) - 16 && p->fx != 0);
    /* Half way between the two in blend mode: 337.5. */
    new_race(RR_BETWEEN_BLEND);
    between.margin = 39;
    update_shows(10, 0x00100000, 335);
    update_shows(14, 0x00108000, 340);
    between.delay = 2 << 16;
    assert(scene(14) == 1 && between.t == 0x8000 && between.margin_ok == 1);
    p = &between.margin_set.picture[between.margin_fine.picture[0]];
    assert(p->x == 333 && p->fx == 0x8000);
}

static void test_margin_0_scene_makes_no_margin_set(void) {
    static RrStrips narrow;
    new_race(RR_BETWEEN_PREDICT);
    update_shows(10, 0x00100000, 335);
    update_shows(15, 0x00108000, 340);
    assert(between.margin == 0 && scene(15) == 1);
    assert(between.margin_ok == 0 && between.margin_set.ok == 0 && between.margin_fine.table == NULL);
    narrow = between.strips;
    /* With a margin the scene's shifts, objects and table are the same, and its strips are the
     * same within the screen. */
    static RrBetween without;
    without = between;
    new_race(RR_BETWEEN_PREDICT);
    between.margin = 39;
    update_shows(10, 0x00100000, 335);
    update_shows(15, 0x00108000, 340);
    assert(scene(15) == 1);
    assert(memcmp(&between.shifts, &without.shifts, sizeof without.shifts) == 0);
    assert(memcmp(between.table, without.table, sizeof without.table) == 0);
    assert(memcmp(between.set.picture, without.set.picture, sizeof without.set.picture) == 0);
    int strips = 0;
    for (int row = 0; row < RR_SCREEN_H * SCALE; row++) {
        int from = between.strips.from[row] < 0 ? 0 : between.strips.from[row];
        int to = between.strips.to[row] > RR_SCREEN_W * SCALE ? RR_SCREEN_W * SCALE : between.strips.to[row];
        if (to <= from) assert(narrow.to[row] <= narrow.from[row]);
        else assert(narrow.from[row] == from && narrow.to[row] == to);
        strips += narrow.to[row] > narrow.from[row];
    }
    assert(strips > 0);
}

/* The camera far to the right of the road: the road's left edge is beyond the
 * game's left limit on the near lines. */
static void test_beyond_comes_with_the_road_on_screen_rows(void) {
    new_race(RR_BETWEEN_BLEND);
    sideways = 1500;
    reference_line = 80;
    update_shows(10, 0x00100000, 100);
    reference_line = 50; /* the horizon has risen: more ground */
    update_shows(14, 0x00108000, 105);
    int screen_top = first_road_line(road_tracker.whole.have);
    between.delay = 2 << 16;
    assert(scene(14) == 1 && between.t == 0x8000);
    int own_top = first_road_line(between.road_have);
    assert(own_top >= screen_top + 5);
    /* The lines of ground above the scene's own road are the road on screen's, with its `beyond`. */
    int beyond_rows = 0;
    for (int l = screen_top; l < own_top; l++)
        for (int r = 0; r < SCALE; r++) {
            assert(between.shifts.beyond_a[l * SCALE + r] == road_tracker.shifts.beyond_a[l * SCALE + r]);
            assert(between.shifts.beyond_b[l * SCALE + r] == road_tracker.shifts.beyond_b[l * SCALE + r]);
        }
    /* The scene's own lines have the scene's road's: held at a limit means something beyond it. */
    for (int l = own_top; l < 152; l++)
        for (int r = 0; r < SCALE; r++) {
            int at = l * SCALE + r;
            int held = between.shifts.a[at] == (-0x160 - 0x2A0) * 65536;
            assert(!held || between.shifts.beyond_a[at] <= 0);
            assert(held || between.shifts.beyond_a[at] == 0);
            beyond_rows += between.shifts.beyond_a[at] < 0;
        }
    assert(beyond_rows > 0);
}

static void test_beyond_is_zero_where_the_road_was_cleared(void) {
    new_race(RR_BETWEEN_BLEND);
    sideways = 1500;
    reference_line = 50;
    update_shows(10, 0x00100000, 100);
    int older_top = first_road_line(road_tracker.whole.have);
    reference_line = 80; /* the horizon has dropped: more sky */
    update_shows(14, 0x00108000, 105);
    int screen_top = first_road_line(road_tracker.whole.have);
    assert(screen_top >= older_top + 20);
    between.delay = 2 << 16;
    assert(scene(14) == 1 && between.t == 0x8000);
    /* Every line that is not this road's: the sky, the hills where a road was, the dashboard. */
    for (int l = 0; l < RR_SCREEN_H; l++) {
        if (between.road_have[l]) continue;
        for (int r = 0; r < SCALE; r++)
            assert(between.shifts.beyond_a[l * SCALE + r] == 0 && between.shifts.beyond_b[l * SCALE + r] == 0);
    }
    assert(between.road_have[older_top + 2] == 0 && between.road_have[screen_top + 2] == 1);
}

int main(void) {
    test_the_scene_at_an_update_has_that_updates_margin_set();
    test_margin_objects_move_between_updates();
    test_margin_0_scene_makes_no_margin_set();
    test_beyond_comes_with_the_road_on_screen_rows();
    test_beyond_is_zero_where_the_road_was_cleared();
    test_the_scene_at_an_update_has_that_updates_dashes();
    test_dashes_move_between_updates();
    test_unproven_dashes_keep_the_games_sprites();
    test_a_frame_with_no_set_on_screen_forgets_the_updates();
    test_a_long_gap_starts_afresh();
    test_an_earlier_set_returning_is_not_a_new_update();
    test_a_stopped_game_holds_within_one_update_in_every_mode();
    test_the_comparisons_report_a_difference();
    test_roadside_record_is_keyed_by_segment_side_picture();
    test_vehicle_record_is_keyed_by_its_vehicle();
    test_duplicate_keys_are_not_followed();
    test_wild_ram_is_refused();
    test_objects_at_zero_are_a();
    test_objects_at_one_are_b();
    test_matched_record_is_halfway();
    test_a_quarter_of_the_way();
    test_motion_is_carried_past_b();
    test_a_shrinking_object_carried_on_keeps_a_size();
    test_unmatched_records_snap();
    test_a_leaving_object_keeps_moving();
    test_far_moved_object_snaps();
    test_pose_changes_at_the_halfway_point();
    test_a_sprite_without_a_full_picture_refuses_the_set();
    test_sprite_table_puts_the_games_other_sprites_after();
    test_sprite_table_with_nothing_after_the_objects();
    test_sprite_table_refuses_more_than_80();
    test_predict_at_the_stamp_is_the_newest_update();
    test_predict_carries_on_evenly();
    test_blend_runs_one_mean_update_behind();
    test_blend_guesses_when_an_update_runs_long();
    test_blend_delay_is_the_mean_length();
    test_late_blend_uses_the_longest_length();
    test_clock_holds_when_no_update_arrives();
    test_before_the_oldest_update_is_the_oldest();
    test_pair_never_spans_a_broken_update();
    test_note_adds_an_update_once();
    test_only_the_newest_updates_are_kept();
    test_a_road_without_inputs_makes_a_broken_update();
    test_set_mode_forgets_updates();
    test_off_makes_no_scene();
    test_frame_at_an_update_is_that_updates_scene();
    test_frame_past_an_update_moves_on();
    test_no_motion_gives_no_motion();
    test_sky_shift_goes_the_short_way_round();
    test_the_road_is_not_drawn_above_the_horizon_on_screen();
    test_ground_above_the_in_between_road_is_the_road_on_screen();
    test_delay_moves_a_quarter_frame_at_a_time();
    test_clock_never_steps_back();
    test_frame_falls_back_and_counts_why();
    puts("all between tests passed");
    return 0;
}
