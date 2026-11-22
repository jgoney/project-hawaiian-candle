/* Unit tests for the video chip model. Run with: make -C port test */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rr_genesis.h"
#include "rr_sound.h"
#include "rr_mem.h"
#include "rr_run.h"
#include "rr_vdp.h"

/* The generated code normally provides these. */
const uint32_t rr_code_rom_crc32 = 0x12345678;
RrInsnFn rr_lookup(uint32_t pc) { (void)pc; return NULL; }

static uint8_t rom[0x400];
static RrMachine machine;
static RrVdp vdp;

static RrVdp *fresh(void) {
    memset(&machine, 0, sizeof machine);
    machine.rom = rom;
    machine.rom_size = sizeof rom;
    rr_vdp_reset(&vdp);
    return &vdp;
}

static void control(RrVdp *v, uint16_t word) { rr_vdp_write_control(v, &machine, word); }

static void set_address(RrVdp *v, uint16_t first, uint16_t second) {
    control(v, first);
    control(v, second);
}

static void test_register_write(void) {
    RrVdp *v = fresh();
    control(v, 0x8F02);
    assert(v->reg[15] == 2);
    uint8_t before[24];
    memcpy(before, v->reg, sizeof before);
    control(v, 0x9F00); /* register 31 does not exist */
    assert(memcmp(before, v->reg, sizeof before) == 0);
}

static void test_vram_write_and_autoincrement(void) {
    RrVdp *v = fresh();
    control(v, 0x8F02);
    set_address(v, 0x4000, 0x0000);
    rr_vdp_write_data(v, 0x1234);
    rr_vdp_write_data(v, 0x5678);
    assert(v->vram[0] == 0x12 && v->vram[1] == 0x34 && v->vram[2] == 0x56 && v->vram[3] == 0x78);
    assert(v->addr == 4);
}

static void test_vram_write_to_odd_address_swaps_bytes(void) {
    RrVdp *v = fresh();
    control(v, 0x8F02);
    set_address(v, 0x4001, 0x0000);
    rr_vdp_write_data(v, 0x1234);
    assert(v->vram[1] == 0x12 && v->vram[0] == 0x34);
}

static void test_vram_address_uses_the_high_bits_from_the_second_word(void) {
    RrVdp *v = fresh();
    control(v, 0x8F02);
    set_address(v, 0x4000, 0x0003); /* address 0xC000 */
    rr_vdp_write_data(v, 0xABCD);
    assert(v->vram[0xC000] == 0xAB && v->vram[0xC001] == 0xCD);
}

static void test_cram_and_vsram_write(void) {
    RrVdp *v = fresh();
    control(v, 0x8F02);
    set_address(v, 0xC000, 0x0000);
    rr_vdp_write_data(v, 0x0EEE);
    assert(v->cram[0] == 0x0EEE);
    set_address(v, 0x4000, 0x0010);
    rr_vdp_write_data(v, 0x0123);
    assert(v->vsram[0] == 0x0123);
}

static void test_cram_address_wraps_and_vsram_ignores_out_of_range(void) {
    RrVdp *v = fresh();
    control(v, 0x8F02);
    set_address(v, 0xC080, 0x0000);
    rr_vdp_write_data(v, 0x0222);
    assert(v->cram[0] == 0x0222);
    uint16_t before[40];
    memcpy(before, v->vsram, sizeof before);
    set_address(v, 0x4050, 0x0010); /* index 40 */
    rr_vdp_write_data(v, 0x0111);
    assert(memcmp(before, v->vsram, sizeof before) == 0);
}

static void test_second_control_word_is_not_a_register_write(void) {
    RrVdp *v = fresh();
    control(v, 0x8F02);
    set_address(v, 0x4000, 0x8F04);
    assert(v->reg[15] == 2);
    assert(v->pending == 0);
}

static void test_status_read_clears_pending(void) {
    RrVdp *v = fresh();
    control(v, 0x8F02);
    control(v, 0x4000);
    (void)rr_vdp_read_status(v, 0, 0);
    control(v, 0x8F04);
    assert(v->reg[15] == 4);
}

static void test_status_reports_blanking(void) {
    RrVdp *v = fresh();
    assert(rr_vdp_read_status(v, 0, 0) == 0x3600);
    assert(rr_vdp_read_status(v, 1, 0) == 0x3608);
    assert(rr_vdp_read_status(v, 0, 1) == 0x3604);
}

static void test_frame_interrupt_flag_stays_until_the_interrupt_is_acknowledged(void) {
    RrVdp *v = fresh();
    rr_vdp_frame_interrupt(v);
    assert(rr_vdp_read_status(v, 1, 0) == 0x3688);
    assert(rr_vdp_read_status(v, 1, 0) == 0x3688); /* reading does not clear it */
    rr_vdp_ack_frame_interrupt(v);
    assert(rr_vdp_read_status(v, 1, 0) == 0x3608);
}

static void arm_68k_copy(RrVdp *v, uint16_t length_words) {
    control(v, 0x8114); /* mode 5, DMA enabled */
    control(v, 0x8F02);
    control(v, (uint16_t)(0x9300 | (length_words & 0xFF)));
    control(v, (uint16_t)(0x9400 | (length_words >> 8)));
    control(v, 0x9500);
    control(v, 0x9680);
    control(v, 0x977F); /* source 0xFF0000 */
}

static void test_dma_68k_copy(void) {
    RrVdp *v = fresh();
    rr_write32(&machine, 0xFF0000, 0x11223344);
    arm_68k_copy(v, 2);
    set_address(v, 0x4000, 0x0080);
    assert(v->vram[0] == 0x11 && v->vram[1] == 0x22 && v->vram[2] == 0x33 && v->vram[3] == 0x44);
    assert(v->reg[19] == 0 && v->reg[20] == 0);
    assert(v->addr == 4);
}

static void test_dma_68k_copy_to_cram(void) {
    RrVdp *v = fresh();
    rr_write32(&machine, 0xFF0000, 0x0E000002);
    arm_68k_copy(v, 2);
    set_address(v, 0xC000, 0x0080);
    assert(v->cram[0] == 0x0E00 && v->cram[1] == 0x0002);
}

static void test_dma_disabled_does_nothing(void) {
    RrVdp *v = fresh();
    rr_write32(&machine, 0xFF0000, 0x11223344);
    arm_68k_copy(v, 2);
    control(v, 0x8104); /* DMA disabled */
    set_address(v, 0x4000, 0x0080);
    assert(v->vram[0] == 0 && v->vram[1] == 0);
}

typedef struct {
    uint8_t before[64];
    RrVdp vdp;
    uint8_t after[64];
} Guarded;

static void assert_guards(const Guarded *g) {
    for (int i = 0; i < 64; i++) assert(g->before[i] == 0xA5 && g->after[i] == 0xA5);
}

static void test_dma_length_zero_and_wrap_stay_in_bounds(void) {
    Guarded *g = malloc(sizeof *g);
    assert(g != NULL);
    memset(g, 0xA5, sizeof *g);
    (void)fresh();
    RrVdp *v = &g->vdp;
    rr_vdp_reset(v);

    /* 68000 copy, length 0 = 65,536 words, source starting half way through its
     * 128 KiB block (word 0x8000 = 0xFF0000), destination starting at 0xFFFE. */
    machine.ram[0x0000] = 0x77; /* source word 0x0000, reached only by wrapping */
    machine.ram[0x0002] = 0xAB; /* source word 0x8001, the second word copied */
    machine.ram[0xFFFE] = 0xCD; /* source word 0x7FFF, the last word copied */
    arm_68k_copy(v, 0);
    set_address(v, 0x7FFE, 0x0083);
    assert_guards(g);
    assert(v->vram[0x0000] == 0xAB);  /* word 1 landed after the destination wrapped */
    assert(v->vram[0xFFFC] == 0xCD);  /* all 65,536 words were copied */
    assert(v->vram[0xFFFE] == 0x77);  /* word 0x8000 came from the wrapped source */
    assert(v->addr == 0xFFFE);        /* 65,536 steps of 2 return to the start */
    assert(v->reg[19] == 0 && v->reg[20] == 0);
    assert(v->reg[21] == 0x00 && v->reg[22] == 0x80); /* source wrapped back to 0x8000 */

    /* VRAM fill, length 0 = 65,536 bytes. */
    control(v, 0x9300);
    control(v, 0x9400);
    control(v, 0x9780);
    control(v, 0x8F01);
    set_address(v, 0x7FFE, 0x0083);
    rr_vdp_write_data(v, 0x5500);
    assert_guards(g);
    assert(v->vram[0x1234] == 0x55 && v->vram[0xFFFF] == 0x55 && v->vram[0x0000] == 0x55);

    /* VRAM copy, length 0 = 65,536 bytes. */
    control(v, 0x9300);
    control(v, 0x9400);
    control(v, 0x95FE);
    control(v, 0x96FF);
    control(v, 0x97C0);
    set_address(v, 0x7FFE, 0x00C3);
    assert_guards(g);
    free(g);
}

static void arm_vram_dma(RrVdp *v, uint16_t length, uint16_t source, int mode) {
    control(v, 0x8114);
    control(v, 0x8F01);
    control(v, (uint16_t)(0x9300 | (length & 0xFF)));
    control(v, (uint16_t)(0x9400 | (length >> 8)));
    control(v, (uint16_t)(0x9500 | (source & 0xFF)));
    control(v, (uint16_t)(0x9600 | (source >> 8)));
    control(v, (uint16_t)(0x9700 | mode));
}

static void test_vram_fill_pattern(void) {
    RrVdp *v = fresh();
    arm_vram_dma(v, 4, 0, 0x80);
    set_address(v, 0x4010, 0x0080);
    rr_vdp_write_data(v, 0xAB12);
    /* The data word is written first (0xAB at 0x10, 0x12 at 0x11), then the
     * high byte is written four times at the address with bit 0 flipped. */
    static const uint8_t want[6] = {0xAB, 0x12, 0xAB, 0xAB, 0x00, 0xAB};
    assert(memcmp(&v->vram[0x10], want, sizeof want) == 0);
    assert(v->addr == 0x15);
}

static void test_vram_copy(void) {
    RrVdp *v = fresh();
    static const uint8_t source[4] = {1, 2, 3, 4};
    memcpy(&v->vram[0x10], source, sizeof source);
    arm_vram_dma(v, 4, 0x0010, 0xC0);
    set_address(v, 0x0020, 0x00C0);
    assert(memcmp(&v->vram[0x20], source, sizeof source) == 0);
    assert(v->vram[0x24] == 0 && v->addr == 0x24);
    assert(v->reg[21] == 0x14 && v->reg[22] == 0x00); /* source register advanced */
}

static void test_vdp_save_load_round_trip(void) {
    RrVdp *v = fresh();
    control(v, 0x8F02);
    set_address(v, 0x4000, 0x0000);
    rr_vdp_write_data(v, 0x1234);
    set_address(v, 0xC000, 0x0000);
    rr_vdp_write_data(v, 0x0EEE);
    control(v, 0x4000); /* leave a command half written */
    FILE *f = tmpfile();
    assert(f != NULL && rr_vdp_save(v, f) == 0);
    static RrVdp copy;
    memset(&copy, 0xEE, sizeof copy);
    rewind(f);
    assert(rr_vdp_load(&copy, f) == 0);
    fclose(f);
    assert(memcmp(copy.vram, v->vram, sizeof v->vram) == 0);
    assert(memcmp(copy.reg, v->reg, sizeof v->reg) == 0);
    assert(copy.cram[0] == 0x0EEE && copy.addr == v->addr && copy.code == v->code);
    assert(copy.pending == 1 && copy.first_word == 0x4000);
}

/* ---- renderer ---- */

#define PLANE_A 0xC000
#define PLANE_B 0xE000
#define SPRITES 0xD800
#define HSCROLL 0xDC00

static RrVdp *screen(void) {
    RrVdp *v = fresh();
    v->reg[1] = 0x44;  /* display on, mode 5 */
    v->reg[2] = 0x30;  /* plane A at 0xC000 */
    v->reg[3] = 0x3C;  /* window at 0xF000 */
    v->reg[4] = 0x07;  /* plane B at 0xE000 */
    v->reg[5] = 0x6C;  /* sprite table at 0xD800 */
    v->reg[12] = 0x81; /* 40 cells */
    v->reg[13] = 0x37; /* horizontal scroll table at 0xDC00 */
    v->reg[16] = 0x01; /* planes are 64 x 32 cells */
    return v;
}

static void tile_pixel(RrVdp *v, int tile, int x, int y, int colour) {
    uint8_t *byte = &v->vram[tile * 32 + y * 4 + x / 2];
    *byte = (x & 1) ? (uint8_t)((*byte & 0xF0) | colour) : (uint8_t)((*byte & 0x0F) | colour << 4);
}

static void solid_tile(RrVdp *v, int tile, int colour) {
    memset(&v->vram[tile * 32], colour << 4 | colour, 32);
}

static void name(RrVdp *v, int plane, int cell_x, int cell_y, uint16_t entry) {
    int offset = plane + (cell_y * 64 + cell_x) * 2;
    v->vram[offset] = (uint8_t)(entry >> 8);
    v->vram[offset + 1] = (uint8_t)entry;
}

static void sprite(RrVdp *v, int index, int x, int y, int size, int link, uint16_t attr) {
    uint8_t *s = &v->vram[SPRITES + index * 8];
    s[0] = (uint8_t)(y >> 8); s[1] = (uint8_t)y;
    s[2] = (uint8_t)size;     s[3] = (uint8_t)link;
    s[4] = (uint8_t)(attr >> 8); s[5] = (uint8_t)attr;
    s[6] = (uint8_t)(x >> 8); s[7] = (uint8_t)x;
}

static uint32_t row[RR_SCREEN_W];

static void test_backdrop_fills_a_blank_display(void) {
    RrVdp *v = screen();
    v->reg[7] = 0x05;
    v->cram[5] = 0x000E;
    rr_vdp_render_line(v, 0, row);
    for (int x = 0; x < RR_SCREEN_W; x++) assert(row[x] == rr_vdp_colour(0x000E));
}

static void test_display_disabled_shows_the_backdrop(void) {
    RrVdp *v = screen();
    v->reg[1] = 0x04;
    v->cram[0] = 0x0E00;
    v->cram[1] = 0x00E0;
    solid_tile(v, 1, 1);
    name(v, PLANE_A, 0, 0, 0x0001);
    rr_vdp_render_line(v, 0, row);
    assert(row[0] == rr_vdp_colour(0x0E00));
}

static void test_colour_channels(void) {
    assert(rr_vdp_colour(0x0000) == 0x000000);
    assert(rr_vdp_colour(0x000E) == 0xFF0000);
    assert(rr_vdp_colour(0x00E0) == 0x00FF00);
    assert(rr_vdp_colour(0x0E00) == 0x0000FF);
    /* The eight levels of MAME's 315-5313, as seen in its boot snapshots. */
    static const uint32_t levels[8] = {0, 52, 87, 116, 144, 172, 206, 255};
    for (unsigned i = 0; i < 8; i++) assert(rr_vdp_colour((uint16_t)(i << 1)) == levels[i] << 16);
}

static void test_plane_a_tile_pixel(void) {
    RrVdp *v = screen();
    v->cram[1] = 0x00E0;
    tile_pixel(v, 1, 0, 0, 1);
    name(v, PLANE_A, 0, 0, 0x0001);
    rr_vdp_render_line(v, 0, row);
    assert(row[0] == rr_vdp_colour(0x00E0));
    assert(row[1] == rr_vdp_colour(0) && row[8] == rr_vdp_colour(0));
}

static void test_tile_palette_line(void) {
    RrVdp *v = screen();
    v->cram[32 + 1] = 0x000E; /* palette line 2, colour 1 */
    tile_pixel(v, 1, 0, 0, 1);
    name(v, PLANE_A, 0, 0, 0x4001);
    rr_vdp_render_line(v, 0, row);
    assert(row[0] == rr_vdp_colour(0x000E));
}

static void test_tile_flips(void) {
    RrVdp *v = screen();
    v->cram[1] = 0x00E0;
    tile_pixel(v, 1, 0, 0, 1);
    name(v, PLANE_A, 0, 0, 0x0801); /* horizontal flip */
    name(v, PLANE_A, 2, 0, 0x1001); /* vertical flip */
    rr_vdp_render_line(v, 0, row);
    assert(row[7] == rr_vdp_colour(0x00E0) && row[0] == rr_vdp_colour(0));
    assert(row[16] == rr_vdp_colour(0));
    rr_vdp_render_line(v, 7, row);
    assert(row[16] == rr_vdp_colour(0x00E0));
}

static void test_horizontal_scroll_full_screen(void) {
    RrVdp *v = screen();
    v->cram[1] = 0x00E0;
    tile_pixel(v, 1, 0, 0, 1);
    name(v, PLANE_A, 0, 0, 0x0001);
    v->vram[HSCROLL + 1] = 4; /* plane A moves 4 pixels right */
    rr_vdp_render_line(v, 0, row);
    assert(row[4] == rr_vdp_colour(0x00E0) && row[0] == rr_vdp_colour(0));
}

static void test_vertical_scroll(void) {
    RrVdp *v = screen();
    v->cram[1] = 0x00E0;
    tile_pixel(v, 1, 0, 0, 1);
    name(v, PLANE_A, 0, 1, 0x0001); /* cell row 1 = plane line 8 */
    v->vsram[0] = 8;
    rr_vdp_render_line(v, 0, row);
    assert(row[0] == rr_vdp_colour(0x00E0));
}

static void test_high_priority_plane_b_over_low_priority_plane_a(void) {
    RrVdp *v = screen();
    v->cram[1] = 0x00E0;
    v->cram[2] = 0x000E;
    solid_tile(v, 1, 1);
    solid_tile(v, 2, 2);
    name(v, PLANE_A, 0, 0, 0x0001);
    name(v, PLANE_B, 0, 0, 0x8002);
    name(v, PLANE_A, 1, 0, 0x0001);
    name(v, PLANE_B, 1, 0, 0x0002);
    rr_vdp_render_line(v, 0, row);
    assert(row[0] == rr_vdp_colour(0x000E)); /* B has priority */
    assert(row[8] == rr_vdp_colour(0x00E0)); /* equal priority: A is in front */
}

static void test_sprite_position_and_size(void) {
    RrVdp *v = screen();
    v->cram[3] = 0x0E00;
    for (int tile = 3; tile < 7; tile++) solid_tile(v, tile, 3);
    sprite(v, 0, 144, 136, 0x05, 0, 0x0003); /* 2 x 2 cells at screen (16, 8) */
    uint32_t on = rr_vdp_colour(0x0E00), off = rr_vdp_colour(0);
    rr_vdp_render_line(v, 7, row);
    assert(row[16] == off);
    rr_vdp_render_line(v, 8, row);
    assert(row[15] == off && row[16] == on && row[31] == on && row[32] == off);
    rr_vdp_render_line(v, 23, row);
    assert(row[16] == on);
    rr_vdp_render_line(v, 24, row);
    assert(row[16] == off);
}

static void test_sprite_tiles_run_down_columns(void) {
    RrVdp *v = screen();
    v->cram[1] = 0x00E0;
    v->cram[2] = 0x000E;
    solid_tile(v, 3, 1); /* first tile: top-left */
    solid_tile(v, 4, 2); /* second tile: below it, not beside it */
    sprite(v, 0, 128, 128, 0x05, 0, 0x0003);
    rr_vdp_render_line(v, 0, row);
    assert(row[0] == rr_vdp_colour(0x00E0) && row[8] == rr_vdp_colour(0));
    rr_vdp_render_line(v, 8, row);
    assert(row[0] == rr_vdp_colour(0x000E));
}

static void test_first_sprite_in_the_list_is_in_front(void) {
    RrVdp *v = screen();
    v->cram[1] = 0x00E0;
    v->cram[2] = 0x000E;
    solid_tile(v, 1, 1);
    solid_tile(v, 2, 2);
    sprite(v, 0, 128, 128, 0x00, 1, 0x0001);
    sprite(v, 1, 128, 128, 0x00, 0, 0x0002);
    rr_vdp_render_line(v, 0, row);
    assert(row[0] == rr_vdp_colour(0x00E0));
}

static void test_sprite_link_cycle_terminates(void) {
    RrVdp *v = screen();
    v->cram[1] = 0x00E0;
    solid_tile(v, 1, 1);
    /* Neither sprite is on the rendered line, so only the 80-sprite limit can
     * end the walk round this two-sprite cycle. */
    sprite(v, 0, 128, 328, 0x00, 1, 0x0001);
    sprite(v, 1, 160, 328, 0x00, 0, 0x0001);
    rr_vdp_render_line(v, 0, row);
    assert(row[0] == rr_vdp_colour(0) && row[32] == rr_vdp_colour(0));
    rr_vdp_render_line(v, 200, row);
    assert(row[0] == rr_vdp_colour(0x00E0) && row[32] == rr_vdp_colour(0x00E0));
}

static void test_unsupported_video_modes_render_magenta(void) {
    RrVdp *v = screen();
    v->reg[12] = 0x00; /* 32-cell mode */
    rr_vdp_render_line(v, 0, row);
    assert(row[0] == 0xFF00FF);
    v->reg[12] = 0x81;
    v->reg[1] = 0x4C; /* 30-row mode */
    rr_vdp_render_line(v, 0, row);
    assert(row[0] == 0xFF00FF);
    v->reg[1] = 0x04; /* display off: the mode does not matter, the backdrop shows */
    v->reg[12] = 0x00;
    rr_vdp_render_line(v, 0, row);
    assert(row[0] == rr_vdp_colour(0));
}

static void test_column_vertical_scroll_follows_the_horizontal_scroll(void) {
    RrVdp *v = screen();
    v->cram[1] = 0x00E0;
    v->cram[2] = 0x000E;
    solid_tile(v, 1, 1);
    solid_tile(v, 2, 2);
    for (int cell = 0; cell < 64; cell++) {
        name(v, PLANE_A, cell, 0, 0x0001); /* plane lines 0-7 */
        name(v, PLANE_A, cell, 2, 0x0002); /* plane lines 16-23 */
    }
    v->reg[11] = 0x04;        /* vertical scroll per two-cell column */
    v->vram[HSCROLL + 1] = 8; /* plane A shifted right by 8 */
    v->vsram[0] = 0;          /* column 0 */
    v->vsram[2] = 16;         /* column 1 */
    rr_vdp_render_line(v, 0, row);
    /* Columns sit on the scrolled grid: column 0 covers screen x 8-23. */
    assert(row[8] == rr_vdp_colour(0x00E0) && row[23] == rr_vdp_colour(0x00E0));
    assert(row[24] == rr_vdp_colour(0x000E) && row[39] == rr_vdp_colour(0x000E));
}

static void test_sprite_vertical_position_is_nine_bits(void) {
    RrVdp *v = screen();
    v->cram[1] = 0x00E0;
    solid_tile(v, 1, 1);
    sprite(v, 0, 128, 0x280, 0x00, 0, 0x0001); /* 0x280 & 0x1FF = 128: screen line 0 */
    rr_vdp_render_line(v, 0, row);
    assert(row[0] == rr_vdp_colour(0x00E0));
}

static void test_window_replaces_plane_a(void) {
    RrVdp *v = screen();
    v->cram[1] = 0x00E0;
    v->cram[2] = 0x000E;
    solid_tile(v, 1, 1);
    solid_tile(v, 2, 2);
    name(v, PLANE_A, 0, 0, 0x0001);
    name(v, 0xF000, 0, 0, 0x0002);
    v->reg[18] = 0x01; /* window covers the top cell row */
    rr_vdp_render_line(v, 0, row);
    assert(row[0] == rr_vdp_colour(0x000E));
    rr_vdp_render_line(v, 8, row);
    assert(row[0] == rr_vdp_colour(0));
}

/* Which layer each pixel comes from. One frame holds every case that has to
 * be right: plane B behind a partly transparent plane A, a window strip, a
 * sprite half off the left edge, two overlapping sprites, and a sprite with a
 * transparent pixel over a plane. */
static RrVdp *layered_frame(void) {
    RrVdp *v = screen();
    for (int i = 1; i < 16; i++) v->cram[i] = (uint16_t)(i * 0x0222 & 0x0EEE);
    v->cram[0] = 0x0E0E;
    v->reg[7] = 0x00;
    solid_tile(v, 1, 1);                 /* plane B's tile */
    solid_tile(v, 2, 2);                 /* plane A's tile */
    solid_tile(v, 3, 3);                 /* window */
    solid_tile(v, 4, 4);                 /* sprite */
    solid_tile(v, 5, 5);                 /* second sprite */
    solid_tile(v, 6, 6);                 /* sprite with a hole */
    tile_pixel(v, 6, 0, 0, 0);
    for (int y = 0; y < 28; y++)
        for (int x = 0; x < 40; x++) {
            if (y < 20) name(v, PLANE_B, x, y, 0x0001); /* rows 20+ show the backdrop */
            if (x >= 20) name(v, PLANE_A, x, y, 0x0002);
            name(v, 0xF000, x, y, 0x0003);
        }
    v->reg[18] = 0x01;                              /* window on the top cell row */
    sprite(v, 0, 128 - 4, 128 + 40, 0x00, 1, 0x0004);  /* half off the left edge */
    sprite(v, 1, 128 + 100, 128 + 40, 0x00, 2, 0x0004);
    sprite(v, 2, 128 + 104, 128 + 40, 0x00, 3, 0x0005); /* behind sprite 1 where they overlap */
    sprite(v, 3, 128 + 200, 128 + 40, 0x00, 0, 0x0006); /* hole at its top left */
    return v;
}

static void test_source_colour_equals_the_picture(void) {
    RrVdp *v = layered_frame();
    static RrPixelSource src[RR_SCREEN_W];
    for (int line = 0; line < RR_SCREEN_H; line++) {
        rr_vdp_render_line(v, line, row);
        rr_vdp_render_line_sources(v, line, src);
        for (int x = 0; x < RR_SCREEN_W; x++) assert(rr_vdp_colour(v->cram[src[x].colour]) == row[x]);
        if (line == 0) assert(src[10].layer == RR_SRC_WINDOW && src[10].detail == 0x0003);
        if (line == 100) {
            assert(src[10].layer == RR_SRC_PLANE_B && src[10].detail == 0x0001);
            assert(src[200].layer == RR_SRC_PLANE_A && src[200].detail == 0x0002);
        }
        if (line == 200) assert(src[10].layer == RR_SRC_BACKDROP && src[10].colour == 0);
        if (line == 40) {
            assert(src[0].layer == RR_SRC_SPRITE && src[0].detail == 0 && src[3].layer == RR_SRC_SPRITE);
            assert(src[4].layer == RR_SRC_PLANE_B);
            assert(src[105].layer == RR_SRC_SPRITE && src[105].detail == 1); /* the overlap */
            assert(src[109].layer == RR_SRC_SPRITE && src[109].detail == 2);
            assert(src[200].layer == RR_SRC_PLANE_A);                        /* through the hole */
            assert(src[201].layer == RR_SRC_SPRITE && src[201].detail == 3);
        }
    }
}

/* The layered frame with a different sideways shift of each plane on every line. */
static RrVdp *shifted_frame(void) {
    RrVdp *v = layered_frame();
    v->reg[11] = 0x03; /* a horizontal scroll value per line */
    for (int line = 0; line < RR_SCREEN_H; line++) {
        unsigned a = (unsigned)(line * 3) & 0x3FF, b = (unsigned)(0x400 - line) & 0x3FF;
        v->vram[HSCROLL + line * 4] = (uint8_t)(a >> 8);
        v->vram[HSCROLL + line * 4 + 1] = (uint8_t)a;
        v->vram[HSCROLL + line * 4 + 2] = (uint8_t)(b >> 8);
        v->vram[HSCROLL + line * 4 + 3] = (uint8_t)b;
    }
    return v;
}

static uint32_t big[RR_SCALE_MAX * RR_SCALE_MAX * RR_SCREEN_W];
#define NARROW_PICTURE_HASH 0x182bb925c986d805ull

static void test_scale_1_without_shifts_is_the_original_line(void) {
    RrVdp *v = shifted_frame();
    for (int line = 0; line < RR_SCREEN_H; line++) {
        rr_vdp_render_line(v, line, row);
        rr_vdp_render_line_scaled(v, line, 1, NULL, NULL, big);
        assert(memcmp(row, big, sizeof row) == 0);
    }
}

static void test_scaled_without_shifts_repeats_each_pixel(void) {
    RrVdp *v = shifted_frame();
    for (int line = 0; line < RR_SCREEN_H; line += 7) {
        rr_vdp_render_line(v, line, row);
        rr_vdp_render_line_scaled(v, line, 3, NULL, NULL, big);
        for (int r = 0; r < 3; r++)
            for (int x = 0; x < RR_SCREEN_W * 3; x++) assert(big[r * RR_SCREEN_W * 3 + x] == row[x / 3]);
    }
}

static void test_integer_shifts_sample_back_to_the_original(void) {
    RrVdp *v = shifted_frame();
    RrVdpLineState state;
    for (int line = 0; line < RR_SCREEN_H; line++) {
        int32_t a[4], b[4];
        rr_vdp_line_state(v, line, &state);
        for (int r = 0; r < 4; r++) {
            a[r] = (int32_t)state.hscroll[0] << 16;
            b[r] = (int32_t)state.hscroll[1] << 16;
        }
        rr_vdp_render_line(v, line, row);
        rr_vdp_render_line_scaled(v, line, 4, a, b, big);
        for (int x = 0; x < RR_SCREEN_W; x++) assert(big[x * 4] == row[x]);
    }
}

/* Output column of the first pixel of plane A's tile in a row of the scaled line. */
static int edge_in_row(const uint32_t *line4, int r, uint32_t colour) {
    for (int x = 0; x < RR_SCREEN_W * 4; x++)
        if (line4[r * RR_SCREEN_W * 4 + x] == colour) return x;
    return -1;
}

static void test_fractional_shift_moves_an_edge_one_output_pixel_per_row(void) {
    RrVdp *v = screen();
    v->cram[1] = 0x00E0; v->cram[2] = 0x000E; v->cram[3] = 0x0E00; v->cram[4] = 0x0EEE;
    solid_tile(v, 1, 1); solid_tile(v, 2, 2); solid_tile(v, 3, 3); solid_tile(v, 4, 4);
    for (int x = 10; x < 20; x++) name(v, PLANE_A, x, 2, 0x8001); /* plane A: an edge at x = 80 */
    for (int x = 30; x < 40; x++) name(v, PLANE_B, x, 2, 0x8002); /* plane B: an edge at x = 240 */
    sprite(v, 0, 128 + 20, 128 + 16, 0x00, 0, 0x8004);
    int32_t a[4], b[4];
    for (int r = 0; r < 4; r++) {
        a[r] = (5 << 16) + r * 0x4000; /* 5, 5 1/4, 5 1/2, 5 3/4 */
        b[r] = -(3 << 16);             /* whole, and negative */
    }
    rr_vdp_render_line_scaled(v, 16, 4, a, b, big);
    for (int r = 0; r < 4; r++) {
        assert(edge_in_row(big, r, rr_vdp_colour(0x00E0)) == 85 * 4 + r);
        assert(edge_in_row(big, r, rr_vdp_colour(0x0EEE)) == 20 * 4); /* the sprite stays put */
    }
    /* Plane B was moved 3 whole pixels to the left, the same on every row. */
    for (int r = 0; r < 4; r++) assert(edge_in_row(big, r, rr_vdp_colour(0x000E)) == 237 * 4);
    name(v, 0xF000, 0, 2, 0x8003);
    v->reg[18] = 0x03; /* window on the top three cell rows: it replaces plane A */
    rr_vdp_render_line_scaled(v, 16, 4, a, b, big);
    for (int r = 0; r < 4; r++) {
        assert(edge_in_row(big, r, rr_vdp_colour(0x0E00)) == 0); /* the window does not move */
        assert(edge_in_row(big, r, rr_vdp_colour(0x00E0)) == -1);
    }
}

static void test_clamped_and_missing_lines_sample_back_to_the_original(void) {
    RrVdp *v = shifted_frame();
    RrVdpLineState state;
    /* Plane A shifted and plane B left alone on the same line; then a shift written the
     * way the game does, as a negative number whose low 10 bits are the table's value. */
    for (int line = 0; line < RR_SCREEN_H; line += 5) {
        int32_t a[4], wrapped[4];
        rr_vdp_line_state(v, line, &state);
        for (int r = 0; r < 4; r++) {
            a[r] = (int32_t)state.hscroll[0] << 16;
            wrapped[r] = ((int32_t)state.hscroll[1] - 0x400) * 65536;
        }
        rr_vdp_render_line(v, line, row);
        rr_vdp_render_line_scaled(v, line, 4, a, NULL, big);
        for (int x = 0; x < RR_SCREEN_W; x++) assert(big[x * 4] == row[x]);
        rr_vdp_render_line_scaled(v, line, 4, NULL, wrapped, big);
        for (int x = 0; x < RR_SCREEN_W; x++) assert(big[x * 4] == row[x]);
    }
}

/* A sprite shown from a finer picture. The picture is 4 x 4 with a different
 * colour in every pixel but one, which is see-through. */
static const uint8_t fine_rows[4 * 3] = {0x12, 0x34, 0, 0x56, 0x78, 0, 0x9A, 0xBC, 0, 0xDE, 0xF0, 0};
static RrFinePicture fine_picture;
static RrFineSprites fine;

static int fine_pixel(int sx, int sy) {
    uint8_t byte = fine_rows[sy * 3 + sx / 2];
    return (sx & 1) ? byte & 0x0F : byte >> 4;
}

/* One 2 x 2-cell sprite at screen (16, 8) whose tiles are solid colour 5, and
 * the picture at `size` standing at its top-left corner. */
static RrVdp *fine_scene(uint32_t size) {
    RrVdp *v = screen();
    /* Sixteen different colours: red from the low two bits, green from the high two. */
    for (int i = 1; i < 16; i++) v->cram[i] = (uint16_t)(((i & 3) * 2) << 1 | (((i >> 2) & 3) * 2) << 5 | 0x200);
    for (int i = 1; i < 16; i++) v->cram[16 + i] = (uint16_t)(v->cram[i] | 0xC00);
    for (int tile = 3; tile < 7; tile++) solid_tile(v, tile, 5);
    sprite(v, 0, 144, 136, 0x05, 0, 0x0003);
    fine_picture = (RrFinePicture){.rows = fine_rows, .width = 4, .height = 4, .stride = 3, .size = size,
                                   .x = 16, .y = 8, .columns = (uint16_t)((4 * (uint64_t)size + 0xFFFF) >> 16),
                                   .rows_shown = (uint16_t)((4 * (uint64_t)size) >> 16)};
    fine.pictures = &fine_picture;
    fine.table = NULL;
    fine.strips = NULL;
    fine.soft = 0;
    memset(fine.skip, 0, sizeof fine.skip);
    for (int i = 0; i < RR_SPRITES_MAX; i++) {
        fine.picture[i] = -1;
        fine.dx[i] = 0;
    }
    fine.picture[0] = 0;
    return v;
}

/* The colour at output column c, output row q of the frame enlarged `scale` times. */
static uint32_t fine_at(const RrVdp *v, int scale, int c, int q) {
    rr_vdp_render_line_fine(v, q / scale, scale, NULL, NULL, &fine, big);
    return big[(q % scale) * RR_SCREEN_W * scale + c];
}

static uint32_t picture_colour(const RrVdp *v, int sx, int sy) {
    return rr_vdp_colour(v->cram[fine_pixel(sx, sy)]);
}

static void test_fine_null_is_render_line_scaled(void) {
    static uint32_t other[RR_SCALE_MAX * RR_SCALE_MAX * RR_SCREEN_W];
    static int32_t a[RR_SCALE_MAX], b[RR_SCALE_MAX];
    RrVdp *v = shifted_frame();
    for (int line = 0; line < RR_SCREEN_H; line += 5) {
        for (int r = 0; r < 3; r++) {
            a[r] = (int32_t)((line * 3) << 16) + r * 0x5555;
            b[r] = (int32_t)(((0x400 - line) & 0x3FF) << 16) + r * 0x3333;
        }
        rr_vdp_render_line_scaled(v, line, 3, NULL, NULL, other);
        rr_vdp_render_line_fine(v, line, 3, NULL, NULL, NULL, big);
        assert(memcmp(big, other, 9 * RR_SCREEN_W * sizeof *big) == 0);
        rr_vdp_render_line_scaled(v, line, 3, a, b, other);
        rr_vdp_render_line_fine(v, line, 3, a, b, NULL, big);
        assert(memcmp(big, other, 9 * RR_SCREEN_W * sizeof *big) == 0);
    }
}

static void test_fine_picture_at_natural_size_repeats_each_pixel(void) {
    RrVdp *v = fine_scene(0x10000);
    uint32_t backdrop = rr_vdp_colour(v->cram[0]);
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++)
            for (int r = 0; r < 3; r++)
                for (int k = 0; k < 3; k++)
                    assert(fine_at(v, 3, 48 + 3 * sx + k, 24 + 3 * sy + r) == picture_colour(v, sx, sy));
    /* Beyond the picture the sprite is see-through, though its tiles are solid. */
    assert(fine_at(v, 3, 47, 24) == backdrop && fine_at(v, 3, 60, 24) == backdrop);
    assert(fine_at(v, 3, 48, 23) == backdrop && fine_at(v, 3, 48, 36) == backdrop);
}

static void test_fine_picture_at_half_size_shows_every_source_pixel(void) {
    RrVdp *v = fine_scene(0x8000); /* 2 x 2 screen pixels: at scale 2, 4 x 4 output pixels */
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) assert(fine_at(v, 2, 32 + sx, 16 + sy) == picture_colour(v, sx, sy));
    assert(fine_at(v, 2, 36, 16) == rr_vdp_colour(v->cram[0]));
}

static void test_fine_picture_at_double_size_gives_each_pixel_equal_room(void) {
    RrVdp *v = fine_scene(0x20000);
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++)
            for (int k = 0; k < 6; k++) {
                assert(fine_at(v, 3, 48 + 6 * sx + k, 24 + 6 * sy) == picture_colour(v, sx, sy));
                assert(fine_at(v, 3, 48 + 6 * sx, 24 + 6 * sy + k) == picture_colour(v, sx, sy));
            }
}

static void test_fine_picture_at_an_odd_size_keeps_pixels_within_one_output_pixel_of_even(void) {
    RrVdp *v = fine_scene(0x18000); /* 1.5 times: 4.5 output pixels each at scale 3 */
    int starts[5] = {0, 0, 0, 0, 18};
    for (int sx = 1; sx < 4; sx++) {
        int c = 48;
        while (fine_at(v, 3, c, 24) != picture_colour(v, sx, 0)) c++;
        starts[sx] = c - 48;
    }
    for (int sx = 0; sx < 4; sx++) assert(starts[sx + 1] - starts[sx] == 4 || starts[sx + 1] - starts[sx] == 5);
}

static void test_fine_picture_mirrored_runs_right_to_left(void) {
    RrVdp *v = fine_scene(0x10000);
    sprite(v, 0, 144, 136, 0x05, 0, 0x0803);
    fine_picture.mirrored = 1;
    fine_picture.x = 20; /* one past its right-hand end */
    for (int sx = 0; sx < 4; sx++)
        for (int k = 0; k < 3; k++) assert(fine_at(v, 3, 48 + 3 * (3 - sx) + k, 27) == picture_colour(v, sx, 1));
    assert(fine_at(v, 3, 60, 27) == rr_vdp_colour(v->cram[0]));
}

static void test_fine_first_column_and_first_row_skip_into_the_picture(void) {
    RrVdp *v = fine_scene(0x10000);
    fine_picture.first_column = 2;
    fine_picture.first_row = 1;
    fine_picture.columns = 2;
    fine_picture.rows_shown = 3;
    assert(fine_at(v, 3, 48, 24) == picture_colour(v, 2, 1));
    assert(fine_at(v, 3, 53, 24) == picture_colour(v, 3, 1));
    assert(fine_at(v, 3, 48, 32) == picture_colour(v, 2, 3));
    assert(fine_at(v, 3, 54, 24) == rr_vdp_colour(v->cram[0]));
    assert(fine_at(v, 3, 48, 33) == rr_vdp_colour(v->cram[0]));
}

static void test_fine_columns_and_rows_shown_end_the_picture(void) {
    RrVdp *v = fine_scene(0x10000);
    fine_picture.columns = 2;
    fine_picture.rows_shown = 3;
    assert(fine_at(v, 3, 53, 24) == picture_colour(v, 1, 0));
    assert(fine_at(v, 3, 54, 24) == rr_vdp_colour(v->cram[0])); /* picture pixel (2, 0) is cut off */
    assert(fine_at(v, 3, 48, 32) == picture_colour(v, 0, 2));
    assert(fine_at(v, 3, 48, 33) == rr_vdp_colour(v->cram[0])); /* and row 3 */
}

static void test_fine_dx_moves_a_copy(void) {
    RrVdp *v = fine_scene(0x10000);
    fine.dx[0] = 2;
    assert(fine_at(v, 3, 48, 24) == rr_vdp_colour(v->cram[0]));
    assert(fine_at(v, 3, 54, 24) == picture_colour(v, 0, 0));
    assert(fine_at(v, 3, 65, 24) == picture_colour(v, 3, 0));
}


/* The enlarged frame's output rows q0..q1-1, columns 0..wide-1, into `to`. */
static uint32_t fraction_a[24 * RR_SCREEN_W * 4], fraction_b[24 * RR_SCREEN_W * 4];
static void fine_rows_into(const RrVdp *v, int scale, int q0, int q1, uint32_t *to) {
    int wide = RR_SCREEN_W * scale;
    for (int q = q0; q < q1; q++) {
        rr_vdp_render_line_fine(v, q / scale, scale, NULL, NULL, &fine, big);
        memcpy(to + (size_t)(q - q0) * (size_t)wide, big + (size_t)(q % scale) * (size_t)wide, (size_t)wide * sizeof *to);
    }
}

static void test_fraction_moves_a_fine_picture_by_output_pixels(void) {
    RrVdp *v = fine_scene(0x10000);
    int wide = RR_SCREEN_W * 4, seen = 0;
    fine_rows_into(v, 4, 28, 52, fraction_a);
    fine_picture.fx = 0x8000; /* half a screen pixel: two output columns at scale 4 */
    fine_rows_into(v, 4, 28, 52, fraction_b);
    for (int q = 0; q < 24; q++)
        for (int c = 2; c < wide; c++) {
            assert(fraction_b[q * wide + c] == fraction_a[q * wide + c - 2]);
            seen += fraction_a[q * wide + c - 2] != rr_vdp_colour(v->cram[0]);
        }
    assert(seen > 200); /* the picture really is in the rows compared */
    fine_picture.fx = 0;
    fine_picture.fy = 0x4000; /* a quarter: one output row */
    fine_rows_into(v, 4, 29, 53, fraction_b);
    assert(memcmp(fraction_a, fraction_b, sizeof fraction_a) == 0);
}

static void test_fraction_moves_a_mirrored_picture_right(void) {
    RrVdp *v = fine_scene(0x10000);
    int wide = RR_SCREEN_W * 4;
    fine_picture.mirrored = 1;
    fine_picture.x = 20;
    fine_rows_into(v, 4, 32, 48, fraction_a);
    assert(fraction_a[64] == picture_colour(v, 3, 0) && fraction_a[79] == picture_colour(v, 0, 0));
    fine_picture.fx = 0xC000;
    fine_rows_into(v, 4, 32, 48, fraction_b);
    for (int q = 0; q < 16; q++)
        for (int c = 3; c < wide; c++) assert(fraction_b[q * wide + c] == fraction_a[q * wide + c - 3]);
}

/* Without a fraction an entry covers exactly its own rectangle, as before. */
static void test_zero_fraction_changes_nothing(void) {
    RrVdp *v = fine_scene(0x50000); /* 20 pixels wide and high in a 16 x 16 entry */
    uint32_t backdrop = rr_vdp_colour(v->cram[0]);
    fine_picture.columns = 20;
    fine_picture.rows_shown = 20;
    assert(fine_at(v, 4, 127, 32) != backdrop);
    assert(fine_at(v, 4, 128, 32) == backdrop);  /* screen column 32: past the entry */
    assert(fine_at(v, 4, 64, 95) != backdrop);
    assert(fine_at(v, 4, 64, 96) == backdrop);   /* screen line 24: past the entry */
}

static void test_fraction_is_not_cut_at_the_entry_edge(void) {
    RrVdp *v = fine_scene(0x40000); /* exactly the entry's 16 x 16 pixels */
    uint32_t backdrop = rr_vdp_colour(v->cram[0]);
    assert(fine_at(v, 4, 127, 32) == picture_colour(v, 3, 0) && fine_at(v, 4, 128, 32) == backdrop);
    fine_picture.fx = 0x8000;
    assert(fine_at(v, 4, 64, 32) == backdrop && fine_at(v, 4, 65, 32) == backdrop);
    assert(fine_at(v, 4, 66, 32) == picture_colour(v, 0, 0));
    assert(fine_at(v, 4, 128, 32) == picture_colour(v, 3, 0) && fine_at(v, 4, 129, 32) == picture_colour(v, 3, 0));
    assert(fine_at(v, 4, 130, 32) == backdrop);
    fine_picture.fx = 0;
    fine_picture.fy = 0x8000;
    assert(fine_at(v, 4, 64, 33) == backdrop && fine_at(v, 4, 64, 34) == picture_colour(v, 0, 0));
    assert(fine_at(v, 4, 64, 97) == picture_colour(v, 0, 3) && fine_at(v, 4, 64, 98) == backdrop);
}

static void put_entry(uint8_t *table, int index, int x, int y, int size, int link, uint16_t attr) {
    uint8_t *s = table + index * 8;
    s[0] = (uint8_t)(y >> 8); s[1] = (uint8_t)y;
    s[2] = (uint8_t)size;     s[3] = (uint8_t)link;
    s[4] = (uint8_t)(attr >> 8); s[5] = (uint8_t)attr;
    s[6] = (uint8_t)(x >> 8); s[7] = (uint8_t)x;
}

static void test_supplied_sprite_table_replaces_video_memory(void) {
    static uint8_t table[640];
    RrVdp *v = fine_scene(0x10000);
    uint32_t backdrop = rr_vdp_colour(v->cram[0]), tiles = rr_vdp_colour(v->cram[5]);
    memset(table, 0, sizeof table);
    /* Entry 0 shows the picture at screen x 200; entry 1, linked, is plain tiles at x 240. */
    put_entry(table, 0, 128 + 200, 136, 0x05, 1, 0x0003);
    put_entry(table, 1, 128 + 240, 136, 0x05, 0, 0x0003);
    fine_picture.x = 200;
    fine.table = table;
    assert(fine_at(v, 3, 48, 24) == backdrop);                       /* video memory's sprite at x 16 is not drawn */
    assert(fine_at(v, 3, 600, 24) == picture_colour(v, 0, 0));
    assert(fine_at(v, 3, 720, 24) == tiles && fine_at(v, 3, 720 + 47, 24 + 47) == tiles);
    /* A line with no finer picture on it still uses the supplied table. */
    fine.picture[0] = -1;
    assert(fine_at(v, 3, 600, 24) == tiles && fine_at(v, 3, 48, 24) == backdrop);
    fine.table = NULL;
    assert(fine_at(v, 3, 48, 24) == tiles && fine_at(v, 3, 600, 24) == backdrop);
}

static RrStrips strips;

static void test_strips_draw_behind_sprites_and_under_high_priority_planes(void) {
    RrVdp *v = fine_scene(0x10000);
    uint32_t backdrop = rr_vdp_colour(v->cram[0]);
    v->cram[0x3D] = 0x0E0E; /* the strips' colour: set 3, colour 13 */
    uint32_t strip = rr_vdp_colour(v->cram[0x3D]);
    memset(&strips, 0, sizeof strips);
    strips.scale = 3;
    strips.colour = 0x3D;
    for (int q = 24; q < 36; q++) { /* across the picture at (16, 8) .. (20, 12), and beyond it both ways */
        strips.from[q] = 30;
        strips.to[q] = 90;
    }
    fine.strips = &strips;
    assert(fine_at(v, 3, 29, 24) == backdrop && fine_at(v, 3, 30, 24) == strip);
    assert(fine_at(v, 3, 89, 24) == strip && fine_at(v, 3, 90, 24) == backdrop);
    assert(fine_at(v, 3, 48, 24) == picture_colour(v, 0, 0)); /* the sprite is in front */
    assert(fine_at(v, 3, 40, 36) == backdrop);                /* a row without a strip */
    /* A line with no sprite at all still gets its strip. */
    strips.from[150] = 300;
    strips.to[150] = 303;
    assert(fine_at(v, 3, 301, 150) == strip && fine_at(v, 3, 303, 150) == backdrop);
    /* A low-priority plane pixel is behind it, a high-priority one in front. */
    solid_tile(v, 9, 2);
    name(v, 0xC000, 4, 1, 0x0009);
    assert(fine_at(v, 3, 100, 30) == rr_vdp_colour(v->cram[2]));
    strips.to[30] = 130;
    assert(fine_at(v, 3, 100, 30) == strip);
    name(v, 0xC000, 4, 1, 0x8009);
    assert(fine_at(v, 3, 100, 30) == rr_vdp_colour(v->cram[2]));
    /* Strips that run off the picture do no harm. */
    strips.from[30] = -500;
    strips.to[30] = 5000;
    assert(fine_at(v, 3, 0, 30) == strip && fine_at(v, 3, 959, 30) == strip);
    fine.strips = NULL;
}

static void test_a_skipped_entry_is_not_drawn_but_still_counts(void) {
    RrVdp *v = fine_scene(0x10000);
    uint32_t backdrop = rr_vdp_colour(v->cram[0]), tiles = rr_vdp_colour(v->cram[5]);
    fine.picture[0] = -1; /* plain tiles */
    assert(fine_at(v, 3, 48, 24) == tiles);
    fine.skip[0] = 1;
    assert(fine_at(v, 3, 48, 24) == backdrop);
    /* Twenty-one sprites on one line, the first skipped: the twenty-first is still dropped. */
    for (int i = 0; i < 21; i++) sprite(v, i, 128 + 8 * i, 136, 0x00, i == 20 ? 0 : i + 1, 0x0003);
    assert(fine_at(v, 3, 3 * 8 * 19 + 1, 24) == tiles);
    assert(fine_at(v, 3, 3 * 8 * 20 + 1, 24) == backdrop);
    fine.skip[0] = 0;
    assert(fine_at(v, 3, 1, 24) == tiles && fine_at(v, 3, 3 * 8 * 20 + 1, 24) == backdrop);
    memset(fine.skip, 0, sizeof fine.skip);
}

static void test_a_skipped_entry_takes_its_pixels_from_the_lines_320(void) {
    RrVdp *v = fine_scene(0x10000);
    uint32_t backdrop = rr_vdp_colour(v->cram[0]), tiles = rr_vdp_colour(v->cram[5]);
    fine.picture[0] = -1;
    /* Ten sprites of 32 pixels fill the line's 320; an eleventh gets none, whether or not the first is drawn. */
    for (int i = 0; i < 10; i++) sprite(v, i, 128 + 32 * i, 136, 0x0C, i + 1, 0x0003);
    sprite(v, 10, 128 + 4, 136, 0x0C, 0, 0x0003);
    fine.skip[0] = 1;
    assert(fine_at(v, 2, 2 * 40, 16) == tiles);     /* the second sprite */
    assert(fine_at(v, 2, 2 * 10, 16) == backdrop);  /* where the first was, and the eleventh would be */
    fine.skip[0] = 0;
    fine.skip[3] = 1; /* skipped in the middle: the ones after it are where they were, the eleventh still dropped */
    assert(fine_at(v, 2, 2 * 100, 16) == backdrop && fine_at(v, 2, 2 * 130, 16) == tiles);
    assert(fine_at(v, 2, 2 * 300, 16) == tiles);
    memset(fine.skip, 0, sizeof fine.skip);
}

static void test_a_skipped_entry_at_x_0_still_hides_the_rest(void) {
    RrVdp *v = fine_scene(0x10000);
    uint32_t backdrop = rr_vdp_colour(v->cram[0]), tiles = rr_vdp_colour(v->cram[5]);
    fine.picture[0] = -1;
    sprite(v, 0, 128 + 16, 136, 0x00, 1, 0x0003);
    sprite(v, 1, 0, 136, 0x00, 2, 0x0003); /* x = 0: the video chip's mask */
    sprite(v, 2, 128 + 40, 136, 0x00, 0, 0x0003);
    assert(fine_at(v, 2, 2 * 17, 16) == tiles && fine_at(v, 2, 2 * 41, 16) == backdrop);
    fine.skip[1] = 1;
    assert(fine_at(v, 2, 2 * 17, 16) == tiles && fine_at(v, 2, 2 * 41, 16) == backdrop);
    memset(fine.skip, 0, sizeof fine.skip);
}

static void test_no_strips_and_no_skips_is_todays_picture(void) {
    static uint32_t other[RR_SCALE_MAX * RR_SCALE_MAX * RR_SCREEN_W];
    RrVdp *v = fine_scene(0x10000);
    memset(&strips, 0, sizeof strips); /* strips with nothing in them */
    strips.scale = 3;
    for (int line = 0; line < RR_SCREEN_H; line++) {
        fine.strips = NULL;
        rr_vdp_render_line_fine(v, line, 3, NULL, NULL, &fine, other);
        fine.strips = &strips;
        rr_vdp_render_line_fine(v, line, 3, NULL, NULL, &fine, big);
        assert(memcmp(big, other, 9 * RR_SCREEN_W * sizeof *big) == 0);
    }
    fine.strips = NULL;
}

/* The mean of some of the picture's colours, as the softened drawing rounds it. */
static uint32_t mean_of(const RrVdp *v, const int *colours, int count) {
    unsigned sum[3] = {0, 0, 0};
    for (int i = 0; i < count; i++) {
        uint32_t c = rr_vdp_colour(v->cram[colours[i]]);
        sum[0] += (c >> 16) & 0xFF;
        sum[1] += (c >> 8) & 0xFF;
        sum[2] += c & 0xFF;
    }
    return (uint32_t)(((sum[0] + (unsigned)count / 2) / (unsigned)count) << 16 |
                      ((sum[1] + (unsigned)count / 2) / (unsigned)count) << 8 | (sum[2] + (unsigned)count / 2) / (unsigned)count);
}

static void test_soft_half_size_picture_averages_four_pixels(void) {
    RrVdp *v = fine_scene(0x8000); /* the 4 x 4 picture drawn 2 x 2, at scale 1: four picture pixels to one */
    uint32_t backdrop = rr_vdp_colour(v->cram[0]);
    assert(fine_at(v, 1, 16, 8) != mean_of(v, (const int[]){0x1, 0x2, 0x5, 0x6}, 4)); /* sharp: one of the four */
    fine.soft = 1;
    assert(fine_at(v, 1, 16, 8) == mean_of(v, (const int[]){0x1, 0x2, 0x5, 0x6}, 4));
    assert(fine_at(v, 1, 17, 8) == mean_of(v, (const int[]){0x3, 0x4, 0x7, 0x8}, 4));
    assert(fine_at(v, 1, 16, 9) == mean_of(v, (const int[]){0x9, 0xA, 0xD, 0xE}, 4));
    /* The last has a see-through pixel among its four: the mean of the other three. */
    assert(fine_at(v, 1, 17, 9) == mean_of(v, (const int[]){0xB, 0xC, 0xF}, 3));
    assert(fine_at(v, 1, 18, 8) == backdrop && fine_at(v, 1, 16, 10) == backdrop);
    /* At scale 2 each output pixel covers one picture pixel: nothing to average. */
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) assert(fine_at(v, 2, 32 + sx, 16 + sy) == picture_colour(v, sx, sy));
    fine.soft = 0;
}

static void test_soft_leaves_natural_and_larger_pictures_alone(void) {
    static uint32_t sharp[RR_SCALE_MAX * RR_SCALE_MAX * RR_SCREEN_W];
    static const uint32_t sizes[3] = {0x10000, 0x18000, 0x20000};
    for (int i = 0; i < 3; i++) {
        RrVdp *v = fine_scene(sizes[i]);
        for (int line = 0; line < 40; line++) {
            fine.soft = 0;
            rr_vdp_render_line_fine(v, line, 3, NULL, NULL, &fine, sharp);
            fine.soft = 1;
            rr_vdp_render_line_fine(v, line, 3, NULL, NULL, &fine, big);
            assert(memcmp(big, sharp, 9 * RR_SCREEN_W * sizeof *big) == 0);
        }
    }
    fine.soft = 0;
}

static void test_soft_edge_follows_how_much_of_the_pixel_is_picture(void) {
    /* Row pairs: [1 0 | 0 0] over [0 0 | 3 3] and [0 0 | 3 0]: at half size the four output
     * pixels cover one opaque of four, two of four, none, and three of four. */
    static const uint8_t rows[4 * 3] = {0x10, 0x33, 0, 0x00, 0x30, 0, 0x00, 0x33, 0, 0x00, 0x33, 0};
    RrVdp *v = fine_scene(0x8000);
    uint32_t backdrop = rr_vdp_colour(v->cram[0]);
    fine_picture.rows = rows;
    fine.soft = 1;
    assert(fine_at(v, 1, 16, 8) == backdrop);                          /* a quarter covered: see-through */
    assert(fine_at(v, 1, 17, 8) == rr_vdp_colour(v->cram[3]));         /* three quarters: the picture's colour, no fringe */
    assert(fine_at(v, 1, 16, 9) == backdrop);                          /* nothing */
    assert(fine_at(v, 1, 17, 9) == rr_vdp_colour(v->cram[3]));         /* all four */
    fine_picture.rows = fine_rows;
    fine.soft = 0;
    /* Exactly half covered shows. */
    static const uint8_t half[4 * 3] = {0x12, 0x00, 0, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0};
    v = fine_scene(0x8000);
    fine_picture.rows = half;
    fine.soft = 1;
    assert(fine_at(v, 1, 16, 8) == mean_of(v, (const int[]){1, 2}, 2));
    fine_picture.rows = fine_rows;
    fine.soft = 0;
}

static void test_soft_keeps_order_priority_and_colour_set(void) {
    RrVdp *v = fine_scene(0x8000);
    fine.soft = 1;
    sprite(v, 0, 144, 136, 0x05, 0, 0x2003); /* colour set 1 */
    assert(fine_at(v, 1, 16, 8) == mean_of(v, (const int[]){0x11, 0x12, 0x15, 0x16}, 4));
    solid_tile(v, 9, 2);
    name(v, 0xC000, 2, 1, 0x8009); /* a high-priority plane pixel over it */
    assert(fine_at(v, 1, 16, 8) == rr_vdp_colour(v->cram[2]));
    name(v, 0xC000, 2, 1, 0x0009); /* and a low-priority one under it */
    assert(fine_at(v, 1, 16, 8) == mean_of(v, (const int[]){0x11, 0x12, 0x15, 0x16}, 4));
    /* A sprite in front of it still is. */
    sprite(v, 0, 144, 136, 0x05, 1, 0x2003);
    sprite(v, 1, 144, 136, 0x05, 0, 0x0003);
    fine.picture[1] = 0;
    sprite(v, 0, 144, 136, 0x00, 1, 0x0003); /* entry 0: plain tiles, colour 5 */
    fine.picture[0] = -1;
    assert(fine_at(v, 1, 16, 8) == rr_vdp_colour(v->cram[5]));
    fine.soft = 0;
}

static void test_fine_sprite_keeps_order_priority_and_colour_set(void) {
    RrVdp *v = fine_scene(0x10000);
    /* A second sprite behind the first, in the same place, without a picture. */
    sprite(v, 0, 144, 136, 0x05, 1, 0x2003); /* colour set 1 */
    sprite(v, 1, 144, 136, 0x05, 0, 0x0003);
    assert(fine_at(v, 3, 48, 24) == rr_vdp_colour(v->cram[16 + fine_pixel(0, 0)]));
    /* Where the picture is see-through (3, 3) the sprite behind shows; so it does past the picture. */
    assert(fine_at(v, 3, 48 + 9, 24 + 9) == rr_vdp_colour(v->cram[5]));
    assert(fine_at(v, 3, 48 + 12, 24) == rr_vdp_colour(v->cram[5]));
    /* A high-priority plane pixel covers the low-priority sprite, not a high-priority one. */
    solid_tile(v, 9, 6);
    name(v, PLANE_A, 2, 1, 0x8009);
    assert(fine_at(v, 3, 48, 24) == rr_vdp_colour(v->cram[6]));
    sprite(v, 0, 144, 136, 0x05, 1, 0xA003);
    assert(fine_at(v, 3, 48, 24) == rr_vdp_colour(v->cram[16 + fine_pixel(0, 0)]));
    /* The sources say which sprite supplied the pixel. */
}

static void test_entry_without_picture_or_with_size_zero_is_its_tiles(void) {
    static uint32_t other[RR_SCALE_MAX * RR_SCALE_MAX * RR_SCREEN_W];
    RrVdp *v = fine_scene(0x10000);
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0) fine.picture[0] = -1;
        else {
            fine.picture[0] = 0;
            fine_picture.size = 0;
        }
        for (int line = 6; line < 26; line++) {
            rr_vdp_render_line_scaled(v, line, 3, NULL, NULL, other);
            rr_vdp_render_line_fine(v, line, 3, NULL, NULL, &fine, big);
            assert(memcmp(big, other, 9 * RR_SCREEN_W * sizeof *big) == 0);
        }
    }
    fine.picture[0] = 7; /* a number past any picture is not followed */
    fine_picture.size = 0x10000;
    rr_vdp_render_line_fine(v, 8, 3, NULL, NULL, &fine, big);
}

static void test_fine_sprites_with_shifted_planes(void) {
    /* The planes keep their shifts on a line that has a fine sprite. */
    static uint32_t other[RR_SCALE_MAX * RR_SCALE_MAX * RR_SCREEN_W];
    static int32_t a[RR_SCALE_MAX], b[RR_SCALE_MAX];
    RrVdp *v = fine_scene(0x10000);
    v->cram[7] = 0x0444;
    tile_pixel(v, 8, 0, 0, 7);
    for (int cell = 0; cell < 64; cell++) name(v, PLANE_B, cell, 1, 0x0008);
    for (int r = 0; r < 3; r++) a[r] = b[r] = 0x10000 + r * 0x5556;
    rr_vdp_render_line_scaled(v, 8, 3, a, b, other);
    rr_vdp_render_line_fine(v, 8, 3, a, b, &fine, big);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < RR_SCREEN_W * 3; c++) {
            if (c >= 48 && c < 96) continue; /* the sprite's own columns */
            assert(big[r * RR_SCREEN_W * 3 + c] == other[r * RR_SCREEN_W * 3 + c]);
        }
    assert(big[48] == picture_colour(v, 0, 0));
}

static void test_fine_sprites_obey_the_per_line_limits(void) {
    static RrFinePicture pictures[RR_SPRITES_MAX];
    /* 22 one-cell sprites side by side, each with a picture: the chip shows 20 on a line. */
    RrVdp *v = fine_scene(0x10000);
    for (int i = 0; i < 22; i++) {
        sprite(v, i, 128 + 12 * i, 136, 0x00, i == 21 ? 0 : i + 1, 0x0003);
        pictures[i] = fine_picture;
        pictures[i].x = (int16_t)(12 * i);
        fine.picture[i] = (int16_t)i;
    }
    fine.pictures = pictures;
    uint32_t backdrop = rr_vdp_colour(v->cram[0]);
    assert(fine_at(v, 3, 3 * 12 * 19, 24) == picture_colour(v, 0, 0));
    assert(fine_at(v, 3, 3 * 12 * 20, 24) == backdrop && fine_at(v, 3, 3 * 12 * 21, 24) == backdrop);
    /* 11 sprites of 32 pixels: the chip draws 320 sprite pixels on a line, so the 11th is not shown. */
    v = fine_scene(0x10000);
    for (int i = 0; i < 11; i++) {
        sprite(v, i, 128 + (i < 10 ? 32 * i : 0), 136, 0x0C, i == 10 ? 0 : i + 1, 0x0003);
        pictures[i] = fine_picture;
        pictures[i].x = (int16_t)(i < 10 ? 32 * i : 4);
        fine.picture[i] = (int16_t)i;
    }
    fine.pictures = pictures;
    assert(fine_at(v, 3, 3 * 32 * 9, 24) == picture_colour(v, 0, 0));
    /* The 11th stands at x = 4, in the first sprite's rectangle but clear of its picture. */
    assert(fine_at(v, 3, 3 * 4, 24) == backdrop);
}

static void test_vram_writes_counts_every_kind_of_write(void) {
    RrVdp *v = fresh();
    control(v, 0x8F02);
    uint64_t before = v->vram_writes;
    set_address(v, 0x4000, 0x0000);
    rr_vdp_write_data(v, 0x1234);
    assert(v->vram_writes > before);
    before = v->vram_writes;
    set_address(v, 0xC000, 0x0000); /* colour memory */
    rr_vdp_write_data(v, 0x0EEE);
    set_address(v, 0x4000, 0x0010); /* vertical scroll memory */
    rr_vdp_write_data(v, 0x0001);
    assert(v->vram_writes == before);
    /* A fill. */
    control(v, 0x8114); /* DMA on */
    control(v, 0x9304);
    control(v, 0x9400);
    control(v, 0x9780);
    set_address(v, 0x4100, 0x0080);
    rr_vdp_write_data(v, 0xAA00);
    assert(v->vram_writes > before);
    before = v->vram_writes;
    /* A copy within video memory. */
    control(v, 0x9304);
    control(v, 0x9400);
    control(v, 0x9500);
    control(v, 0x9601);
    control(v, 0x97C0);
    set_address(v, 0x0200, 0x00C0);
    assert(v->vram_writes > before);
    before = v->vram_writes;
    /* A copy from 68000 memory. */
    control(v, 0x9302);
    control(v, 0x9400);
    control(v, 0x9500);
    control(v, 0x9600);
    control(v, 0x9700);
    set_address(v, 0x4300, 0x0080);
    assert(v->vram_writes > before);
}

static void test_line_state_reports_registers_and_scroll(void) {
    RrVdp *v = screen();
    v->reg[11] = 0x03; /* a horizontal scroll value per line */
    v->vram[HSCROLL + 5 * 4 + 1] = 7;  /* line 5, plane A */
    v->vram[HSCROLL + 5 * 4 + 3] = 9;  /* line 5, plane B */
    v->vsram[0] = 3; v->vsram[1] = 4;
    RrVdpLineState state;
    rr_vdp_line_state(v, 5, &state);
    assert(state.reg[11] == 0x03 && state.reg[5] == 0x6C);
    assert(state.hscroll[0] == 7 && state.hscroll[1] == 9);
    assert(state.vscroll[0] == 3 && state.vscroll[1] == 4);
}

static void test_shadow_highlight_renders_magenta(void) {
    RrVdp *v = screen();
    v->reg[12] = 0x89;
    rr_vdp_render_line(v, 0, row);
    assert(row[0] == 0xFF00FF && row[319] == 0xFF00FF);
}

static void test_write_ppm(void) {
    RrVdp *v = screen();
    v->cram[0] = 0x000E;
    char path[] = "/tmp/rr_ppm_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    assert(rr_vdp_write_ppm(v, path) == 0);
    FILE *f = fopen(path, "rb");
    char header[16];
    assert(f != NULL && fread(header, 1, 15, f) == 15);
    assert(memcmp(header, "P6\n320 224\n255\n", 15) == 0);
    assert(fgetc(f) == 0xFF && fgetc(f) == 0x00 && fgetc(f) == 0x00);
    fseek(f, 0, SEEK_END);
    assert(ftell(f) == 15 + 320 * 224 * 3);
    fclose(f);
    remove(path);
}

/* ---- whole-machine hardware ---- */

static RrGenesis *genesis(void) {
    (void)fresh();
    RrGenesis *g = rr_genesis_new(&machine);
    machine.hw = &g->hw;
    return g;
}

static void test_version_register(void) {
    RrGenesis *g = genesis();
    assert(rr_read8(&machine, 0xA10001) == 0xA0);
    rr_genesis_free(g);
}

static void test_data_port_write_reaches_the_video_chip(void) {
    RrGenesis *g = genesis();
    rr_write16(&machine, 0xC00004, 0x8F02);
    rr_write32(&machine, 0xC00004, 0x40000000);
    rr_write16(&machine, 0xC00000, 0x1234);
    assert(g->vdp.vram[0] == 0x12 && g->vdp.vram[1] == 0x34);
    rr_genesis_free(g);
}

/* Every write to the video chip can be reported to a tool: what kind, where,
 * what value, and for a copy from 68000 memory where the data came from. */
static RrVideoEvent seen[8];
static int seen_count;
static void note_event(void *ctx, const RrVideoEvent *e) {
    assert(ctx == (void *)seen);
    if (seen_count < 8) seen[seen_count] = *e;
    seen_count++;
}
static void test_video_log_reports_register_data_and_dma_writes(void) {
    RrGenesis *g = genesis();
    g->video_log = note_event;
    g->video_log_ctx = seen;
    seen_count = 0;
    rr_write16(&machine, 0xC00004, 0x8F02);     /* register 15 = 2 */
    rr_write32(&machine, 0xC00004, 0x40200000); /* VRAM write at 0x0020 */
    rr_write16(&machine, 0xC00000, 0x1234);
    rr_write32(&machine, 0xC00004, 0xC0060000); /* colour memory, entry 3 */
    rr_write16(&machine, 0xC00000, 0x0E00);
    assert(seen_count == 3);
    assert(seen[0].kind == RR_VIDEO_REGISTER && seen[0].target == 15 && seen[0].value == 2);
    assert(seen[1].kind == RR_VIDEO_VRAM && seen[1].target == 0x0020 && seen[1].value == 0x1234);
    assert(seen[2].kind == RR_VIDEO_CRAM && seen[2].target == 0x0006 && seen[2].value == 0x0E00);
    /* A copy of 3 words from work RAM 0xFF8000 to VRAM 0xC400. */
    seen_count = 0;
    machine.ram[0x8000] = 0xAB;
    rr_write16(&machine, 0xC00004, 0x8154);     /* display and DMA on */
    rr_write16(&machine, 0xC00004, 0x9303);     /* length 3 */
    rr_write16(&machine, 0xC00004, 0x9400);
    rr_write16(&machine, 0xC00004, 0x9500);     /* source 0xFF8000 / 2 = 0x7FC000 */
    rr_write16(&machine, 0xC00004, 0x96C0);
    rr_write16(&machine, 0xC00004, 0x977F);
    rr_write32(&machine, 0xC00004, 0x44000083); /* VRAM 0xC400, DMA */
    assert(seen_count == 7);
    assert(seen[6].kind == RR_VIDEO_DMA && seen[6].memory == RR_VIDEO_VRAM);
    assert(seen[6].target == 0xC400 && seen[6].value == 3 && seen[6].source == 0xFF8000);
    assert(g->vdp.vram[0xC400] == 0xAB);
    /* A fill of 4 bytes of 0x55 from VRAM 0x0100. */
    seen_count = 0;
    rr_write16(&machine, 0xC00004, 0x8F01);
    rr_write16(&machine, 0xC00004, 0x9304);
    rr_write16(&machine, 0xC00004, 0x9780);
    rr_write32(&machine, 0xC00004, 0x41000080);
    rr_write16(&machine, 0xC00000, 0x5500);
    assert(seen_count == 5);
    assert(seen[4].kind == RR_VIDEO_FILL && seen[4].value == 4 && seen[4].source == 0x55);
    assert(seen[4].frame == 0 && seen[4].line == rr_genesis_line(g));
    rr_genesis_free(g);
}

static void test_bus_request_is_granted(void) {
    RrGenesis *g = genesis();
    assert((rr_read8(&machine, 0xA11100) & 1) == 1); /* Z80 owns its bus */
    rr_write16(&machine, 0xA11100, 0x0100);
    assert((rr_read8(&machine, 0xA11100) & 1) == 0);
    rr_write16(&machine, 0xA11100, 0x0000);
    assert((rr_read8(&machine, 0xA11100) & 1) == 1);
    rr_genesis_free(g);
}

static void test_z80_ram_is_mirrored(void) {
    RrGenesis *g = genesis();
    rr_write8(&machine, 0xA00286, 0x5A);
    assert(g->z80_ram[0x286] == 0x5A);
    assert(rr_read8(&machine, 0xA02286) == 0x5A);
    rr_genesis_free(g);
}

/* A Z80 program the 68000 loads: LD A,0x2A; LD (0x1000),A; LD A,0x2B;
 * LD (0x4000),A; HALT. */
static void load_sound_program(void) {
    static const uint8_t program[] = {0x3E, 0x2A, 0x32, 0x00, 0x10, 0x3E, 0x2B, 0x32, 0x00, 0x40, 0x76};
    rr_write16(&machine, 0xA11100, 0x0100); /* take the Z80's bus */
    rr_write16(&machine, 0xA11200, 0x0100); /* let go of its reset */
    for (unsigned i = 0; i < sizeof program; i++) rr_write8(&machine, 0xA00000 + i, program[i]);
    rr_write16(&machine, 0xA11200, 0x0000); /* reset it */
    rr_write16(&machine, 0xA11200, 0x0100);
    rr_write16(&machine, 0xA11100, 0x0000); /* give the bus back: it runs */
}

static void test_sound_unit_is_driven(void) {
    RrGenesis *g = genesis();
    uint8_t rom_bytes[16] = {0};
    g->sound = rr_sound_new(rom_bytes, sizeof rom_bytes, g->z80_ram);
    load_sound_program();
    assert(g->z80_ram[0x1000] == 0); /* no time has passed */
    machine.cpu.cycles += 2000;
    assert(rr_read8(&machine, 0xA01000) == 0x2A); /* reading its memory lets it catch up first */
    assert(rr_sound_z80_steps(g->sound) >= 5 && rr_sound_fm_writes(g->sound) == 1);
    /* A 68000 write to the FM chip reaches it. */
    rr_write8(&machine, 0xA04000, 0x30);
    assert(rr_sound_fm_writes(g->sound) == 2);
    assert(rr_sound_fm_writes_68000(g->sound) == 1);
    /* What the 68000 sends the sound side is counted. */
    assert(g->z80_ram_writes == 11 && g->psg_writes == 0);
    rr_write8(&machine, 0xC00011, 0x9F);
    assert(g->psg_writes == 1);
    /* While the 68000 holds the bus the Z80 does nothing. */
    uint64_t steps = rr_sound_z80_steps(g->sound);
    rr_write16(&machine, 0xA11100, 0x0100);
    machine.cpu.cycles += 50000;
    (void)rr_read8(&machine, 0xA01000);
    assert(rr_sound_z80_steps(g->sound) == steps);
    /* Power-on starts the sound side again: reset held, time from the start. */
    rr_genesis_power_on(g);
    g->z80_ram[0x1000] = 0;
    machine.cpu.cycles += 50000;
    (void)rr_read8(&machine, 0xA01000);
    assert(g->z80_ram[0x1000] == 0 && rr_sound_z80_steps(g->sound) == 0);
    rr_sound_free(g->sound);
    rr_genesis_free(g);
}

static void test_no_sound_unit_is_as_before(void) {
    RrGenesis *g = genesis();
    assert(g->sound == NULL);
    load_sound_program();
    machine.cpu.cycles += 2000;
    assert(rr_read8(&machine, 0xA01000) == 0); /* nothing runs the program */
    rr_write8(&machine, 0xA04000, 0x30);
    rr_genesis_free(g);
}

/* The sound side follows real time: a faster 68000 does not make more sound. */
static void test_sound_follows_time_not_cpu_speed(void) {
    RrGenesis *g = genesis();
    uint8_t rom_bytes[16] = {0};
    g->sound = rr_sound_new(rom_bytes, sizeof rom_bytes, g->z80_ram);
    rr_genesis_set_cpu_speed(g, 200);
    /* Ten frames of real time are twice ten frames' worth of 68000 cycles. */
    machine.cpu.cycles += 2 * 10 * (RR_MASTER_PER_FRAME / 7);
    (void)rr_read8(&machine, 0xA01000);
    size_t have = rr_sound_available(g->sound);
    size_t want = (size_t)(10 * RR_MASTER_PER_FRAME / RR_SOUND_MASTER_PER_SAMPLE);
    assert(have + 2 >= want && have <= want + 2);
    rr_sound_free(g->sound);
    rr_genesis_free(g);
}

static void test_idle_three_button_pad(void) {
    RrGenesis *g = genesis();
    rr_write8(&machine, 0xA10009, 0x40); /* TH is an output */
    rr_write8(&machine, 0xA10003, 0x40);
    assert(rr_read8(&machine, 0xA10003) == 0x7F);
    rr_write8(&machine, 0xA10003, 0x00);
    assert(rr_read8(&machine, 0xA10003) == 0x33);
    rr_genesis_free(g);
}

static void test_pressed_buttons_read_low(void) {
    RrGenesis *g = genesis();
    g->pad_buttons[0] = 0x01 | 0x10 | 0x80; /* up, B, Start */
    rr_write8(&machine, 0xA10009, 0x40);
    rr_write8(&machine, 0xA10003, 0x40);
    assert(rr_read8(&machine, 0xA10003) == 0x6E); /* up and B low */
    rr_write8(&machine, 0xA10003, 0x00);
    assert(rr_read8(&machine, 0xA10003) == 0x12); /* up and Start low */
    rr_genesis_free(g);
}

/* Put the machine at a master-clock position within a frame. */
static void at_master(RrGenesis *g, uint64_t line, uint64_t column) {
    (void)g;
    uint64_t master = line * RR_MASTER_PER_LINE + column + 10ull * RR_MASTER_PER_FRAME;
    machine.cpu.cycles = ((master - RR_CLOCK_PHASE) * RR_CPU_HZ + RR_MASTER_HZ - 1) / RR_MASTER_HZ;
}

/* Counter values MAME returned at these cycle counts (race-start trace window):
 * the video position is a function of absolute time alone. */
static void test_hv_counter_reproduces_mame_samples(void) {
    RrGenesis *g = genesis();
    machine.cpu.cycles = 132614755; /* scanline 1, position 357: above the H jump */
    assert(rr_read16(&machine, 0xC00008) == 0x011C);
    machine.cpu.cycles = 132614885; /* scanline 2, position 5 */
    assert(rr_read16(&machine, 0xC00008) == 0x0205);
    machine.cpu.cycles = 132615353; /* scanline 2, position 465: V already shows the next line */
    assert(rr_read16(&machine, 0xC00008) == 0x0388);
    machine.cpu.cycles = 132617339; /* a byte read of H */
    assert(rr_read8(&machine, 0xC00009) == 0x10);
    rr_genesis_free(g);
}

/* Time is kept finer than a master clock: MAME's clocks do not divide evenly,
 * and rounding to whole master clocks first puts one read in twenty a count
 * out. These reads straddle such a rounding (race-start trace window). */
static void test_hv_counter_keeps_sub_master_clock_precision(void) {
    RrGenesis *g = genesis();
    static const struct { uint64_t cycles; uint16_t value; } samples[] = {
        {132614807ull, 0x0150}, {132617315ull, 0x0790}, {132629627ull, 0x2059}, {132635783ull, 0x2C30}, {132652085ull, 0x4E49}, {132689705ull, 0x9B49},
    };
    for (unsigned i = 0; i < sizeof samples / sizeof samples[0]; i++) {
        machine.cpu.cycles = samples[i].cycles;
        assert(rr_read16(&machine, 0xC00008) == samples[i].value);
    }
    rr_genesis_free(g);
}

/* MAME keeps CPU time in whole attoseconds per cycle (rounded down), so it
 * runs a hair behind the true clock ratio, and a read in the last CPU cycle of
 * a line shows position 480 with the line count already moved on. Reads from
 * the race-start and race trace windows that only come out right with both. */
static void test_hv_counter_follows_mame_clock_rounding_and_line_end(void) {
    RrGenesis *g = genesis();
    static const struct { uint64_t cycles; uint16_t value; } samples[] = {
        {132635887ull, 0x2E97}, {133035013ull, 0x4B71}, {134098181ull, 0x9C96},
        {513464803ull, 0x45D5}, {513943833ull, 0x016E},
    };
    for (unsigned i = 0; i < sizeof samples / sizeof samples[0]; i++) {
        machine.cpu.cycles = samples[i].cycles;
        assert(rr_read16(&machine, 0xC00008) == samples[i].value);
    }
    machine.cpu.cycles = 135433469ull;
    assert(rr_read8(&machine, 0xC00009) == 0x14);
    rr_genesis_free(g);
}

/* Hours into a session the time arithmetic must not wrap. */
static void test_hv_counter_survives_long_sessions(void) {
    RrGenesis *g = genesis();
    static const struct { uint64_t cycles; uint16_t value; } samples[] = { {55227273945ull, 0xDED3}, {1325454279177ull, 0x11B8} };
    for (unsigned i = 0; i < sizeof samples / sizeof samples[0]; i++) {
        machine.cpu.cycles = samples[i].cycles;
        assert(rr_read16(&machine, 0xC00008) == samples[i].value);
    }
    rr_genesis_free(g);
}

static void test_vertical_counter_and_blanking(void) {
    RrGenesis *g = genesis();
    at_master(g, 100, 500);
    assert((rr_read16(&machine, 0xC00008) >> 8) == 100 && rr_genesis_line(g) == 100);
    at_master(g, 100, 3300); /* position 463 of 480: the counter has stepped */
    assert((rr_read16(&machine, 0xC00008) >> 8) == 101 && rr_genesis_line(g) == 100);
    at_master(g, 234, 500);
    assert((rr_read16(&machine, 0xC00008) >> 8) == 0xEA);
    at_master(g, 235, 500);
    assert((rr_read16(&machine, 0xC00008) >> 8) == 0xE5); /* the NTSC counter jumps back here */
    at_master(g, 261, 500);
    assert((rr_read16(&machine, 0xC00008) >> 8) == 0xFF);
    g->vdp.reg[1] = 0x40; /* display on, so blanking is the beam's doing */
    at_master(g, 223, 500);
    assert((rr_read16(&machine, 0xC00004) & 0x0008) == 0);
    at_master(g, 224, 500);
    assert((rr_read16(&machine, 0xC00004) & 0x0008) != 0);
    rr_genesis_free(g);
}

/* The cycle count is MAME's: 34 at the first instruction after power-on. Video
 * time and the interrupt entry delay both depend on the absolute count. */
static void test_power_on_starts_the_cycle_count_where_mame_does(void) {
    RrGenesis *g = genesis();
    rom[0] = 0x00; rom[1] = 0xFF; rom[2] = 0xFF; rom[3] = 0xF6; /* stack pointer */
    rom[4] = 0x00; rom[5] = 0x00; rom[6] = 0x02; rom[7] = 0x00; /* start address */
    machine.cpu.d[3] = 7;
    rr_genesis_power_on(g);
    assert(machine.cpu.cycles == 34);
    assert(machine.cpu.pc == 0x200 && machine.cpu.a[7] == 0x00FFFFF6 && machine.cpu.sr == 0x2700);
    assert(machine.cpu.d[3] == 0);
    memset(rom, 0, 8);
    rr_genesis_free(g);
}

static void test_master_time_uses_the_clock_rates_mame_uses(void) {
    RrGenesis *g = genesis();
    /* A second's worth of CPU cycles falls just short of a second of master
     * clocks, because MAME rounds the length of a CPU cycle down. */
    machine.cpu.cycles = RR_CPU_HZ;
    assert(rr_genesis_master_time(g) == RR_MASTER_HZ + RR_CLOCK_PHASE - 1);
    machine.cpu.cycles = 0;
    assert(rr_genesis_master_time(g) == RR_CLOCK_PHASE);
    rr_genesis_free(g);
}

/* MAME holds the 68000 during a 68000-to-VDP DMA: one cycle for every 228
 * words after the first word (456 words cost one cycle, 2512 cost eleven,
 * 2736 still eleven). Exact for all 347 such DMAs, of 61 different lengths,
 * in the six race trace windows. */
static void test_dma_holds_the_cpu_in_proportion_to_its_length(void) {
    static const struct { unsigned words, cycles; } cases[] = {
        {64, 0}, {344, 1}, {456, 1}, {504, 2}, {832, 3}, {1856, 8}, {2512, 11},
        {2736, 11}, {2768, 12}, {2976, 13}, {3000, 13},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        RrGenesis *g = genesis();
        rr_write16(&machine, 0xC00004, 0x8114);
        rr_write16(&machine, 0xC00004, 0x8F02);
        rr_write16(&machine, 0xC00004, (uint16_t)(0x9300 | (cases[i].words & 0xFF)));
        rr_write16(&machine, 0xC00004, (uint16_t)(0x9400 | (cases[i].words >> 8)));
        rr_write16(&machine, 0xC00004, 0x9500);
        rr_write16(&machine, 0xC00004, 0x9680);
        rr_write16(&machine, 0xC00004, 0x977F);
        rr_write16(&machine, 0xC00004, 0x4000);
        machine.cpu.cycles = 7000;
        rr_write16(&machine, 0xC00004, 0x0080);
        assert(machine.cpu.cycles == 7000 + cases[i].cycles);
        rr_genesis_free(g);
    }
}

static void test_byte_write_to_z80_memory_costs_one_extra_cycle(void) {
    RrGenesis *g = genesis();
    machine.cpu.cycles = 5000;
    rr_write8(&machine, 0xA00286, 1);
    assert(machine.cpu.cycles == 5001); /* as MAME charges; found at 0x2C34E and 0x2C388 */
    rr_write16(&machine, 0xC00004, 0x8F02);
    assert(machine.cpu.cycles == 5001);
    rr_genesis_free(g);
}

static void expect_fatal(void (*fn)(void), const char *needle) {
    int fds[2];
    assert(pipe(fds) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        fn();
        _exit(0);
    }
    close(fds[1]);
    char buf[1024];
    ssize_t n, total = 0;
    while ((n = read(fds[0], buf + total, sizeof buf - 1 - (size_t)total)) > 0) total += n;
    buf[total] = 0;
    close(fds[0]);
    int status;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 2);
    assert(strstr(buf, "RR2FATAL") != NULL && strstr(buf, needle) != NULL);
}

static void unknown_address(void) {
    (void)genesis();
    (void)rr_read8(&machine, 0xB00000);
}

/* ---- the opaque runs of a plane row ---- */

/* A screen whose planes are 128 cells (1,024 pixels) wide, as the game's are;
 * plane A at 0x4000, plane B at 0x6000. */
static RrVdp *wide_planes(void) {
    RrVdp *v = screen();
    v->reg[2] = 0x10;  /* plane A at 0x4000 */
    v->reg[4] = 0x03;  /* plane B at 0x6000 */
    v->reg[16] = 0x03; /* 128 x 32 cells */
    return v;
}

/* Colour `colour` on pixels [from, to) of pixel row `py` of a plane, each
 * touched cell getting a tile of its own (tiles 0x400 to 0x5FF, clear of
 * the planes and tables; two cell rows at most are told apart). */
static void plane_pixels(RrVdp *v, int which, int py, int from, int to, int colour) {
    uint32_t base = which == 0 ? 0x4000 : 0x6000;
    for (int px = from; px < to; px++) {
        int cell = (py >> 3) * 128 + (px >> 3), tile = 0x400 + (which * 0x100) + ((py >> 3) & 1) * 0x80 + (px >> 3);
        v->vram[base + cell * 2] = (uint8_t)(tile >> 8);
        v->vram[base + cell * 2 + 1] = (uint8_t)tile;
        tile_pixel(v, tile, px & 7, py & 7, colour);
    }
}

static void test_runs_of_a_block_with_two_marks(void) {
    RrVdp *v = wide_planes();
    plane_pixels(v, 0, 3, 331, 338, 5);
    plane_pixels(v, 0, 3, 352, 672, 6);
    plane_pixels(v, 0, 3, 686, 693, 5);
    int first[4], end[4];
    assert(rr_vdp_plane_row_runs(v, 0, 3, first, end, 4) == 3);
    assert(first[0] == 331 && end[0] == 338 && first[1] == 352 && end[1] == 672 && first[2] == 686 && end[2] == 693);
    /* Another pixel row of the same cells, and the other plane, hold nothing. */
    assert(rr_vdp_plane_row_runs(v, 0, 4, first, end, 4) == 0);
    assert(rr_vdp_plane_row_runs(v, 1, 3, first, end, 4) == 0);
}

static void test_runs_follow_the_vertical_scroll(void) {
    RrVdp *v = wide_planes();
    plane_pixels(v, 1, 13, 352, 672, 6);
    v->vsram[1] = 10; /* plane B scrolled up by 10: its row 13 shows on line 3 */
    int first[2], end[2];
    assert(rr_vdp_plane_row_runs(v, 1, 3, first, end, 2) == 1 && first[0] == 352 && end[0] == 672);
    assert(rr_vdp_plane_row_runs(v, 1, 13, first, end, 2) == 0);
    v->vsram[1] = 0x100 + 10; /* the plane is 256 lines high: the same row again */
    assert(rr_vdp_plane_row_runs(v, 1, 3, first, end, 2) == 1);
}

static void test_a_full_row_is_one_run_0_to_1024(void) {
    RrVdp *v = wide_planes();
    plane_pixels(v, 0, 0, 0, 1024, 2);
    int first[2], end[2];
    assert(rr_vdp_plane_row_runs(v, 0, 0, first, end, 2) == 1 && first[0] == 0 && end[0] == 1024);
}

static void test_an_empty_row_has_no_runs(void) {
    RrVdp *v = wide_planes();
    int first[2] = {-1, -1}, end[2] = {-1, -1};
    assert(rr_vdp_plane_row_runs(v, 0, 100, first, end, 2) == 0 && first[0] == -1 && end[0] == -1);
}

static void test_more_runs_than_room_says_so(void) {
    RrVdp *v = wide_planes();
    for (int i = 0; i < 5; i++) plane_pixels(v, 0, 0, 100 * i + 10, 100 * i + 20, 2);
    int first[5], end[5];
    assert(rr_vdp_plane_row_runs(v, 0, 0, first, end, 3) == 4);
    assert(first[0] == 10 && end[0] == 20 && first[2] == 210 && end[2] == 220);
    assert(rr_vdp_plane_row_runs(v, 0, 0, first, end, 5) == 5);
}

static void test_a_plane_row_as_pixels(void) {
    RrVdp *v = wide_planes();
    plane_pixels(v, 1, 13, 352, 360, 6);
    plane_pixels(v, 1, 13, 1023, 1024, 9);
    v->vram[0x6000 + (128 + 44) * 2] |= 0x40; /* the cell at columns 352 to 359: palette line 2 */
    v->vsram[1] = 10;
    static uint8_t row[1024];
    memset(row, 0xEE, sizeof row);
    assert(rr_vdp_plane_row_pixels(v, 1, 3, row) == 1024);
    assert(row[351] == 0 && row[352] == (2 << 4 | 6) && row[359] == (2 << 4 | 6) && row[360] == 0);
    assert(row[0] == 0 && row[1023] == 9);
    /* A 64-cell plane is 512 wide, and nothing past that is written. */
    v->reg[16] = 0x01;
    memset(row, 0xEE, sizeof row);
    assert(rr_vdp_plane_row_pixels(v, 1, 3, row) == 512 && row[511] != 0xEE && row[512] == 0xEE);
}

/* The narrow enlarged picture of two scenes, every line, scales 1 to 4, with
 * shifts and with fine sprites, as one number: taken before the wide row was
 * added, so that making rr_vdp_render_line_fine a call of the wide one is
 * known to have changed nothing. */
static uint64_t narrow_picture_hash(void) {
    static int32_t a[RR_SCALE_MAX], b[RR_SCALE_MAX];
    uint64_t hash = 1469598103934665603ull;
    for (int scene = 0; scene < 2; scene++) {
        RrVdp *v = scene == 0 ? shifted_frame() : fine_scene(0x18000);
        if (scene == 1) fine.soft = 1;
        for (int scale = 1; scale <= 4; scale++) {
            for (int line = 0; line < RR_SCREEN_H; line++) {
                for (int r = 0; r < scale; r++) {
                    a[r] = (int32_t)((line * 3) << 16) + r * 0x5555;
                    b[r] = (int32_t)(((0x400 - line) & 0x3FF) << 16) + r * 0x3333;
                }
                for (int with_shifts = 0; with_shifts < 2; with_shifts++) {
                    rr_vdp_render_line_fine(v, line, scale, with_shifts ? a : NULL, with_shifts ? b : NULL,
                                            scene == 1 ? &fine : NULL, big);
                    for (int i = 0; i < scale * scale * RR_SCREEN_W; i++) hash = (hash ^ big[i]) * 1099511628211ull;
                }
            }
        }
    }
    return hash;
}

static void test_the_narrow_picture_is_what_it_was(void) {
    uint64_t hash = narrow_picture_hash();
    if (hash != NARROW_PICTURE_HASH) printf("narrow picture hash 0x%016llxull\n", (unsigned long long)hash);
    assert(hash == NARROW_PICTURE_HASH);
}

/* --- the wide row ------------------------------------------------------- */

enum { WIDE_W_MAX = RR_SCREEN_W + 2 * RR_WIDE_MARGIN_MAX };
static uint32_t wide_big[RR_SCALE_MAX * RR_SCALE_MAX * WIDE_W_MAX];
static RrPixelSource wide_src[RR_SCALE_MAX * RR_SCALE_MAX * WIDE_W_MAX];

static RrWide wide_of(int margin, int lines) {
    RrWide w;
    memset(&w, 0, sizeof w);
    w.margin = margin;
    w.lines = lines;
    w.panel = 0x123456;
    return w;
}

static void test_margin_0_is_render_line_fine(void) {
    static int32_t a[RR_SCALE_MAX], b[RR_SCALE_MAX];
    RrWide none = wide_of(0, RR_SCREEN_H);
    for (int scene = 0; scene < 2; scene++) {
        RrVdp *v = scene == 0 ? shifted_frame() : fine_scene(0x18000);
        const RrFineSprites *f = scene == 1 ? &fine : NULL;
        for (int scale = 1; scale <= 4; scale++) {
            size_t bytes = (size_t)scale * (size_t)scale * RR_SCREEN_W * sizeof *big;
            for (int line = 0; line < RR_SCREEN_H; line++) {
                for (int r = 0; r < scale; r++) {
                    a[r] = (int32_t)((line * 3) << 16) + r * 0x5555;
                    b[r] = (int32_t)(((0x400 - line) & 0x3FF) << 16) + r * 0x3333;
                }
                for (int with_shifts = 0; with_shifts < 2; with_shifts++) {
                    const int32_t *sa = with_shifts ? a : NULL, *sb = with_shifts ? b : NULL;
                    rr_vdp_render_line_fine(v, line, scale, sa, sb, f, big);
                    memset(wide_big, 0xEE, bytes);
                    rr_vdp_render_line_wide(v, line, scale, sa, sb, NULL, NULL, f, NULL, &none, wide_big, NULL);
                    assert(memcmp(wide_big, big, bytes) == 0);
                    memset(wide_big, 0xEE, bytes);
                    rr_vdp_render_line_wide(v, line, scale, sa, sb, NULL, NULL, f, NULL, NULL, wide_big, NULL);
                    assert(memcmp(wide_big, big, bytes) == 0);
                }
            }
        }
    }
}

static void test_the_centre_of_a_wide_row_is_the_narrow_row(void) {
    static int32_t a[RR_SCALE_MAX], b[RR_SCALE_MAX];
    RrWide w = wide_of(39, RR_SCREEN_H);
    int compared = 0;
    for (int scene = 0; scene < 2; scene++) {
        RrVdp *v = scene == 0 ? shifted_frame() : fine_scene(0x18000);
        const RrFineSprites *f = scene == 1 ? &fine : NULL;
        if (scene == 1) fine.soft = 1;
        for (int line = 0; line < RR_SCREEN_H; line++) {
            RrPixelSource narrow[RR_SCREEN_W];
            rr_vdp_render_line_sources(v, line, narrow);
            int window = 0;
            for (int x = 0; x < RR_SCREEN_W; x++) window |= narrow[x].layer == RR_SRC_WINDOW;
            if (window) continue;
            for (int r = 0; r < 3; r++) {
                a[r] = (int32_t)((line * 3) << 16) + r * 0x5555;
                b[r] = (int32_t)(((0x400 - line) & 0x3FF) << 16) + r * 0x3333;
            }
            for (int with_shifts = 0; with_shifts < 2; with_shifts++) {
                const int32_t *sa = with_shifts ? a : NULL, *sb = with_shifts ? b : NULL;
                rr_vdp_render_line_fine(v, line, 3, sa, sb, f, big);
                rr_vdp_render_line_wide(v, line, 3, sa, sb, NULL, NULL, f, NULL, &w, wide_big, NULL);
                for (int r = 0; r < 3; r++)
                    assert(memcmp(&wide_big[r * 398 * 3 + 39 * 3], &big[r * RR_SCREEN_W * 3],
                                  RR_SCREEN_W * 3 * sizeof *big) == 0);
                compared++;
            }
        }
    }
    assert(compared > 400);
}

/* Two 128-cell planes, plane A holding colour 5 at plane column 1016 and
 * colour 6 at column 327 of its row 0; the backdrop is colour 0. */
static RrVdp *margin_scene(void) {
    RrVdp *v = wide_planes();
    v->cram[0] = 0x0222;
    v->cram[5] = 0x00E0;
    v->cram[6] = 0x000E;
    plane_pixels(v, 0, 0, 1016, 1017, 5);
    plane_pixels(v, 0, 0, 327, 328, 6);
    return v;
}

static void test_margins_show_the_plane_beyond_the_edge(void) {
    RrVdp *v = margin_scene();
    RrWide w = wide_of(39, RR_SCREEN_H);
    uint32_t back = rr_vdp_colour(0x0222), five = rr_vdp_colour(0x00E0), six = rr_vdp_colour(0x000E);
    int32_t zero[2] = {0, 0};
    rr_vdp_render_line_wide(v, 0, 2, zero, zero, NULL, NULL, NULL, NULL, &w, wide_big, wide_src);
    for (int r = 0; r < 2; r++) {
        const uint32_t *out = &wide_big[r * 398 * 2];
        /* Screen x = -8 is output columns 62 and 63; x = 327 is 732 and 733. */
        assert(out[61] == back && out[62] == five && out[63] == five && out[64] == back);
        assert(out[731] == back && out[732] == six && out[733] == six && out[734] == back);
        assert(out[0] == back && out[795] == back);
        const RrPixelSource *src = &wide_src[r * 398 * 2];
        assert(src[62].layer == RR_SRC_PLANE_A && src[62].colour == 5 && src[61].layer == RR_SRC_BACKDROP);
        assert(src[733].layer == RR_SRC_PLANE_A && src[733].colour == 6);
    }
}

static void test_a_fractional_shift_carries_into_the_margins(void) {
    RrVdp *v = margin_scene();
    RrWide w = wide_of(39, RR_SCREEN_H);
    uint32_t back = rr_vdp_colour(0x0222), five = rr_vdp_colour(0x00E0), six = rr_vdp_colour(0x000E);
    /* Half a pixel to the right on row 0, a pixel and a quarter on row 1. */
    int32_t a[4] = {0x8000, 0x14000, 0, 0}, b[4] = {0, 0, 0, 0};
    rr_vdp_render_line_wide(v, 0, 4, a, b, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
    const uint32_t *row0 = wide_big, *row1 = &wide_big[398 * 4];
    assert(row0[125] == back && row0[126] == five && row0[129] == five && row0[130] == back);
    assert(row0[1465] == back && row0[1466] == six && row0[1469] == six && row0[1470] == back);
    assert(row1[128] == back && row1[129] == five && row1[132] == five && row1[133] == back);
    assert(row1[1468] == back && row1[1469] == six && row1[1472] == six && row1[1473] == back);
}

static void test_margins_keep_the_edge_columns_vertical_scroll(void) {
    RrVdp *v = wide_planes();
    RrWide w = wide_of(39, RR_SCREEN_H);
    v->cram[0] = 0x0222;
    v->cram[5] = 0x00E0;
    v->cram[6] = 0x000E;
    v->reg[11] = 0x04;   /* a vertical scroll value for every two cells */
    v->vsram[0] = 8;     /* plane A's first column shows the plane 8 rows down */
    v->vsram[38] = 16;   /* and its last column 16 rows down */
    plane_pixels(v, 0, 8, 1016, 1017, 5);
    plane_pixels(v, 0, 16, 327, 328, 6);
    rr_vdp_render_line_wide(v, 0, 1, NULL, NULL, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
    assert(wide_big[31] == rr_vdp_colour(0x00E0) && wide_big[366] == rr_vdp_colour(0x000E));
    assert(wide_big[30] == rr_vdp_colour(0x0222) && wide_big[367] == rr_vdp_colour(0x0222));
}

static void assert_narrow_between_panels(const RrVdp *v, int line, int scale, const RrWide *w) {
    int wide = (RR_SCREEN_W + 2 * w->margin) * scale, edge = w->margin * scale;
    rr_vdp_render_line_fine(v, line, scale, NULL, NULL, NULL, big);
    rr_vdp_render_line_wide(v, line, scale, NULL, NULL, NULL, NULL, NULL, NULL, w, wide_big, wide_src);
    for (int r = 0; r < scale; r++) {
        const uint32_t *out = &wide_big[r * wide];
        for (int c = 0; c < edge; c++) {
            assert(out[c] == w->panel && out[wide - 1 - c] == w->panel);
            assert(wide_src[r * wide + c].layer == RR_SRC_PANEL && wide_src[r * wide + wide - 1 - c].layer == RR_SRC_PANEL);
        }
        assert(memcmp(&out[edge], &big[r * RR_SCREEN_W * scale], (size_t)RR_SCREEN_W * (size_t)scale * sizeof *big) == 0);
        assert(wide_src[r * wide + edge].layer != RR_SRC_PANEL);
    }
}

static void test_a_window_line_is_the_original_between_panels(void) {
    RrVdp *v = screen();
    RrWide w = wide_of(39, RR_SCREEN_H);
    v->cram[0] = 0x0222;
    v->cram[1] = 0x00E0;
    v->cram[2] = 0x000E;
    solid_tile(v, 1, 1);
    solid_tile(v, 2, 2);
    for (int cell = 0; cell < 64; cell++) name(v, PLANE_A, cell, 0, 0x0001);
    for (int cell = 0; cell < 64; cell++) name(v, PLANE_A, cell, 1, 0x0001);
    name(v, 0xF000, 0, 0, 0x0002);
    v->reg[18] = 0x01; /* the window covers the top cell row */
    assert_narrow_between_panels(v, 0, 2, &w);
    assert_narrow_between_panels(v, 7, 2, &w);
    /* Line 8 has no window: plane A runs on into both margins. */
    rr_vdp_render_line_wide(v, 8, 2, NULL, NULL, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
    assert(wide_big[0] == rr_vdp_colour(0x00E0) && wide_big[398 * 2 - 1] == rr_vdp_colour(0x00E0));
}

static void test_lines_from_wide_lines_down_are_the_original_between_panels(void) {
    RrVdp *v = shifted_frame();
    RrWide w = wide_of(39, 152);
    for (int line = 152; line < RR_SCREEN_H; line += 9) assert_narrow_between_panels(v, line, 3, &w);
    RrVdp *m = margin_scene();
    plane_pixels(m, 0, 151, 1016, 1017, 5);
    plane_pixels(m, 0, 152, 1016, 1017, 5);
    rr_vdp_render_line_wide(m, 151, 1, NULL, NULL, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
    assert(wide_big[31] == rr_vdp_colour(0x00E0));
    rr_vdp_render_line_wide(m, 152, 1, NULL, NULL, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
    assert(wide_big[31] == w.panel);
}

static void test_the_dashboard_runs_on_into_the_margins(void) {
    RrVdp *v = screen();
    RrWide w = wide_of(39, 2);
    w.dash.first = 3;
    w.dash.end = 6;
    w.dash.left_from = 64;
    w.dash.right_from = 255;
    w.dash.columns = 3;
    w.dash.fill = 5;
    v->cram[0] = 0x0222;
    v->cram[1] = 0x00E0;
    v->cram[2] = 0x000E;
    v->cram[5] = 0x0E00;
    solid_tile(v, 1, 1);
    solid_tile(v, 2, 2);
    for (int cell = 0; cell < 64; cell++) name(v, 0xF000, cell, 0, cell == 8 || cell == 31 ? 0x0001 : 0x0002);
    v->reg[18] = 0x01; /* the window covers the top cell row */
    const int scale = 2, wide = 398 * scale, edge = 39 * scale;
    uint32_t frame = rr_vdp_colour(0x00E0), rest = rr_vdp_colour(0x000E), fill = rr_vdp_colour(0x0E00);
    /* A line of the mirrors: their frame's columns, mirrored, then the fill. */
    rr_vdp_render_line_wide(v, 3, scale, NULL, NULL, NULL, NULL, NULL, NULL, &w, wide_big, wide_src);
    for (int r = 0; r < scale; r++) {
        const uint32_t *out = &wide_big[r * wide];
        for (int c = 0; c < edge; c++) {
            uint32_t want = c < 3 * scale ? frame : fill;
            assert(out[edge - 1 - c] == want && out[edge + 320 * scale + c] == want);
            assert(wide_src[r * wide + c].layer == RR_SRC_PANEL);
        }
        assert(out[edge] == rest && out[edge + 64 * scale] == frame);
    }
    /* A line of the dashboard above them: its edge pixel runs on. */
    rr_vdp_render_line_wide(v, 2, scale, NULL, NULL, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
    assert(wide_big[0] == rest && wide_big[edge - 1] == rest && wide_big[wide - 1] == rest);
    /* A window line among the wide lines, or with no wide lines at all (a menu): panels. */
    rr_vdp_render_line_wide(v, 1, scale, NULL, NULL, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
    assert(wide_big[0] == w.panel && wide_big[wide - 1] == w.panel);
    w.lines = 0;
    rr_vdp_render_line_wide(v, 3, scale, NULL, NULL, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
    assert(wide_big[0] == w.panel && wide_big[wide - 1] == w.panel);
    w.lines = 2;
    /* A line without the window is no dashboard: panels. */
    rr_vdp_render_line_wide(v, 8, scale, NULL, NULL, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
    assert(wide_big[0] == w.panel && wide_big[wide - 1] == w.panel);
    /* And with no dashboard described, panels as before. */
    w.dash.columns = 0;
    rr_vdp_render_line_wide(v, 3, scale, NULL, NULL, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
    assert(wide_big[0] == w.panel && wide_big[edge - 1] == w.panel);
}

static void test_the_plain_fast_path_is_wide_too(void) {
    RrVdp *v = margin_scene();
    RrWide w = wide_of(39, RR_SCREEN_H);
    rr_vdp_render_line_wide(v, 0, 3, NULL, NULL, NULL, NULL, NULL, NULL, &w, wide_big, wide_src);
    for (int r = 0; r < 3; r++) {
        const uint32_t *out = &wide_big[r * 398 * 3];
        assert(out[92] == rr_vdp_colour(0x0222) && out[93] == rr_vdp_colour(0x00E0) && out[95] == rr_vdp_colour(0x00E0));
        assert(out[96] == rr_vdp_colour(0x0222) && out[1098] == rr_vdp_colour(0x000E));
        assert(wide_src[r * 398 * 3 + 94].layer == RR_SRC_PLANE_A && wide_src[r * 398 * 3 + 96].layer == RR_SRC_BACKDROP);
    }
}

static void test_every_scale_and_margin_stays_in_bounds(void) {
    static const int margins[5] = {0, 1, 39, 64, 500}; /* more than the most is the most */
    static int32_t a[RR_SCALE_MAX], b[RR_SCALE_MAX];
    enum { GUARD = 64 };
    for (int scene = 0; scene < 2; scene++) {
        RrVdp *v = scene == 0 ? shifted_frame() : fine_scene(0x18000);
        for (int scale = 1; scale <= RR_SCALE_MAX; scale++) {
            for (int i = 0; i < 5; i++) {
                int margin = margins[i] > RR_WIDE_MARGIN_MAX ? RR_WIDE_MARGIN_MAX : margins[i];
                size_t count = (size_t)(RR_SCREEN_W + 2 * margin) * (size_t)scale * (size_t)scale;
                uint32_t *out = malloc((count + 2 * GUARD) * sizeof *out);
                RrPixelSource *src = malloc((count + 2 * GUARD) * sizeof *src);
                assert(out != NULL && src != NULL);
                RrWide w = wide_of(margins[i], 152);
                for (int line = 0; line < RR_SCREEN_H; line += 19) {
                    for (int r = 0; r < scale; r++) a[r] = b[r] = (int32_t)(line << 16) + r * 0x2000;
                    for (int with_shifts = 0; with_shifts < 2; with_shifts++) {
                        memset(out, 0xA5, (count + 2 * GUARD) * sizeof *out);
                        memset(src, 0xA5, (count + 2 * GUARD) * sizeof *src);
                        rr_vdp_render_line_wide(v, line, scale, with_shifts ? a : NULL, with_shifts ? b : NULL, NULL,
                                                NULL, scene == 1 ? &fine : NULL, NULL, &w, out + GUARD, src + GUARD);
                        for (int g = 0; g < GUARD; g++) {
                            assert(out[g] == 0xA5A5A5A5u && out[GUARD + count + g] == 0xA5A5A5A5u);
                            assert(src[g].layer == 0xA5 && src[GUARD + count + g].layer == 0xA5);
                        }
                        /* Every pixel was written: no colour has 0xA5 in its top byte. */
                        for (size_t k = 0; k < count; k++) assert((out[GUARD + k] >> 24) == 0 && src[GUARD + k].layer != 0xA5);
                    }
                }
                free(out);
                free(src);
            }
        }
    }
}

/* --- ground rows of a wide row ----------------------------------------- */

/* The game's two ground blocks, and the backdrop colour of a ground line. */
static RrWide ground_wide(void) {
    RrWide w = wide_of(39, RR_SCREEN_H);
    w.block[0] = (RrWideBlock){352, 672, 24, 1, 8, -352, 320};
    w.block[1] = (RrWideBlock){352, 672, 24, 0, 8, 0, 384};
    w.ground_backdrop = 62;
    return w;
}

/* A ground row on line 0 of plane `which`: the block in colour 6 with a mark
 * in colour 5 beyond either end, drawn with the road's backdrop. */
static RrVdp *ground_scene(int which) {
    RrVdp *v = wide_planes();
    v->reg[7] = 62;
    v->reg[11] = 0x03; /* a horizontal scroll value per line */
    v->cram[62] = 0x0222;
    v->cram[5] = 0x00E0;
    v->cram[6] = 0x000E;
    plane_pixels(v, which, 0, 331, 338, 5);
    plane_pixels(v, which, 0, 352, 672, 6);
    plane_pixels(v, which, 0, 686, 693, 5);
    return v;
}

static void scroll_of_line_0(RrVdp *v, int a, int b) {
    v->vram[HSCROLL] = (uint8_t)((a & 0x3FF) >> 8);
    v->vram[HSCROLL + 1] = (uint8_t)a;
    v->vram[HSCROLL + 2] = (uint8_t)((b & 0x3FF) >> 8);
    v->vram[HSCROLL + 3] = (uint8_t)b;
}

/* Line 0 at scale 1 with a 39-pixel margin: the colour at screen x. */
static uint32_t wide_x(int x) { return wide_big[x + 39]; }

static void test_nothing_shows_beyond_the_road_side_mark(void) {
    RrVdp *v = ground_scene(0);
    RrWide w = ground_wide(), plain = ground_wide();
    plain.block[0].period = plain.block[1].period = 0;
    uint32_t back = rr_vdp_colour(0x0222), mark = rr_vdp_colour(0x00E0), grass = rr_vdp_colour(0x000E);
    /* The road's left edge at screen x = 100: plane A's shift is the edge less 0x2A0. */
    int32_t a = (100 - 0x2A0) * 65536, b = 0;
    rr_vdp_render_line_wide(v, 0, 1, &a, &b, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
    assert(wide_x(99) == grass && wide_x(100) == back);              /* the block ends at the edge */
    assert(wide_x(114) == mark && wide_x(120) == mark && wide_x(121) == back); /* its mark */
    for (int x = 124; x < 359; x++) assert(wide_x(x) == back);
    assert(wide_x(-39) == grass);
    /* With the edge held at the left limit, the copy of the row 1,024 columns on
     * reaches the right margin: its mark first. Not as ground. */
    a = (-0x160 - 0x2A0) * 65536;
    rr_vdp_render_line_wide(v, 0, 1, &a, &b, NULL, NULL, NULL, NULL, &plain, wide_big, NULL);
    assert(wide_x(331) == mark && wide_x(337) == mark);
    rr_vdp_render_line_wide(v, 0, 1, &a, &b, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
    for (int x = -39; x < 359; x++) assert(wide_x(x) == back);
}

static void test_the_wrapped_copy_of_the_block_is_not_shown(void) {
    RrVdp *v = ground_scene(0);
    RrWide w = ground_wide(), plain = ground_wide();
    plain.block[0].period = plain.block[1].period = 0;
    uint32_t back = rr_vdp_colour(0x0222), grass = rr_vdp_colour(0x000E);
    int32_t a[3], b[3] = {0, 0, 0};
    for (int r = 0; r < 3; r++) a[r] = (-0x160 - 0x2A0) * 65536;
    rr_vdp_render_line_wide(v, 0, 3, a, b, NULL, NULL, NULL, NULL, &plain, wide_big, NULL);
    assert(wide_big[(352 + 39) * 3] == grass && wide_big[398 * 3 - 1] == grass);
    rr_vdp_render_line_wide(v, 0, 3, a, b, NULL, NULL, NULL, NULL, &w, wide_big, wide_src);
    for (int r = 0; r < 3; r++)
        for (int c = (320 + 39) * 3; c < 398 * 3; c++) {
            assert(wide_big[r * 398 * 3 + c] == back);
            assert(wide_src[r * 398 * 3 + c].layer == RR_SRC_BACKDROP);
        }
}

static void test_an_unshifted_line_picks_the_copy_that_fits_the_clamps(void) {
    uint32_t back = rr_vdp_colour(0x0222), mark = rr_vdp_colour(0x00E0), grass = rr_vdp_colour(0x000E);
    RrWide w = ground_wide(), plain = ground_wide();
    plain.block[0].period = plain.block[1].period = 0;
    /* Plane A at its left limit: the scroll table holds -0x160 - 0x2A0 = -1024, which is 0. */
    RrVdp *v = ground_scene(0);
    scroll_of_line_0(v, 0, 0);
    rr_vdp_render_line_wide(v, 0, 1, NULL, NULL, NULL, NULL, NULL, NULL, &plain, wide_big, NULL);
    assert(wide_x(331) == mark && wide_x(352) == grass && wide_x(358) == grass);
    rr_vdp_render_line_wide(v, 0, 1, NULL, NULL, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
    for (int x = -39; x < 359; x++) assert(wide_x(x) == back);
    /* Plane A with the edge at x = 100: the table holds 100 - 0x2A0 as ten bits. */
    scroll_of_line_0(v, 100 - 0x2A0, 0);
    rr_vdp_render_line_wide(v, 0, 1, NULL, NULL, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
    assert(wide_x(-39) == grass && wide_x(99) == grass && wide_x(100) == back && wide_x(114) == mark);
    for (int x = 124; x < 359; x++) assert(wide_x(x) == back);
    /* Plane B with the road's right edge at x = 0: the table holds 0x2A0. The block
     * covers the screen exactly; what the plane holds beyond the block's far end
     * (the other mark) is not shown. */
    v = ground_scene(1);
    scroll_of_line_0(v, 0, 0x2A0);
    rr_vdp_render_line_wide(v, 0, 1, NULL, NULL, NULL, NULL, NULL, NULL, &plain, wide_big, NULL);
    assert(wide_x(334) == mark);
    rr_vdp_render_line_wide(v, 0, 1, NULL, NULL, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
    /* (Held at the limit with nothing known beyond it, the grass runs on to the left
     * too: test_no_beyond_on_a_line_without_shifts.) */
    for (int x = -39; x < 359; x++) assert(wide_x(x) == grass);
    /* One pixel inside the limit the mark is where the plane has it. */
    scroll_of_line_0(v, 0, 0x2A0 + 1);
    rr_vdp_render_line_wide(v, 0, 1, NULL, NULL, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
    assert(wide_x(1) == grass && wide_x(0) == back);
    assert(wide_x(-20) == mark && wide_x(-14) == mark && wide_x(-13) == back && wide_x(-21) == back);
    for (int x = -39; x < -23; x++) assert(wide_x(x) == back);
    for (int x = 320; x < 359; x++) assert(wide_x(x) == grass);
}

static void test_a_row_that_fails_the_test_wraps_as_before(void) {
    static uint32_t other[WIDE_W_MAX * 2 * 2];
    RrWide w = ground_wide(), plain = ground_wide();
    plain.block[0].period = plain.block[1].period = 0;
    for (int kind = 0; kind < 3; kind++) {
        RrVdp *v = wide_planes();
        v->reg[7] = 62;
        v->cram[62] = 0x0222;
        v->cram[6] = 0x000E;
        if (kind == 0) plane_pixels(v, 0, 0, 396, 498, 6);      /* a sparse sky row */
        else if (kind == 1) plane_pixels(v, 0, 0, 0, 1024, 6);  /* a full row */
        else {                                                  /* a block with something else on the row */
            plane_pixels(v, 0, 0, 352, 672, 6);
            plane_pixels(v, 0, 0, 100, 101, 6);
        }
        for (int shift = -1024; shift <= 0; shift += 64) {
            int32_t a[2] = {shift * 65536, shift * 65536 + 0x8000}, b[2] = {0, 0};
            rr_vdp_render_line_wide(v, 0, 2, a, b, NULL, NULL, NULL, NULL, &plain, other, NULL);
            rr_vdp_render_line_wide(v, 0, 2, a, b, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
            assert(memcmp(wide_big, other, 398 * 2 * 2 * sizeof *other) == 0);
        }
    }
}

static void test_the_centre_is_unchanged(void) {
    RrWide w = ground_wide();
    for (int which = 0; which < 2; which++) {
        RrVdp *v = ground_scene(which);
        for (int edge = -400; edge <= 400; edge += 16) {
            /* Shifts inside and outside the game's limits, whole and fractional. */
            int32_t a[2], b[2];
            a[0] = (edge - 0x2A0) * 65536;
            a[1] = a[0] + 0x4000;
            b[0] = (edge + 0x2A0) * 65536;
            b[1] = b[0] - 0x4000;
            rr_vdp_render_line_fine(v, 0, 2, a, b, NULL, big);
            rr_vdp_render_line_wide(v, 0, 2, a, b, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
            for (int r = 0; r < 2; r++)
                assert(memcmp(&wide_big[r * 398 * 2 + 39 * 2], &big[r * RR_SCREEN_W * 2],
                              RR_SCREEN_W * 2 * sizeof *big) == 0);
            scroll_of_line_0(v, edge - 0x2A0, edge + 0x2A0);
            rr_vdp_render_line_fine(v, 0, 2, NULL, NULL, NULL, big);
            rr_vdp_render_line_wide(v, 0, 2, NULL, NULL, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
            for (int r = 0; r < 2; r++)
                assert(memcmp(&wide_big[r * 398 * 2 + 39 * 2], &big[r * RR_SCREEN_W * 2],
                              RR_SCREEN_W * 2 * sizeof *big) == 0);
        }
    }
}

static void test_a_block_row_on_a_sky_backdrop_line_is_not_ground(void) {
    static uint32_t other[WIDE_W_MAX];
    RrVdp *v = ground_scene(0);
    RrWide w = ground_wide(), plain = ground_wide();
    plain.block[0].period = plain.block[1].period = 0;
    v->reg[7] = 63; /* the sky's backdrop: Tennessee's sky has rows shaped like the block */
    v->cram[63] = 0x0E00;
    int32_t a = (-0x160 - 0x2A0) * 65536, b = 0;
    rr_vdp_render_line_wide(v, 0, 1, &a, &b, NULL, NULL, NULL, NULL, &plain, other, NULL);
    rr_vdp_render_line_wide(v, 0, 1, &a, &b, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
    assert(memcmp(wide_big, other, 398 * sizeof *other) == 0);
    assert(wide_x(352) == rr_vdp_colour(0x000E)); /* the plane as it is, wrapped */
    /* A backdrop of -1 asks for no particular colour. */
    w.ground_backdrop = -1;
    rr_vdp_render_line_wide(v, 0, 1, &a, &b, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
    assert(wide_x(352) == rr_vdp_colour(0x0E00));
}

static void test_a_row_with_a_scroll_per_column_is_not_ground(void) {
    static uint32_t other[WIDE_W_MAX];
    RrVdp *v = ground_scene(0);
    RrWide w = ground_wide(), plain = ground_wide();
    plain.block[0].period = plain.block[1].period = 0;
    v->reg[11] |= 0x04;
    int32_t a = (-0x160 - 0x2A0) * 65536, b = 0;
    rr_vdp_render_line_wide(v, 0, 1, &a, &b, NULL, NULL, NULL, NULL, &plain, other, NULL);
    rr_vdp_render_line_wide(v, 0, 1, &a, &b, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
    assert(memcmp(wide_big, other, 398 * sizeof *other) == 0);
}

/* --- the grass continued, and the edge beyond the clamp ----------------- */

/* A ground row on line 0 of plane `which` whose block repeats every 8
 * columns in colours 1 to 8, with a mark in colour 9 beyond either end. */
static RrVdp *pattern_scene(int which) {
    RrVdp *v = wide_planes();
    v->reg[7] = 62;
    v->reg[11] = 0x03;
    v->cram[62] = 0x0222;
    for (int i = 1; i <= 8; i++) v->cram[i] = (uint16_t)(i << 1 | (8 - i) << 5);
    v->cram[9] = 0x0E00;
    for (int q = 352; q < 672; q++) plane_pixels(v, which, 0, q, q + 1, 1 + q % 8);
    plane_pixels(v, which, 0, 331, 338, 9);
    plane_pixels(v, which, 0, 686, 693, 9);
    return v;
}

static uint32_t pattern(const RrVdp *v, int phase) { return rr_vdp_colour(v->cram[1 + phase]); }

static void test_grass_continues_on_the_grass_side(void) {
    RrWide w = ground_wide();
    int32_t zero = 0;
    /* Plane A, the road's left edge at the screen's right edge: the block covers the
     * screen and the left margin is its grass side. Screen x shows column x + 352. */
    RrVdp *v = pattern_scene(0);
    int32_t a = (320 - 0x2A0) * 65536, b = 0;
    static const int periods[3] = {8, 4, 1};
    for (int i = 0; i < 3; i++) {
        w.block[0].period = (uint8_t)periods[i];
        rr_vdp_render_line_wide(v, 0, 1, &a, &b, &zero, &zero, NULL, NULL, &w, wide_big, wide_src);
        for (int x = -39; x < 0; x++) {
            assert(wide_x(x) == pattern(v, ((x + 352) % 8) % periods[i]));
            assert(wide_src[x + 39].layer == RR_SRC_PLANE_A);
        }
        assert(wide_x(-30) == pattern(v, (320 - 350 + 352) % 8 % periods[i])); /* x = L - 350 */
        assert(wide_x(0) == pattern(v, 0) && wide_x(7) == pattern(v, 7));
    }
    /* Plane B, the road's right edge at the screen's left edge: the right margin is
     * its grass side. Screen x shows column x + 352 again. */
    v = pattern_scene(1);
    a = 0;
    b = 0x2A0 * 65536;
    for (int i = 0; i < 3; i++) {
        w.block[1].period = (uint8_t)periods[i];
        rr_vdp_render_line_wide(v, 0, 1, &a, &b, &zero, &zero, NULL, NULL, &w, wide_big, NULL);
        for (int x = 320; x < 359; x++)
            assert(wide_x(x) == pattern(v, periods[i] == 8 ? (x + 352) % 8 : 8 - periods[i] + (x + 352) % periods[i]));
    }
}

static void test_the_grass_side_mark_is_not_drawn(void) {
    RrWide w = ground_wide();
    uint32_t mark = rr_vdp_colour(0x0E00);
    int32_t zero = 0;
    RrVdp *v = pattern_scene(0);
    int32_t a = (320 - 0x2A0) * 65536, b = 0;
    rr_vdp_render_line_wide(v, 0, 1, &a, &b, &zero, &zero, NULL, NULL, &w, wide_big, NULL);
    for (int x = -39; x < 0; x++) assert(wide_x(x) != mark); /* columns 331..337 are at x = -21..-15 */
    assert(wide_x(334) == mark);                             /* the road-side one is */
    v = pattern_scene(1);
    a = 0;
    b = 0x2A0 * 65536;
    rr_vdp_render_line_wide(v, 0, 1, &a, &b, &zero, &zero, NULL, NULL, &w, wide_big, NULL);
    for (int x = 320; x < 359; x++) assert(wide_x(x) != mark);
    assert(wide_x(-21) == mark);
}

static void test_beyond_moves_the_mark_and_fills_with_grass(void) {
    RrWide w = ground_wide();
    int32_t zero[2] = {0, 0};
    /* Plane A held at the limit, x = 320, with the edge really 20 pixels further right. */
    RrVdp *v = pattern_scene(0);
    uint32_t back = rr_vdp_colour(0x0222), mark = rr_vdp_colour(0x0E00);
    int32_t a[2] = {(320 - 0x2A0) * 65536, (320 - 0x2A0) * 65536}, b[2] = {0, 0};
    int32_t beyond[2] = {20 * 65536, 20 * 65536 + 0x8000};
    rr_vdp_render_line_wide(v, 0, 1, a, b, beyond, zero, NULL, NULL, &w, wide_big, wide_src);
    for (int x = 320; x < 340; x++) assert(wide_x(x) == pattern(v, x % 8)); /* the grass runs on in step */
    for (int x = 340; x < 354; x++) assert(wide_x(x) == back);              /* the gap before the mark's pixels */
    for (int x = 354; x < 359; x++) assert(wide_x(x) == mark);              /* columns 686.. moved 20 right */
    assert(wide_src[339 + 39].layer == RR_SRC_PLANE_A && wide_src[340 + 39].layer == RR_SRC_BACKDROP);
    /* At scale 2 the second row's edge is 20.5 pixels out: one output pixel more. */
    rr_vdp_render_line_wide(v, 0, 2, a, b, beyond, zero, NULL, NULL, &w, wide_big, NULL);
    const uint32_t *row0 = wide_big, *row1 = &wide_big[398 * 2];
    assert(row0[(339 + 39) * 2 + 1] != back && row0[(340 + 39) * 2] == back);
    assert(row1[(340 + 39) * 2] != back && row1[(340 + 39) * 2 + 1] == back);
    assert(row0[(354 + 39) * 2] == mark && row1[(354 + 39) * 2] == back && row1[(354 + 39) * 2 + 1] == mark);
    /* Plane B held at its limit, x = 0, with the edge really 20 pixels further left. */
    v = pattern_scene(1);
    a[0] = 0;
    b[0] = 0x2A0 * 65536;
    beyond[0] = -20 * 65536;
    rr_vdp_render_line_wide(v, 0, 1, a, b, zero, beyond, NULL, NULL, &w, wide_big, NULL);
    for (int x = -20; x < 0; x++) assert(wide_x(x) == pattern(v, (x + 352) % 8));
    for (int x = -34; x < -20; x++) assert(wide_x(x) == back);
    for (int x = -39; x < -34; x++) assert(wide_x(x) == mark);
    /* No excess: the mark where the plane has it. */
    rr_vdp_render_line_wide(v, 0, 1, a, b, zero, zero, NULL, NULL, &w, wide_big, NULL);
    assert(wide_x(-1) == back && wide_x(-15) == mark && wide_x(-21) == mark && wide_x(-22) == back);
}

static void test_no_beyond_on_a_line_without_shifts(void) {
    RrWide w = ground_wide();
    uint32_t back = rr_vdp_colour(0x0222), mark = rr_vdp_colour(0x0E00);
    int32_t far = 20 * 65536;
    /* Plane A at the limit by the scroll table alone: where the edge really is is not
     * known, so the grass runs on through the margin and no mark is drawn. A `beyond`
     * handed in with no shift is not used. */
    RrVdp *v = pattern_scene(0);
    scroll_of_line_0(v, 320 - 0x2A0, 0);
    for (int given = 0; given < 2; given++) {
        rr_vdp_render_line_wide(v, 0, 1, NULL, NULL, given ? &far : NULL, given ? &far : NULL, NULL, NULL, &w,
                                wide_big, NULL);
        for (int x = 320; x < 359; x++) assert(wide_x(x) == pattern(v, x % 8));
    }
    /* The same with a shift but no `beyond`. */
    int32_t a = (320 - 0x2A0) * 65536, b = 0;
    rr_vdp_render_line_wide(v, 0, 1, &a, &b, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
    for (int x = 320; x < 359; x++) assert(wide_x(x) == pattern(v, x % 8));
    /* Away from the limit the mark is where the game has it. */
    scroll_of_line_0(v, 100 - 0x2A0, 0);
    rr_vdp_render_line_wide(v, 0, 1, NULL, NULL, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
    assert(wide_x(99) == pattern(v, (99 - 100 + 672) % 8) && wide_x(100) == back && wide_x(114) == mark);
    /* Plane B at its limit by the table alone: grass through the left margin. */
    v = pattern_scene(1);
    scroll_of_line_0(v, 0, 0x2A0);
    rr_vdp_render_line_wide(v, 0, 1, NULL, NULL, NULL, NULL, NULL, NULL, &w, wide_big, NULL);
    for (int x = -39; x < 0; x++) assert(wide_x(x) == pattern(v, (x + 352) % 8));
}

static void test_a_block_wholly_off_the_view_gives_grass_across_its_side(void) {
    RrWide w = ground_wide();
    int32_t zero = 0, most = 256 * 65536, least = -256 * 65536;
    /* The left edge of the road far beyond the right margin: grass right across. */
    RrVdp *v = pattern_scene(0);
    int32_t a = (320 - 0x2A0) * 65536, b = 0;
    rr_vdp_render_line_wide(v, 0, 1, &a, &b, &most, &zero, NULL, NULL, &w, wide_big, NULL);
    for (int x = -39; x < 359; x++) assert(wide_x(x) == pattern(v, (x + 352) % 8));
    /* The right edge far beyond the left margin. */
    v = pattern_scene(1);
    a = 0;
    b = 0x2A0 * 65536;
    rr_vdp_render_line_wide(v, 0, 1, &a, &b, &zero, &least, NULL, NULL, &w, wide_big, NULL);
    for (int x = -39; x < 359; x++) assert(wide_x(x) == pattern(v, (x + 352) % 8));
}

static void test_the_centre_is_unchanged_by_continuation(void) {
    RrWide w = ground_wide();
    for (int which = 0; which < 2; which++) {
        RrVdp *v = pattern_scene(which);
        for (int edge = -400; edge <= 400; edge += 16) {
            for (int excess = -300; excess <= 300; excess += 150) {
                int32_t a[2], b[2], beyond[2] = {excess * 65536, excess * 65536 + 0x4000};
                a[0] = (edge - 0x2A0) * 65536;
                a[1] = a[0] + 0x4000;
                b[0] = (edge + 0x2A0) * 65536;
                b[1] = b[0] - 0x4000;
                rr_vdp_render_line_fine(v, 0, 2, a, b, NULL, big);
                rr_vdp_render_line_wide(v, 0, 2, a, b, beyond, beyond, NULL, NULL, &w, wide_big, NULL);
                for (int r = 0; r < 2; r++)
                    assert(memcmp(&wide_big[r * 398 * 2 + 39 * 2], &big[r * RR_SCREEN_W * 2],
                                  RR_SCREEN_W * 2 * sizeof *big) == 0);
            }
        }
    }
}

static void test_a_strip_is_drawn_in_the_margin(void) {
    RrVdp *v = fine_scene(0x10000);
    RrWide w = wide_of(39, RR_SCREEN_H);
    uint32_t backdrop = rr_vdp_colour(v->cram[0]);
    v->cram[0x3D] = 0x0E0E;
    uint32_t strip = rr_vdp_colour(v->cram[0x3D]);
    memset(&strips, 0, sizeof strips);
    strips.scale = 3;
    strips.colour = 0x3D;
    strips.from[24] = -60; /* from 20 pixels left of the screen */
    strips.to[24] = 30;
    strips.from[25] = 950; /* to nearly 17 pixels right of it */
    strips.to[25] = 1010;
    strips.from[26] = -5000;
    strips.to[26] = 5000;
    fine.strips = &strips;
    rr_vdp_render_line_wide(v, 8, 3, NULL, NULL, NULL, NULL, &fine, NULL, &w, wide_big, wide_src);
    const uint32_t *row0 = wide_big, *row1 = &wide_big[398 * 3], *row2 = &wide_big[2 * 398 * 3];
    /* Output column 117 is the screen's left edge. */
    assert(row0[56] == backdrop && row0[57] == strip && row0[117] == strip && row0[146] == strip && row0[147] == backdrop);
    assert(wide_src[57].layer == RR_SRC_SPRITE && wide_src[56].layer == RR_SRC_BACKDROP);
    assert(row1[1066] == backdrop && row1[1067] == strip && row1[1126] == strip && row1[1127] == backdrop);
    assert(row2[0] == strip && row2[398 * 3 - 1] == strip);
    /* The picture without margins has the same strip, cut at its sides. */
    rr_vdp_render_line_fine(v, 8, 3, NULL, NULL, &fine, big);
    for (int r = 0; r < 3; r++)
        assert(memcmp(&wide_big[r * 398 * 3 + 117], &big[r * RR_SCREEN_W * 3], RR_SCREEN_W * 3 * sizeof *big) == 0);
    assert(big[0] == strip && big[29] == strip && big[30] == backdrop);
    /* On a line that lies between panels the panels stay panels. */
    w.lines = 8;
    rr_vdp_render_line_wide(v, 8, 3, NULL, NULL, NULL, NULL, &fine, NULL, &w, wide_big, NULL);
    assert(wide_big[57] == w.panel && wide_big[116] == w.panel && wide_big[117] == strip);
    fine.strips = NULL;
}

/* --- the margin sprite layer -------------------------------------------- */

/* Art of 12 columns and 4 rows, column c in colour c + 1 on every row. */
static const uint8_t art12[4 * 7] = {
    0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0,
    0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0,
};
static RrFineSprites margin_fine;
static RrFinePicture margin_pictures[RR_SPRITES_MAX];
static uint8_t margin_table[RR_SPRITES_MAX * 8];
static int margin_count;

/* A screen with no sprites of its own and 12 different colours; an empty margin set. */
static RrVdp *margin_sprite_scene(void) {
    RrVdp *v = screen();
    v->cram[0] = 0x0222;
    for (int i = 1; i < 16; i++) v->cram[i] = (uint16_t)(((i & 3) * 2) << 1 | (((i >> 2) & 3) * 2) << 5 | 0x800);
    memset(&margin_fine, 0, sizeof margin_fine);
    memset(margin_table, 0, sizeof margin_table);
    margin_fine.pictures = margin_pictures;
    margin_fine.table = margin_table;
    for (int i = 0; i < RR_SPRITES_MAX; i++) margin_fine.picture[i] = -1;
    margin_count = 0;
    return v;
}

/* One more margin picture of art12 with its left end (mirrored: one past its right
 * end) at x on lines 8 to 11, in a sprite entry `cells` cells wide starting at entry_x. */
static RrFinePicture *margin_picture_at(int x, int mirrored, int entry_x, int cells, uint16_t attr) {
    int i = margin_count++;
    margin_pictures[i] = (RrFinePicture){.rows = art12, .width = 12, .height = 4, .stride = 7, .size = 0x10000,
                                         .x = (int16_t)x, .y = 8, .mirrored = (uint8_t)mirrored, .columns = 12,
                                         .rows_shown = 4};
    if (i > 0) margin_table[8 * (i - 1) + 3] = (uint8_t)i;
    put_entry(margin_table, i, entry_x + 128, 8 + 128, (cells - 1) << 2, 0, attr);
    margin_fine.picture[i] = (int16_t)i;
    return &margin_pictures[i];
}

static uint32_t art_colour(const RrVdp *v, int column) { return rr_vdp_colour(v->cram[column + 1]); }

static void wide_line_8(const RrVdp *v, const RrFineSprites *centre, const RrWide *w) {
    rr_vdp_render_line_wide(v, 8, 1, NULL, NULL, NULL, NULL, centre, &margin_fine, w, wide_big, wide_src);
}

static void test_margin_sprites_draw_only_in_the_margins(void) {
    RrVdp *v = margin_sprite_scene();
    RrWide w = wide_of(39, RR_SCREEN_H);
    uint32_t back = rr_vdp_colour(0x0222);
    margin_picture_at(-20, 0, -20, 2, 0);
    margin_picture_at(100, 0, 100, 2, 0);
    margin_picture_at(330, 0, 330, 2, 0);
    margin_picture_at(-6, 0, -6, 2, 0);  /* across the screen's left edge */
    wide_line_8(v, NULL, &w);
    for (int c = 0; c < 12; c++) assert(wide_x(-20 + c) == art_colour(v, c) && wide_x(330 + c) == art_colour(v, c));
    assert(wide_x(-21) == back && wide_x(-8) == back && wide_x(342) == back);
    for (int x = 0; x < RR_SCREEN_W; x++) assert(wide_x(x) == back); /* nothing on the screen itself */
    for (int c = 0; c < 6; c++) assert(wide_x(-6 + c) == art_colour(v, c));
    assert(wide_src[-20 + 39].layer == RR_SRC_SPRITE && (wide_src[-20 + 39].detail & RR_MARGIN_SPRITE));
    assert((wide_src[330 + 39].detail & ~RR_MARGIN_SPRITE) == 2);
    /* Enlarged, every row of the line. */
    rr_vdp_render_line_wide(v, 8, 3, NULL, NULL, NULL, NULL, NULL, &margin_fine, &w, wide_big, NULL);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 36; c++) assert(wide_big[r * 398 * 3 + (39 - 20) * 3 + c] == art_colour(v, c / 3));
    /* With no margin there is no margin layer. */
    rr_vdp_render_line_wide(v, 8, 1, NULL, NULL, NULL, NULL, NULL, &margin_fine, NULL, wide_big, NULL);
    for (int x = 0; x < RR_SCREEN_W; x++) assert(wide_big[x] == back);
}

static void test_a_crowded_margin_does_not_cost_the_centre_a_sprite(void) {
    static uint32_t alone[WIDE_W_MAX];
    RrVdp *v = fine_scene(0x10000); /* one sprite of the screen's own at (16, 8) */
    RrWide w = wide_of(39, RR_SCREEN_H);
    rr_vdp_render_line_wide(v, 8, 1, NULL, NULL, NULL, NULL, &fine, NULL, &w, alone, NULL);
    memset(&margin_fine, 0, sizeof margin_fine);
    memset(margin_table, 0, sizeof margin_table);
    margin_fine.pictures = margin_pictures;
    margin_fine.table = margin_table;
    for (int i = 0; i < RR_SPRITES_MAX; i++) margin_fine.picture[i] = -1;
    margin_count = 0;
    for (int i = 0; i < 21; i++) margin_picture_at(-36 + i, 0, -36 + i, 1, 0); /* one more than a line holds */
    wide_line_8(v, &fine, &w);
    assert(memcmp(&wide_big[39], &alone[39], RR_SCREEN_W * sizeof *alone) == 0);
    assert(wide_big[39 + 16] == picture_colour(v, 0, 0));
    /* The margin has its own twenty: the first twenty entries are drawn, the last is not. */
    assert(wide_x(-36) == rr_vdp_colour(v->cram[1]) && wide_x(-36 + 19 + 7) != rr_vdp_colour(v->cram[0]));
    assert(wide_x(-36 + 20 + 7) == rr_vdp_colour(v->cram[0]));
}

static void test_a_cut_picture_continues_without_a_seam(void) {
    RrVdp *v = margin_sprite_scene();
    RrWide w = wide_of(39, RR_SCREEN_H);
    /* The screen's own picture, cut at the left: art columns 3 to 11 from x = 0. */
    static RrFinePicture cut;
    static RrFineSprites centre;
    cut = (RrFinePicture){.rows = art12, .width = 12, .height = 4, .stride = 7, .size = 0x10000, .x = 0, .y = 8,
                          .first_column = 3, .columns = 9, .rows_shown = 4};
    memset(&centre, 0, sizeof centre);
    centre.pictures = &cut;
    for (int i = 0; i < RR_SPRITES_MAX; i++) centre.picture[i] = -1;
    centre.picture[0] = 0;
    sprite(v, 0, 128, 136, 0x04, 0, 0x0003); /* a 2 x 1-cell sprite at (0, 8) */
    /* The same picture uncut in the margin set. */
    margin_picture_at(-3, 0, -3, 2, 0);
    wide_line_8(v, &centre, &w);
    for (int x = -3; x <= 8; x++) assert(wide_x(x) == art_colour(v, x + 3));
    assert(wide_x(-4) == rr_vdp_colour(0x0222));
    assert(wide_src[39 - 1].detail & RR_MARGIN_SPRITE);
    assert(!(wide_src[39].detail & RR_MARGIN_SPRITE)); /* x = 0 is the screen's own sprite */
}

static void test_a_mirrored_right_cut_continues_without_a_gap(void) {
    RrVdp *v = margin_sprite_scene();
    RrWide w = wide_of(39, RR_SCREEN_H);
    /* The screen's own picture, mirrored, stopped by the game at 316: its right-hand
     * end really is at 324, so art columns 8 to 11 stand at 315 down to 312. */
    static RrFinePicture cut;
    static RrFineSprites centre;
    cut = (RrFinePicture){.rows = art12, .width = 12, .height = 4, .stride = 7, .size = 0x10000, .x = 316, .y = 8,
                          .mirrored = 1, .first_column = 8, .columns = 4, .rows_shown = 4};
    memset(&centre, 0, sizeof centre);
    centre.pictures = &cut;
    for (int i = 0; i < RR_SPRITES_MAX; i++) centre.picture[i] = -1;
    centre.picture[0] = 0;
    sprite(v, 0, 308 + 128, 136, 0x00, 0, 0x0803); /* a 1 x 1-cell sprite at (308, 8) */
    margin_picture_at(324, 1, 308, 2, 0x0800);
    wide_line_8(v, &centre, &w);
    for (int x = 312; x < 324; x++) assert(wide_x(x) == art_colour(v, 323 - x));
    assert(wide_x(324) == rr_vdp_colour(0x0222));
    /* 312 to 315 are the screen's own sprite, 316 to 319 the seam filled from the margin set. */
    assert(!(wide_src[39 + 315].detail & RR_MARGIN_SPRITE) && (wide_src[39 + 316].detail & RR_MARGIN_SPRITE));
    /* The middle differs from the picture without margins in those four columns only. */
    rr_vdp_render_line_fine(v, 8, 1, NULL, NULL, &centre, big);
    for (int x = 0; x < RR_SCREEN_W; x++) assert((wide_x(x) != big[x]) == (x >= 316));
}

/* As its pairs of pixels fall, the game stops a mirrored picture at 315 as often as at 316. */
static void test_a_mirrored_right_cut_that_ends_at_315_has_no_slit(void) {
    RrVdp *v = margin_sprite_scene();
    RrWide w = wide_of(39, RR_SCREEN_H);
    static RrFinePicture cut;
    static RrFineSprites centre;
    /* Its right-hand end really is at 324: art columns 9 to 11 stand at 314 down to 312. */
    cut = (RrFinePicture){.rows = art12, .width = 12, .height = 4, .stride = 7, .size = 0x10000, .x = 315, .y = 8,
                          .mirrored = 1, .first_column = 9, .columns = 3, .rows_shown = 4};
    memset(&centre, 0, sizeof centre);
    centre.pictures = &cut;
    for (int i = 0; i < RR_SPRITES_MAX; i++) centre.picture[i] = -1;
    centre.picture[0] = 0;
    sprite(v, 0, 307 + 128, 136, 0x00, 0, 0x0803); /* a 1 x 1-cell sprite at (307, 8) */
    margin_picture_at(324, 1, 308, 2, 0x0800);
    wide_line_8(v, &centre, &w);
    for (int x = 312; x < 324; x++) assert(wide_x(x) == art_colour(v, 323 - x));
    assert(!(wide_src[39 + 314].detail & RR_MARGIN_SPRITE) && (wide_src[39 + 315].detail & RR_MARGIN_SPRITE));
    /* Column 314 is never the margin set's, even where the screen shows nothing there. */
    sprite(v, 0, 128 + 100, 136, 0x00, 0, 0x0803);
    wide_line_8(v, &centre, &w);
    assert(wide_x(314) == rr_vdp_colour(0x0222) && wide_x(315) == art_colour(v, 8));
}

static void test_the_seam_fill_never_covers_a_centre_object(void) {
    RrVdp *v = margin_sprite_scene();
    RrWide w = wide_of(39, RR_SCREEN_H);
    static RrFineSprites centre;
    memset(&centre, 0, sizeof centre);
    for (int i = 0; i < RR_SPRITES_MAX; i++) centre.picture[i] = -1;
    /* A sprite of the screen's own on 312 to 319, of a tile with one see-through column. */
    solid_tile(v, 3, 13);
    for (int y = 0; y < 8; y++) tile_pixel(v, 3, 6, y, 0);
    sprite(v, 0, 312 + 128, 136, 0x00, 0, 0x0003);
    margin_picture_at(310, 0, 310, 2, 0); /* art columns 6 to 9 on 316 to 319 */
    static RrStrips some;
    memset(&some, 0, sizeof some);
    wide_line_8(v, &centre, &w);
    uint32_t own = rr_vdp_colour(v->cram[13]);
    assert(wide_x(316) == own && wide_x(317) == own && wide_x(319) == own);
    assert(wide_x(318) == art_colour(v, 8)); /* through the hole */
    assert(wide_x(312) == own && wide_x(320) == art_colour(v, 10));
    /* A strip is not an object: the seam is filled over it. */
    v->cram[0x3D] = 0x0E0E;
    some.scale = 1;
    some.colour = 0x3D;
    some.from[8] = 300;
    some.to[8] = 320;
    centre.strips = &some;
    sprite(v, 0, 128 + 100, 136, 0x00, 0, 0x0003); /* the screen's sprite moved away */
    wide_line_8(v, &centre, &w);
    assert(wide_x(314) == rr_vdp_colour(0x0E0E) && wide_x(315) == art_colour(v, 5) && wide_x(319) == art_colour(v, 9));
}

static void test_margin_sprites_are_not_drawn_below_the_wide_lines(void) {
    RrVdp *v = margin_sprite_scene();
    RrWide w = wide_of(39, 8);
    margin_picture_at(-20, 0, -20, 2, 0);
    margin_picture_at(310, 0, 310, 2, 0);
    wide_line_8(v, NULL, &w);
    for (int x = -39; x < 0; x++) assert(wide_x(x) == w.panel && wide_x(359 + x) == w.panel);
    for (int x = 0; x < RR_SCREEN_W; x++) assert(wide_x(x) == rr_vdp_colour(0x0222)); /* no seam fill either */
}

static void test_soft_margin_pictures(void) {
    static uint32_t on_screen[WIDE_W_MAX * 3 * 3], hard[WIDE_W_MAX * 3 * 3];
    RrVdp *v = margin_sprite_scene();
    RrWide w = wide_of(39, RR_SCREEN_H);
    /* The same half-size picture as a picture of the screen's own at x = 100 and in the margin at -20. */
    static RrFinePicture half;
    static RrFineSprites centre;
    half = (RrFinePicture){.rows = art12, .width = 12, .height = 4, .stride = 7, .size = 0x8000, .x = 100, .y = 8,
                           .columns = 6, .rows_shown = 2};
    memset(&centre, 0, sizeof centre);
    centre.pictures = &half;
    for (int i = 0; i < RR_SPRITES_MAX; i++) centre.picture[i] = -1;
    centre.picture[0] = 0;
    centre.soft = 1;
    sprite(v, 0, 100 + 128, 136, 0x00, 0, 0x0003);
    rr_vdp_render_line_wide(v, 8, 3, NULL, NULL, NULL, NULL, &centre, NULL, &w, on_screen, NULL);
    RrFinePicture *p = margin_picture_at(-20, 0, -20, 1, 0);
    p->size = 0x8000;
    p->columns = 6;
    p->rows_shown = 2;
    rr_vdp_render_line_wide(v, 8, 3, NULL, NULL, NULL, NULL, NULL, &margin_fine, &w, hard, NULL);
    margin_fine.soft = 1;
    rr_vdp_render_line_wide(v, 8, 3, NULL, NULL, NULL, NULL, NULL, &margin_fine, &w, wide_big, NULL);
    int softened = 0;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 18; c++) {
            uint32_t soft = wide_big[r * 398 * 3 + (39 - 20) * 3 + c];
            assert(soft == on_screen[r * 398 * 3 + (39 + 100) * 3 + c]);
            softened += soft != hard[r * 398 * 3 + (39 - 20) * 3 + c];
        }
    assert(softened > 0);
}

static void test_margin_sprites_keep_plane_priority(void) {
    RrVdp *v = margin_sprite_scene();
    RrWide w = wide_of(39, RR_SCREEN_H);
    v->cram[14] = 0x0EEE;
    solid_tile(v, 9, 14);
    margin_picture_at(330, 0, 330, 2, 0);
    /* Plane A's cell at columns 328 to 335 of line 8, low priority: behind the sprite. */
    name(v, PLANE_A, 41, 1, 0x0009);
    wide_line_8(v, NULL, &w);
    assert(wide_x(329) == rr_vdp_colour(0x0EEE) && wide_x(330) == art_colour(v, 0) && wide_x(335) == art_colour(v, 5));
    /* High priority: in front of a low-priority sprite... */
    name(v, PLANE_A, 41, 1, 0x8009);
    wide_line_8(v, NULL, &w);
    assert(wide_x(330) == rr_vdp_colour(0x0EEE) && wide_x(336) == art_colour(v, 6));
    /* ...and behind a high-priority one. */
    margin_table[4] |= 0x80;
    wide_line_8(v, NULL, &w);
    assert(wide_x(330) == art_colour(v, 0));
}

int main(void) {
    test_runs_of_a_block_with_two_marks();
    test_runs_follow_the_vertical_scroll();
    test_a_full_row_is_one_run_0_to_1024();
    test_an_empty_row_has_no_runs();
    test_more_runs_than_room_says_so();
    test_a_plane_row_as_pixels();
    test_the_narrow_picture_is_what_it_was();
    test_margin_0_is_render_line_fine();
    test_the_centre_of_a_wide_row_is_the_narrow_row();
    test_margins_show_the_plane_beyond_the_edge();
    test_a_fractional_shift_carries_into_the_margins();
    test_margins_keep_the_edge_columns_vertical_scroll();
    test_a_window_line_is_the_original_between_panels();
    test_lines_from_wide_lines_down_are_the_original_between_panels();
    test_the_dashboard_runs_on_into_the_margins();
    test_the_plain_fast_path_is_wide_too();
    test_every_scale_and_margin_stays_in_bounds();
    test_nothing_shows_beyond_the_road_side_mark();
    test_the_wrapped_copy_of_the_block_is_not_shown();
    test_an_unshifted_line_picks_the_copy_that_fits_the_clamps();
    test_a_row_that_fails_the_test_wraps_as_before();
    test_the_centre_is_unchanged();
    test_a_block_row_on_a_sky_backdrop_line_is_not_ground();
    test_a_row_with_a_scroll_per_column_is_not_ground();
    test_grass_continues_on_the_grass_side();
    test_the_grass_side_mark_is_not_drawn();
    test_beyond_moves_the_mark_and_fills_with_grass();
    test_no_beyond_on_a_line_without_shifts();
    test_a_block_wholly_off_the_view_gives_grass_across_its_side();
    test_the_centre_is_unchanged_by_continuation();
    test_a_strip_is_drawn_in_the_margin();
    test_margin_sprites_draw_only_in_the_margins();
    test_a_crowded_margin_does_not_cost_the_centre_a_sprite();
    test_a_cut_picture_continues_without_a_seam();
    test_a_mirrored_right_cut_continues_without_a_gap();
    test_a_mirrored_right_cut_that_ends_at_315_has_no_slit();
    test_the_seam_fill_never_covers_a_centre_object();
    test_margin_sprites_are_not_drawn_below_the_wide_lines();
    test_soft_margin_pictures();
    test_margin_sprites_keep_plane_priority();
    test_backdrop_fills_a_blank_display();
    test_display_disabled_shows_the_backdrop();
    test_colour_channels();
    test_plane_a_tile_pixel();
    test_tile_palette_line();
    test_tile_flips();
    test_horizontal_scroll_full_screen();
    test_vertical_scroll();
    test_high_priority_plane_b_over_low_priority_plane_a();
    test_sprite_position_and_size();
    test_sprite_tiles_run_down_columns();
    test_first_sprite_in_the_list_is_in_front();
    test_sprite_link_cycle_terminates();
    test_unsupported_video_modes_render_magenta();
    test_source_colour_equals_the_picture();
    test_fine_null_is_render_line_scaled();
    test_fine_picture_at_natural_size_repeats_each_pixel();
    test_fine_picture_at_half_size_shows_every_source_pixel();
    test_fine_picture_at_double_size_gives_each_pixel_equal_room();
    test_fine_picture_at_an_odd_size_keeps_pixels_within_one_output_pixel_of_even();
    test_fine_picture_mirrored_runs_right_to_left();
    test_fine_first_column_and_first_row_skip_into_the_picture();
    test_fine_columns_and_rows_shown_end_the_picture();
    test_fine_dx_moves_a_copy();
    test_fine_sprite_keeps_order_priority_and_colour_set();
    test_soft_half_size_picture_averages_four_pixels();
    test_soft_leaves_natural_and_larger_pictures_alone();
    test_soft_edge_follows_how_much_of_the_pixel_is_picture();
    test_soft_keeps_order_priority_and_colour_set();
    test_strips_draw_behind_sprites_and_under_high_priority_planes();
    test_a_skipped_entry_is_not_drawn_but_still_counts();
    test_a_skipped_entry_takes_its_pixels_from_the_lines_320();
    test_a_skipped_entry_at_x_0_still_hides_the_rest();
    test_no_strips_and_no_skips_is_todays_picture();
    test_fraction_moves_a_fine_picture_by_output_pixels();
    test_fraction_moves_a_mirrored_picture_right();
    test_zero_fraction_changes_nothing();
    test_fraction_is_not_cut_at_the_entry_edge();
    test_supplied_sprite_table_replaces_video_memory();
    test_entry_without_picture_or_with_size_zero_is_its_tiles();
    test_fine_sprites_with_shifted_planes();
    test_fine_sprites_obey_the_per_line_limits();
    test_vram_writes_counts_every_kind_of_write();
    test_line_state_reports_registers_and_scroll();
    test_scale_1_without_shifts_is_the_original_line();
    test_scaled_without_shifts_repeats_each_pixel();
    test_integer_shifts_sample_back_to_the_original();
    test_fractional_shift_moves_an_edge_one_output_pixel_per_row();
    test_clamped_and_missing_lines_sample_back_to_the_original();
    test_column_vertical_scroll_follows_the_horizontal_scroll();
    test_sprite_vertical_position_is_nine_bits();
    test_window_replaces_plane_a();
    test_shadow_highlight_renders_magenta();
    test_write_ppm();
    test_version_register();
    test_data_port_write_reaches_the_video_chip();
    test_video_log_reports_register_data_and_dma_writes();
    test_bus_request_is_granted();
    test_z80_ram_is_mirrored();
    test_sound_unit_is_driven();
    test_no_sound_unit_is_as_before();
    test_sound_follows_time_not_cpu_speed();
    test_idle_three_button_pad();
    test_pressed_buttons_read_low();
    test_hv_counter_reproduces_mame_samples();
    test_hv_counter_keeps_sub_master_clock_precision();
    test_hv_counter_follows_mame_clock_rounding_and_line_end();
    test_hv_counter_survives_long_sessions();
    test_vertical_counter_and_blanking();
    test_power_on_starts_the_cycle_count_where_mame_does();
    test_master_time_uses_the_clock_rates_mame_uses();
    test_byte_write_to_z80_memory_costs_one_extra_cycle();
    test_dma_holds_the_cpu_in_proportion_to_its_length();
    expect_fatal(unknown_address, "B00000");
    test_register_write();
    test_vram_write_and_autoincrement();
    test_vram_write_to_odd_address_swaps_bytes();
    test_vram_address_uses_the_high_bits_from_the_second_word();
    test_cram_and_vsram_write();
    test_cram_address_wraps_and_vsram_ignores_out_of_range();
    test_second_control_word_is_not_a_register_write();
    test_status_read_clears_pending();
    test_status_reports_blanking();
    test_frame_interrupt_flag_stays_until_the_interrupt_is_acknowledged();
    test_dma_68k_copy();
    test_dma_68k_copy_to_cram();
    test_dma_disabled_does_nothing();
    test_dma_length_zero_and_wrap_stay_in_bounds();
    test_vram_fill_pattern();
    test_vram_copy();
    test_vdp_save_load_round_trip();
    puts("all vdp tests passed");
    return 0;
}
