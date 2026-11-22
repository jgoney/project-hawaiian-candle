/* Unit tests for the sound unit (Z80, FM chip), on hand-written programs.
 * Run with: make -C port test */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rr_sound.h"
#include "ym3438.h"
#include "z80.h"

static uint8_t core_mem[0x10000];
static uint8_t core_read(void *ctx, uint16_t addr) { (void)ctx; return core_mem[addr]; }
static void core_write(void *ctx, uint16_t addr, uint8_t value) { (void)ctx; core_mem[addr] = value; }
static uint8_t core_in(z80 *z, uint8_t port) { (void)z; (void)port; return 0xFF; }
static void core_out(z80 *z, uint8_t port, uint8_t value) { (void)z; (void)port; (void)value; }

/* The two vendored cores build and do the simplest thing. */
static void test_cores_link(void) {
    static const uint8_t program[] = {0x3E, 0x2A, 0x76}; /* LD A,0x2A; HALT */
    z80 cpu;
    memset(core_mem, 0, sizeof core_mem);
    memcpy(core_mem, program, sizeof program);
    z80_init(&cpu);
    cpu.read_byte = core_read;
    cpu.write_byte = core_write;
    cpu.port_in = core_in;
    cpu.port_out = core_out;
    z80_step(&cpu);
    z80_step(&cpu);
    assert(cpu.a == 0x2A && cpu.halted);

    static ym3438_t chip;
    long sum[2] = {0, 0};
    OPN2_SetChipType(ym3438_mode_ym2612);
    OPN2_Reset(&chip);
    /* A silent YM2612 is not at zero: its converter sits a step off centre
     * (the "ladder effect"). It is steady and small. */
    for (int sample = 0; sample < 2; sample++) {
        for (int i = 0; i < 24; i++) {
            Bit16s out[2];
            OPN2_Clock(&chip, out);
            sum[sample] += out[0] + out[1];
        }
    }
    assert(sum[0] == sum[1] && sum[0] > -400 && sum[0] < 400);
}

static uint8_t ram[0x2000], rom[0x10000];
static RrSound *snd;

static void fresh(const uint8_t *program, size_t size) {
    if (snd != NULL) rr_sound_free(snd);
    memset(ram, 0, sizeof ram);
    for (size_t i = 0; i < sizeof rom; i++) rom[i] = (uint8_t)(i * 7 + (i >> 8));
    memcpy(ram, program, size);
    snd = rr_sound_new(rom, sizeof rom, ram);
}

/* LD A,0x2A; LD (0x1000),A; HALT */
static const uint8_t store_program[] = {0x3E, 0x2A, 0x32, 0x00, 0x10, 0x76};

static void test_held_at_start(void) {
    fresh(store_program, sizeof store_program);
    rr_sound_run_to(snd, RR_SOUND_MASTER_PER_FRAME);
    assert(rr_sound_z80_steps(snd) == 0 && ram[0x1000] == 0);
    size_t have = rr_sound_available(snd);
    assert(have >= 887 && have <= 889); /* 896,040 master clocks / 1,008 */
}

static void test_runs_after_reset_release(void) {
    fresh(store_program, sizeof store_program);
    rr_sound_set_reset_held(snd, 0, 0);
    rr_sound_run_to(snd, 1000);
    assert(ram[0x1000] == 0x2A && rr_sound_z80_steps(snd) >= 3);
}

static void test_bus_held_stops_z80(void) {
    fresh(store_program, sizeof store_program);
    rr_sound_set_bus_held(snd, 0, 1);
    rr_sound_set_reset_held(snd, 0, 0);
    rr_sound_run_to(snd, 100000);
    assert(ram[0x1000] == 0 && rr_sound_z80_steps(snd) == 0);
    rr_sound_set_bus_held(snd, 100000, 0);
    rr_sound_run_to(snd, 101000);
    assert(ram[0x1000] == 0x2A);
}

static struct { uint64_t master; unsigned port; uint8_t value; int from_68000; } logged[64];
static int logged_count;
static void note_fm(void *ctx, uint64_t master, unsigned port, uint8_t value, int from_68000) {
    assert(ctx == &logged_count);
    if (logged_count < 64) {
        logged[logged_count].master = master;
        logged[logged_count].port = port;
        logged[logged_count].value = value;
        logged[logged_count].from_68000 = from_68000;
    }
    logged_count++;
}

static void test_fm_write_logged(void) {
    /* LD A,0x2B; LD (0x4000),A; LD A,0x80; LD (0x4001),A; HALT */
    static const uint8_t program[] = {0x3E, 0x2B, 0x32, 0x00, 0x40, 0x3E, 0x80, 0x32, 0x01, 0x40, 0x76};
    fresh(program, sizeof program);
    logged_count = 0;
    rr_sound_set_fm_log(snd, note_fm, &logged_count);
    rr_sound_set_reset_held(snd, 0, 0);
    rr_sound_run_to(snd, 5000);
    assert(logged_count == 2 && rr_sound_fm_writes(snd) == 2);
    assert(logged[0].port == 0 && logged[0].value == 0x2B);
    assert(logged[1].port == 1 && logged[1].value == 0x80);
    assert(logged[0].master < logged[1].master && logged[1].master < 5000);
    assert(!logged[0].from_68000 && !logged[1].from_68000 && rr_sound_fm_writes_68000(snd) == 0);
    /* A 68000 write is logged too, at the time it is made. */
    rr_sound_fm_write(snd, 6000, 2, 0x30);
    assert(logged_count == 3 && logged[2].port == 2 && logged[2].value == 0x30 && logged[2].master == 6000);
    assert(logged[2].from_68000 && rr_sound_fm_writes_68000(snd) == 1 && rr_sound_fm_writes(snd) == 3);
    assert(rr_sound_now(snd) == 6000);
}

/* Nine writes to 0x6000 choose the bank, lowest bit first; then a read
 * through the window at 0x8000. */
static size_t bank_program(uint8_t *at, unsigned bank) {
    size_t n = 0;
    for (int bit = 0; bit < 9; bit++) {
        at[n++] = 0x3E; at[n++] = (uint8_t)((bank >> bit) & 1); /* LD A,bit */
        at[n++] = 0x32; at[n++] = 0x00; at[n++] = 0x60;          /* LD (0x6000),A */
    }
    at[n++] = 0x3A; at[n++] = 0x34; at[n++] = 0x92; /* LD A,(0x9234) */
    at[n++] = 0x32; at[n++] = 0x00; at[n++] = 0x10; /* LD (0x1000),A */
    at[n++] = 0x76;
    return n;
}

static void test_bank_and_rom(void) {
    uint8_t program[64];
    size_t size = bank_program(program, 1);
    fresh(program, size);
    rr_sound_set_reset_held(snd, 0, 0);
    rr_sound_run_to(snd, 20000);
    assert(ram[0x1000] == rom[0x8000 + 0x1234]);
    /* A bank past the end of the ROM reads 0xFF and touches nothing. */
    size = bank_program(program, 0x1FF);
    fresh(program, size);
    ram[0x1000] = 0x55;
    rr_sound_set_reset_held(snd, 0, 0);
    rr_sound_run_to(snd, 20000);
    assert(ram[0x1000] == 0xFF);
    /* The Z80's memory repeats every 0x2000 below 0x4000. */
    static const uint8_t mirror[] = {0x3E, 0x77, 0x32, 0x00, 0x30, 0x76}; /* LD (0x3000),A */
    fresh(mirror, sizeof mirror);
    rr_sound_set_reset_held(snd, 0, 0);
    rr_sound_run_to(snd, 2000);
    assert(ram[0x1000] == 0x77);
}

/* The Z80 reaches the square-wave chip at 0x7F11 and its neighbours. */
static void test_psg_writes_are_counted(void) {
    /* LD A,0x9F; LD (0x7F11),A; LD (0x7F13),A; LD (0x7F20),A; HALT */
    static const uint8_t program[] = {0x3E, 0x9F, 0x32, 0x11, 0x7F, 0x32, 0x13, 0x7F, 0x32, 0x20, 0x7F, 0x76};
    fresh(program, sizeof program);
    rr_sound_set_reset_held(snd, 0, 0);
    rr_sound_run_to(snd, 5000);
    assert(rr_sound_psg_writes(snd) == 2);
}

static void test_interrupt_each_frame(void) {
    uint8_t program[0x40];
    memset(program, 0, sizeof program);
    /* 0: LD SP,0x1FF0; IM 1; EI; HALT; JR -3 (back to the HALT) */
    static const uint8_t start[] = {0x31, 0xF0, 0x1F, 0xED, 0x56, 0xFB, 0x76, 0x18, 0xFD};
    /* 0x38: LD HL,0x1000; INC (HL); EI; RET */
    static const uint8_t handler[] = {0x21, 0x00, 0x10, 0x34, 0xFB, 0xC9};
    memcpy(program, start, sizeof start);
    memcpy(program + 0x38, handler, sizeof handler);
    fresh(program, sizeof program);
    rr_sound_set_reset_held(snd, 0, 0);
    rr_sound_run_to(snd, RR_SOUND_INTERRUPT_AT - 100);
    assert(ram[0x1000] == 0); /* not before the vertical blank */
    rr_sound_run_to(snd, RR_SOUND_MASTER_PER_FRAME * 3);
    /* The handler ends inside the line the interrupt is held for, so the
     * real Z80 takes it again; what matters is that every frame has one. */
    assert(ram[0x1000] >= 3);
    uint8_t after_three = ram[0x1000];
    rr_sound_run_to(snd, RR_SOUND_MASTER_PER_FRAME * 3 + RR_SOUND_INTERRUPT_AT - 100);
    assert(ram[0x1000] == after_three); /* and none between blanks */
    /* An interrupt the Z80 could not take (bus held all through it) is gone. */
    rr_sound_set_bus_held(snd, RR_SOUND_MASTER_PER_FRAME * 3 + RR_SOUND_INTERRUPT_AT - 100, 1);
    rr_sound_set_bus_held(snd, RR_SOUND_MASTER_PER_FRAME * 4 + 100, 0);
    rr_sound_run_to(snd, RR_SOUND_MASTER_PER_FRAME * 4 + 50000);
    assert(ram[0x1000] == after_three);
}

static void test_power_on_starts_again(void) {
    fresh(store_program, sizeof store_program);
    rr_sound_set_reset_held(snd, 0, 0);
    rr_sound_run_to(snd, RR_SOUND_MASTER_PER_FRAME);
    assert(rr_sound_z80_steps(snd) > 0 && rr_sound_available(snd) > 0);
    ram[0x1000] = 0;
    rr_sound_power_on(snd);
    assert(rr_sound_available(snd) == 0 && rr_sound_z80_steps(snd) == 0);
    rr_sound_run_to(snd, 50000); /* an earlier time than before: time began again */
    assert(ram[0x1000] == 0 && rr_sound_z80_steps(snd) == 0); /* reset is held */
    assert(rr_sound_available(snd) == 50000 / RR_SOUND_MASTER_PER_SAMPLE);
    rr_sound_set_reset_held(snd, 50000, 0);
    rr_sound_run_to(snd, 52000);
    assert(ram[0x1000] == 0x2A);
}

/* A unit attached to a machine that has been running starts at that time. */
static void test_set_time_skips_ahead(void) {
    fresh(store_program, sizeof store_program);
    uint64_t late = RR_SOUND_MASTER_PER_FRAME * 60 * 3600; /* an hour in */
    rr_sound_set_time(snd, late);
    assert(rr_sound_now(snd) == late && rr_sound_available(snd) == 0);
    rr_sound_run_to(snd, late + RR_SOUND_MASTER_PER_FRAME);
    size_t have = rr_sound_available(snd);
    assert(have >= 887 && have <= 889);
    rr_sound_set_reset_held(snd, late + RR_SOUND_MASTER_PER_FRAME, 0);
    rr_sound_run_to(snd, late + RR_SOUND_MASTER_PER_FRAME + 2000);
    assert(ram[0x1000] == 0x2A);
}

static void test_long_hold(void) {
    fresh(store_program, sizeof store_program);
    rr_sound_set_bus_held(snd, 0, 1);
    rr_sound_set_reset_held(snd, 0, 0);
    rr_sound_run_to(snd, RR_SOUND_MASTER_PER_FRAME * 60 * 20); /* 20 seconds, never taken */
    assert(rr_sound_z80_steps(snd) == 0);
    assert(rr_sound_available(snd) == (size_t)RR_SOUND_BUFFER_SECONDS * RR_SOUND_HZ);
}

static void fm(uint64_t *at, unsigned reg, uint8_t value) {
    rr_sound_fm_write(snd, *at, 0, (uint8_t)reg);
    *at += 2000;
    rr_sound_fm_write(snd, *at, 1, value);
    *at += 2000;
}

/* A plain sine on channel 1, set up as the 68000 would: about 251 Hz. */
static void test_tone_and_take(void) {
    static int16_t out[2 * 6000], first[2 * 10];
    uint64_t at = 1000;
    fresh(store_program, sizeof store_program);
    fm(&at, 0x22, 0x00); fm(&at, 0x27, 0x00); fm(&at, 0x28, 0x00);
    fm(&at, 0xB0, 0x07); fm(&at, 0xB4, 0xC0);
    fm(&at, 0x40, 0x7F); fm(&at, 0x44, 0x7F); fm(&at, 0x48, 0x7F); fm(&at, 0x4C, 0x00);
    fm(&at, 0x3C, 0x01); fm(&at, 0x5C, 0x1F); fm(&at, 0x6C, 0x00); fm(&at, 0x7C, 0x00);
    fm(&at, 0x8C, 0x0F); fm(&at, 0xA4, 0x22); fm(&at, 0xA0, 0x69);
    rr_sound_run_to(snd, at);
    while (rr_sound_take(snd, out, 6000) > 0) {} /* the silence before the note */
    fm(&at, 0x28, 0xF0);
    rr_sound_run_to(snd, at + (uint64_t)RR_SOUND_MASTER_PER_SAMPLE * 5400); /* about 0.1 s */
    size_t have = rr_sound_available(snd);
    assert(have >= 5390 && have <= 5410);
    /* Taking a few leaves the rest, in order. */
    assert(rr_sound_take(snd, first, 10) == 10 && rr_sound_available(snd) == have - 10);
    size_t got = rr_sound_take(snd, out, 6000);
    assert(got == have - 10 && rr_sound_available(snd) == 0 && rr_sound_take(snd, out + 2 * got, 10) == 0);
    long mean = 0, low = 32767, high = -32768;
    for (size_t i = 0; i < got; i++) {
        mean += out[2 * i];
        if (out[2 * i] < low) low = out[2 * i];
        if (out[2 * i] > high) high = out[2 * i];
        assert(out[2 * i] == out[2 * i + 1]); /* both sides on */
    }
    mean /= (long)got;
    assert(high - low > 2000);
    int crossings = 0;
    for (size_t i = 1; i < got; i++) {
        if ((out[2 * (i - 1)] < mean) != (out[2 * i] < mean)) crossings++;
    }
    assert(crossings >= 40 && crossings <= 60); /* 251 Hz for 0.1 s: about 50 */
}

int main(void) {
    test_cores_link();
    test_held_at_start();
    test_runs_after_reset_release();
    test_bus_held_stops_z80();
    test_fm_write_logged();
    test_bank_and_rom();
    test_psg_writes_are_counted();
    test_interrupt_each_frame();
    test_power_on_starts_again();
    test_set_time_skips_ahead();
    test_long_hold();
    test_tone_and_take();
    printf("test_sound: all passed\n");
    return 0;
}
