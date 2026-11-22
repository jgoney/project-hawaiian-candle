/* Unit tests for the host console API. Run with: make -C port test */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rr_between.h"
#include "rr_console.h"
#include "rr_dashes.h"
#include "rr_genesis.h"
#include "rr_mem.h"
#include "rr_objects.h"
#include "rr_road.h"
#include "rr_run.h"

/* A 0x60000-byte ROM: stack pointer 0x00FFFF00, reset vector 0x200, at 0x15472
 * the table of 65536 / n that the road arithmetic uses, at 0x27C84 a table
 * of perspective scales (65536 x 64 / (distance + 64), less one), at 0x7C58
 * the table of sprite shapes the object drawing uses and at 0x58000 object
 * picture 1 (16 x 16, every byte different); zeros elsewhere. */
const uint32_t rr_code_rom_crc32 = 0xC7F71008u;
static RrGenesis *seen_hw; /* the console's hardware, noted by the instruction at 0x200 */

/* 0x200 loops in place until RAM byte 0 is set, then jumps to untranslated 0x300. */
static int fall_through; /* the instruction at 0x200 goes on to 0x202, which jumps back */
static int visit_object_drawing; /* ... or, once, to 0xA4EE, where the game draws its objects */
static void insn_loop(RrMachine *m) {
    seen_hw = (RrGenesis *)m->hw;
    m->cpu.cycles += 10;
    if (m->ram[0] != 0) m->cpu.pc = 0x300;
    else if (visit_object_drawing) {
        visit_object_drawing = 0;
        m->cpu.pc = 0xA4EE;
    } else if (fall_through) m->cpu.pc = 0x202;
}
static void insn_back(RrMachine *m) {
    m->cpu.cycles += 10;
    m->cpu.pc = 0x200;
}
/* 0x340: a line interrupt handler that notes the line it ran on (RAM bytes 2
 * and 3: count and first line) and returns like RTE. */
static void insn_line_handler(RrMachine *m) {
    RrGenesis *g = (RrGenesis *)m->hw;
    if (m->ram[2] == 0) m->ram[3] = (uint8_t)rr_genesis_line(g);
    m->ram[2]++;
    m->cpu.sr = rr_read16(m, m->cpu.a[7]);
    m->cpu.pc = rr_read32(m, m->cpu.a[7] + 2);
    m->cpu.a[7] += 6;
    m->cpu.cycles += 20;
}
RrInsnFn rr_lookup(uint32_t pc) {
    return pc == 0x200 ? insn_loop : pc == 0x202 || pc == 0xA4EE ? insn_back
           : pc == 0x340 ? insn_line_handler : NULL;
}

static char rom_path[] = "/tmp/rr_console_rom_XXXXXX";
static char other_path[] = "/tmp/rr_console_other_XXXXXX";

static void write_roms(void) {
    static uint8_t rom[0x60000];
    rom[1] = 0xFF; rom[2] = 0xFF; rom[6] = 0x02;
    for (int high = 1; high <= 4; high++) {
        for (int wide = 1; wide <= 4; wide++) {
            uint8_t *shape = rom + 0x7C58 + (high - 1) * 0x20 + (wide - 1) * 8;
            shape[1] = (uint8_t)(high * 8);
            shape[2] = (uint8_t)((wide - 1) << 2 | (high - 1));
            shape[5] = (uint8_t)(wide * high);
            shape[7] = (uint8_t)(wide * 8);
        }
    }
    rom[0x56A0A + 5] = 0x05; rom[0x56A0A + 6] = 0x80; /* picture 1 is at 0x58000 */
    uint8_t *picture = rom + 0x58000;
    picture[0] = 16; picture[1] = 16; picture[2] = 0x20;
    for (int r = 0; r < 16; r++)
        for (int column = 0; column < 8; column++) picture[4 + r * 9 + column] = (uint8_t)(((r * 8 + column) % 15 + 1) * 0x11);
    for (int n = 2; n < 256; n++) {
        rom[0x15472 + 2 * n] = (uint8_t)((65536 / n) >> 8);
        rom[0x15472 + 2 * n + 1] = (uint8_t)(65536 / n);
    }
    for (int d = 0; d < 16384; d++) {
        unsigned scale = (unsigned)(0x10000L * 64 / (d + 64) - 1);
        rom[0x27C84 + 2 * d] = (uint8_t)(scale >> 8);
        rom[0x27C84 + 2 * d + 1] = (uint8_t)scale;
    }
    int fd = mkstemp(rom_path);
    assert(fd >= 0 && write(fd, rom, sizeof rom) == (ssize_t)sizeof rom);
    close(fd);
    rom[0x100] = 1; /* a different ROM */
    fd = mkstemp(other_path);
    assert(fd >= 0 && write(fd, rom, sizeof rom) == (ssize_t)sizeof rom);
    close(fd);
}

static RrConsole *open_console(void) {
    char error[256] = "";
    RrConsole *c = rr_console_open(rom_path, error, sizeof error);
    if (c == NULL) fprintf(stderr, "open failed: %s\n", error);
    assert(c != NULL && error[0] == 0);
    return c;
}

/* ---- sound ---- */

#define FRAME_SOUND_48K (896040.0 / 1008 * 48000 / 53267) /* frames of sound a video frame makes */
static float sound_out[2 * 200000];

static void test_sound_is_off_as_opened(void) {
    RrConsole *c = open_console();
    assert(!rr_console_sound(c));
    for (int i = 0; i < 10; i++) rr_console_run_frame(c);
    assert(rr_console_take_sound(c, 48000, sound_out, 200000) == 0);
    rr_console_close(c);
}

static void test_sound_is_handed_out_at_the_asked_rate(void) {
    RrConsole *c = open_console();
    rr_console_set_sound(c, 1);
    assert(rr_console_sound(c));
    for (int i = 0; i < 10; i++) rr_console_run_frame(c);
    size_t got = rr_console_take_sound(c, 48000, sound_out, 200000);
    assert(fabs((double)got - 10 * FRAME_SOUND_48K) < 30);
    for (size_t i = 0; i < 2 * got; i++) assert(fabs(sound_out[i]) < 0.02); /* nothing is playing */
    assert(rr_console_take_sound(c, 48000, sound_out, 200000) == 0);
    /* Another card rate gets the same stretch of time. */
    for (int i = 0; i < 10; i++) rr_console_run_frame(c);
    got = rr_console_take_sound(c, 44100, sound_out, 200000);
    assert(fabs((double)got - 10 * FRAME_SOUND_48K * 44100 / 48000) < 30);
    /* A rate that makes no sense gives nothing. */
    rr_console_run_frame(c);
    assert(rr_console_take_sound(c, 0, sound_out, 200000) == 0);
    assert(rr_console_take_sound(c, -1, sound_out, 200000) == 0);
    rr_console_close(c);
}

static void test_a_faster_game_makes_no_more_sound(void) {
    RrConsole *c = open_console();
    rr_console_set_sound(c, 1);
    rr_console_set_cpu_speed(c, 200);
    for (int i = 0; i < 10; i++) rr_console_run_frame(c);
    size_t got = rr_console_take_sound(c, 48000, sound_out, 200000);
    assert(fabs((double)got - 10 * FRAME_SOUND_48K) < 30);
    rr_console_close(c);
}

static void test_sound_stays_on_through_a_reset(void) {
    RrConsole *c = open_console();
    rr_console_set_sound(c, 1);
    for (int i = 0; i < 5; i++) rr_console_run_frame(c);
    rr_console_reset(c);
    assert(rr_console_sound(c));
    /* What was waiting before the reset is gone. */
    assert(rr_console_take_sound(c, 48000, sound_out, 200000) == 0);
    rr_console_run_frame(c);
    size_t got = rr_console_take_sound(c, 48000, sound_out, 200000);
    assert(got > 0 && fabs((double)got - FRAME_SOUND_48K) < 30);
    rr_console_close(c);
}

static void test_sound_never_asked_for_does_not_pile_up(void) {
    RrConsole *c = open_console();
    rr_console_set_sound(c, 1);
    for (int i = 0; i < 120; i++) rr_console_run_frame(c); /* two seconds */
    size_t got = rr_console_take_sound(c, 48000, sound_out, 200000);
    assert(got > 20000 && got <= 24000 + 30); /* the newest half second */
    rr_console_close(c);
}

/* A caller that takes less than is made each frame must not build up a
 * longer and longer delay inside the converter. */
static void test_small_takes_do_not_pile_up_either(void) {
    RrConsole *c = open_console();
    rr_console_set_sound(c, 1);
    for (int i = 0; i < 180; i++) { /* three seconds, taking an eighth of each frame's sound */
        rr_console_run_frame(c);
        assert(rr_console_take_sound(c, 48000, sound_out, 100) == 100);
    }
    size_t got = rr_console_take_sound(c, 48000, sound_out, 200000);
    assert(got > 20000 && got <= 24000 + 1200); /* the newest half second, and a chunk in hand */
    rr_console_close(c);
}

static void test_sound_can_be_turned_off_and_on_mid_run(void) {
    RrConsole *c = open_console();
    for (int i = 0; i < 600; i++) rr_console_run_frame(c);
    rr_console_set_sound(c, 1); /* no catching up on the ten seconds before */
    rr_console_run_frame(c);
    size_t got = rr_console_take_sound(c, 48000, sound_out, 200000);
    assert(fabs((double)got - FRAME_SOUND_48K) < 30);
    rr_console_set_sound(c, 0);
    assert(!rr_console_sound(c));
    rr_console_run_frame(c);
    assert(rr_console_take_sound(c, 48000, sound_out, 200000) == 0);
    rr_console_close(c);
}

static void test_sound_does_not_change_the_game(void) {
    RrConsole *quiet = open_console(), *loud = open_console();
    rr_console_set_sound(loud, 1);
    for (int i = 0; i < 30; i++) {
        rr_console_run_frame(quiet);
        rr_console_run_frame(loud);
    }
    assert(memcmp(rr_console_ram(quiet), rr_console_ram(loud), 0x10000) == 0);
    assert(memcmp(rr_console_frame(quiet), rr_console_frame(loud), 320 * 224 * 4) == 0);
    rr_console_close(quiet);
    rr_console_close(loud);
}

static void test_open_matching_rom_and_run_frames(void) {
    RrConsole *c = open_console();
    assert(rr_console_halt_message(c) == NULL);
    assert(rr_console_run_frame(c) == 1 && rr_console_run_frame(c) == 1);
    assert(rr_console_frames(c) == 2);
    assert(rr_console_frame(c) != NULL && rr_console_ram(c) != NULL);
    rr_console_close(c);
}

static void test_open_missing_file_names_the_path(void) {
    char error[256] = "";
    assert(rr_console_open("/nonexistent/rr2.gen", error, sizeof error) == NULL);
    assert(strstr(error, "/nonexistent/rr2.gen") != NULL);
}

static void test_open_wrong_rom_names_both_checksums(void) {
    char error[256] = "";
    assert(rr_console_open(other_path, error, sizeof error) == NULL);
    assert(strstr(error, "C7F71008") != NULL && strstr(error, other_path) != NULL);
}

static void test_open_error_fits_a_small_buffer(void) {
    char error[16];
    memset(error, 'x', sizeof error);
    assert(rr_console_open("/nonexistent/rr2.gen", error, sizeof error) == NULL);
    assert(memchr(error, 0, sizeof error) != NULL);
}

static void test_fatal_error_halts_instead_of_exiting(void) {
    RrConsole *c = open_console();
    assert(rr_console_run_frame(c) == 1);
    ((uint8_t *)rr_console_ram(c))[0] = 1; /* make the code jump to 0x300 */
    assert(rr_console_run_frame(c) == 0);  /* and we are still here */
    const char *message = rr_console_halt_message(c);
    assert(message != NULL && strstr(message, "000300") != NULL);
    assert(strstr(message, "untranslated") != NULL);
    assert(rr_console_run_frame(c) == 0);
    assert(rr_console_frames(c) == 1);
    rr_console_close(c);
}

static void test_reset_clears_a_halt(void) {
    RrConsole *c = open_console();
    ((uint8_t *)rr_console_ram(c))[0] = 1;
    assert(rr_console_run_frame(c) == 0);
    rr_console_reset(c);
    assert(rr_console_halt_message(c) == NULL);
    assert(rr_console_ram(c)[0] == 0);
    assert(rr_console_run_frame(c) == 1);
    rr_console_close(c);
}

static void test_opposite_directions_are_never_sent_together(void) {
    RrConsole *c = open_console();
    rr_console_set_buttons(c, 0, RR_PAD_LEFT | RR_PAD_RIGHT | RR_PAD_A);
    assert(rr_console_buttons(c, 0) == (RR_PAD_RIGHT | RR_PAD_A));
    rr_console_set_buttons(c, 1, RR_PAD_UP | RR_PAD_DOWN | RR_PAD_START);
    assert(rr_console_buttons(c, 1) == (RR_PAD_DOWN | RR_PAD_START));
    rr_console_set_buttons(c, 7, RR_PAD_A); /* no such port: ignored */
    assert(rr_console_buttons(c, 7) == 0);
    rr_console_close(c);
}

static void test_buttons_survive_a_reset(void) {
    RrConsole *c = open_console();
    rr_console_set_buttons(c, 0, RR_PAD_START);
    rr_console_reset(c);
    assert(rr_console_buttons(c, 0) == RR_PAD_START);
    rr_console_close(c);
}

static void test_fatal_handler_is_removed_after_a_frame(void) {
    RrConsole *c = open_console();
    assert(rr_console_run_frame(c) == 1);
    rr_console_close(c);
    /* With no console running, rr_fatal must exit the process as before. */
    fflush(NULL);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        fclose(stderr);
        rr_fatal(NULL, "plain fatal");
    }
    int status;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 2);
}

/* ---- frame timing, on a bare machine ---- */

static uint8_t timing_rom[0x400];

static RrGenesis *timing_machine(RrMachine *m) {
    memset(m, 0, sizeof *m);
    memset(timing_rom, 0, sizeof timing_rom);
    timing_rom[0x70 + 2] = 0x03; timing_rom[0x70 + 3] = 0x40; /* level 4 vector = 0x340 */
    m->rom = timing_rom;
    m->rom_size = sizeof timing_rom;
    m->cpu.pc = 0x200;
    m->cpu.sr = 0x2000;
    m->cpu.a[7] = 0x00FFFF00;
    m->cpu.cycles = 12345; /* an arbitrary starting point */
    RrGenesis *g = rr_genesis_new(m);
    m->hw = &g->hw;
    return g;
}

static void test_frame_length_does_not_drift(void) {
    static RrMachine m;
    RrGenesis *g = timing_machine(&m);
    /* The loop instruction takes 10 cycles, so every frame boundary falls
     * inside an instruction. Each call must still end in the next frame, and
     * the overshoot must never accumulate. */
    for (uint64_t frame = 1; frame <= 100; frame++) {
        rr_genesis_run_frame(g);
        uint64_t master = rr_genesis_master_time(g);
        assert(master / RR_MASTER_PER_FRAME == frame);
        assert(master % RR_MASTER_PER_FRAME < 20 * 7); /* within two instructions of its start */
    }
    rr_genesis_free(g);
}

/* The picture can change part-way down the screen (the game's line interrupt
 * handler rewrites video registers), so the layer map and the per-line state
 * are taken line by line as the picture is drawn, not once a frame. */
static void change_backdrop_at_line_100(RrGenesis *g, void *ctx) {
    (void)ctx;
    if (rr_genesis_line(g) == 100) g->vdp.reg[7] = 0x01;
}
static void test_source_map_follows_a_mid_frame_change(void) {
    static RrMachine m;
    static RrPixelSource sources[RR_SCREEN_W * RR_SCREEN_H];
    static RrVdpLineState lines[RR_SCREEN_H];
    RrGenesis *g = timing_machine(&m);
    g->vdp.reg[1] = 0x44;  /* display on */
    g->vdp.reg[12] = 0x81; /* 40 cells */
    g->vdp.cram[0] = 0x000E;
    g->vdp.cram[1] = 0x0E00;
    g->sources = sources;
    g->line_state = lines;
    g->step_hook = change_backdrop_at_line_100;
    rr_genesis_run_frame(g);
    assert(sources[50 * RR_SCREEN_W + 7].layer == RR_SRC_BACKDROP);
    assert(sources[50 * RR_SCREEN_W + 7].colour == 0 && sources[150 * RR_SCREEN_W + 7].colour == 1);
    assert(lines[100].reg[7] == 0x00 && lines[101].reg[7] == 0x01);
    assert(g->frame[150 * RR_SCREEN_W + 7] == rr_vdp_colour(0x0E00));
    rr_genesis_free(g);
}

/* MAME enters the frame interrupt handler on line 224 (never 223 or 225), a
 * little way into the line. */
static void test_frame_interrupt_is_raised_shortly_after_line_224_starts(void) {
    static RrMachine m;
    RrGenesis *g = timing_machine(&m);
    timing_rom[0x78 + 2] = 0x03; timing_rom[0x78 + 3] = 0x40; /* level 6 uses the same handler */
    g->vdp.reg[1] = 0x64; /* display and frame interrupt on */
    rr_genesis_run_frame(g);
    assert(m.ram[2] == 1 && m.ram[3] == 224);
    assert(g->vblank_entries == 1);
    rr_genesis_free(g);
}

/* A tool may replace the cycle count (to restart from a recorded state). The
 * frame in progress must carry on from the new position: nothing scheduled
 * before it fires late, and the next frame is whole. */
static void test_frame_resumes_from_wherever_the_clock_is_moved_to(void) {
    static RrMachine m;
    RrGenesis *g = timing_machine(&m);
    timing_rom[0x78 + 2] = 0x03; timing_rom[0x78 + 3] = 0x40;
    g->vdp.reg[1] = 0x64; /* frame interrupt on */
    rr_genesis_run_frame(g);
    assert(g->vblank_entries == 1);
    /* Move to line 230 of some later frame: past where the interrupt is raised. */
    uint64_t target = 50 * RR_MASTER_PER_FRAME + 230ull * RR_MASTER_PER_LINE + 100;
    m.cpu.cycles = (target - RR_CLOCK_PHASE) * RR_CPU_HZ / RR_MASTER_HZ;
    rr_genesis_time_changed(g);
    rr_genesis_run_frame(g);
    assert(rr_genesis_master_time(g) / RR_MASTER_PER_FRAME == 51);
    assert(g->vblank_entries == 1); /* the rest of frame 50 had no frame interrupt left */
    rr_genesis_run_frame(g);
    assert(rr_genesis_master_time(g) / RR_MASTER_PER_FRAME == 52);
    assert(g->vblank_entries == 2);
    rr_genesis_free(g);
}

/* Where MAME raises a line's interrupts, bracketed from 1,600 interrupt
 * entries in the trace windows: an instruction boundary at position 205.000
 * of the line never saw one, a boundary at 205.078 did. In the race-start
 * window the boundary at cycle 132,634,451 (position 202.2 of line 42) was
 * passed and the interrupt was taken at the next one. */
static uint64_t accepted_at, previous_step;
static void note_acceptance(RrGenesis *g, void *ctx) {
    (void)ctx;
    if (g->m->cpu.pc == 0x340 && accepted_at == 0) accepted_at = previous_step + 10;
    previous_step = g->m->cpu.cycles;
}
static void test_line_interrupt_is_raised_just_past_position_205(void) {
    static RrMachine m;
    RrGenesis *g = timing_machine(&m);
    g->vdp.reg[0] = 0x14; /* line interrupts enabled, every line */
    g->hint_counter = 0;
    m.cpu.cycles = 132634451ull - 20;
    rr_genesis_time_changed(g);
    accepted_at = previous_step = 0;
    g->step_hook = note_acceptance;
    rr_genesis_run_frame(g);
    assert(accepted_at == 132634461ull);
    rr_genesis_free(g);
}

/* MAME's 68000 notes a waiting interrupt at one step inside each instruction:
 * for most that is the last four cycles, but shifts, multiplies, divides and
 * short untaken branches finish with internal work and note it before that. An
 * interrupt raised later in such an instruction waits for the next one. In
 * the trace windows a register shift took all 42 interrupts raised up to 8
 * cycles in and none of the 12 raised later; multiplies and divides likewise
 * (49 late, from 9 cycles after their operand fetch); a short branch that
 * fell through missed both of the two raised 6 and 7 cycles in.
 * Here the interrupt is raised 9 cycles into an instruction that began at
 * cycle 132,634,443 of the reference run. */
static uint64_t acceptance_start = 132634443ull;
static uint64_t acceptance_after_opcode(unsigned opcode) {
    static RrMachine m;
    RrGenesis *g = timing_machine(&m);
    timing_rom[0x200] = (uint8_t)(opcode >> 8);
    timing_rom[0x201] = (uint8_t)opcode;
    g->vdp.reg[0] = 0x14; /* line interrupts enabled, every line */
    g->hint_counter = 0;
    m.cpu.cycles = acceptance_start - 20;
    rr_genesis_time_changed(g);
    accepted_at = previous_step = 0;
    g->step_hook = note_acceptance;
    rr_genesis_run_frame(g);
    rr_genesis_free(g);
    return accepted_at;
}
static void test_interrupt_late_in_a_shift_multiply_or_divide_waits_one_instruction(void) {
    assert(acceptance_after_opcode(0x4E71) == 132634453ull); /* nop: taken at its end */
    assert(acceptance_after_opcode(0xE040) == 132634463ull); /* asr.w #8,d0 */
    assert(acceptance_after_opcode(0xC2C0) == 132634463ull); /* mulu d0,d1 */
    assert(acceptance_after_opcode(0x80C1) == 132634463ull); /* divu d1,d0 */
    assert(acceptance_after_opcode(0xE0D0) == 132634453ull); /* asr (a0): a memory shift ends with a fetch */
    /* With an operand to fetch first, the note is taken later: mulu #n,d0
     * spends 4 cycles on that, so 9 cycles in is still in time. */
    assert(acceptance_after_opcode(0xC0FC) == 132634453ull);
    /* A divide notes it sooner than a multiply: raised 5 cycles in (the
     * instruction now starts at ...447), a multiply still takes it and a
     * divide does not (divu d3,d4 in the race-66 trace window, 6 cycles in). */
    acceptance_start = 132634447ull;
    assert(acceptance_after_opcode(0xC2C0) == 132634457ull); /* mulu d0,d1 */
    assert(acceptance_after_opcode(0x88C3) == 132634467ull); /* divu d3,d4 */
    acceptance_start = 132634443ull;
    /* A branch that is taken ends with a fetch; one that falls through does not. */
    assert(acceptance_after_opcode(0x6CDA) == 132634453ull);
    fall_through = 1;
    assert(acceptance_after_opcode(0x6CDA) == 132634463ull);
    assert(acceptance_after_opcode(0x6C00) == 132634453ull); /* the long form ends with a fetch either way */
    assert(acceptance_after_opcode(0x4E71) == 132634453ull);
    fall_through = 0;
}

/* An interrupt that has been waiting behind the CPU's mask was raised long
 * ago. A newer raise of the same level must not make it look fresh: here a
 * line interrupt has been pending for two lines when the mask drops at the
 * start of a shift, and the next line's raise comes 9 cycles into that shift
 * (too late for the shift to note). The old one is still taken right after it. */
static void unmask_then_note(RrGenesis *g, void *ctx) {
    if (g->m->cpu.cycles == 132634443ull) g->m->cpu.sr = 0x2000;
    note_acceptance(g, ctx);
}
static void test_a_waiting_interrupt_is_not_delayed_by_a_newer_raise(void) {
    static RrMachine m;
    RrGenesis *g = timing_machine(&m);
    timing_rom[0x200] = 0xE0; timing_rom[0x201] = 0x40; /* asr.w #8,d0 */
    g->vdp.reg[0] = 0x14;
    g->hint_counter = 0;
    m.cpu.sr = 0x2700;
    m.cpu.cycles = 132634443ull - 980; /* two lines earlier */
    rr_genesis_time_changed(g);
    accepted_at = previous_step = 0;
    g->step_hook = unmask_then_note;
    rr_genesis_run_frame(g);
    assert(accepted_at == 132634453ull);
    rr_genesis_free(g);
}

/* Two more of MAME's steps, each measured in the trace windows. A 32-bit
 * compare with a data register finishes with two internal cycles and notes
 * the interrupt during them: 34 entries, taken when raised up to 2 cycles
 * after the instruction ended. An indexed lea notes it two cycles sooner than
 * most: raised in its last 2 cycles, it was missed both times.
 * The interrupt is raised at cycle 132,634,452; the fake instruction first
 * runs from ...441 to ...451, so it has just ended. */
static void test_interrupt_step_of_long_compare_and_indexed_lea(void) {
    acceptance_start = 132634441ull;
    assert(acceptance_after_opcode(0x4E71) == 132634461ull); /* nop: raised after it ended */
    assert(acceptance_after_opcode(0xB880) == 132634451ull); /* cmp.l d0,d4 */
    assert(acceptance_after_opcode(0xB8BC) == 132634451ull); /* cmp.l #n,d4 */
    assert(acceptance_after_opcode(0xC342) == 132634451ull); /* exg d1,d2: the same shape */
    assert(acceptance_after_opcode(0xB9FC) == 132634451ull); /* cmpa.l #n,a4: and again */
    assert(acceptance_after_opcode(0x0801) == 132634451ull); /* btst #n,d1 */
    assert(acceptance_after_opcode(0x08C5) == 132634451ull); /* bset #n,d5 */
    assert(acceptance_after_opcode(0x4283) == 132634451ull); /* clr.l d3 */
    assert(acceptance_after_opcode(0xD090) == 132634451ull); /* add.l (a0),d0 */
    assert(acceptance_after_opcode(0xD081) == 132634461ull); /* add.l d1,d0: four internal cycles, like most */
    assert(acceptance_after_opcode(0xB840) == 132634461ull); /* cmp.w d0,d4: no internal cycles */
    acceptance_start = 132634443ull; /* now it runs to ...453: raised 1 cycle before its end */
    assert(acceptance_after_opcode(0x4E71) == 132634453ull);
    assert(acceptance_after_opcode(0x47F2) == 132634463ull); /* lea (d8,a2,xn),a3 */
    assert(acceptance_after_opcode(0x47F9) == 132634453ull); /* lea abs.l,a3 */
}

static void test_no_line_interrupt_before_the_counter_is_programmed(void) {
    static RrMachine m;
    RrGenesis *g = timing_machine(&m);
    g->vdp.reg[0] = 0x14;  /* line interrupts enabled */
    g->vdp.reg[10] = 50;   /* every 51 lines */
    rr_genesis_run_frame(g);
    rr_genesis_run_frame(g);
    assert(m.ram[2] > 0);                    /* interrupts did arrive */
    assert(m.ram[3] == 50 || m.ram[3] == 51); /* and the first was not at line 0 */
    rr_genesis_free(g);
}

/* ---- CPU speed ---- */

/* Instructions the 10-cycle loop executes in 100 frames from cycle 12,345,
 * measured before the speed scale was added. */
#define PINNED_100_FRAMES 1278822ull

static uint64_t instructions_in_frames(unsigned percent, int frames) {
    static RrMachine m;
    RrGenesis *g = timing_machine(&m);
    if (percent != 0) rr_genesis_set_cpu_speed(g, percent);
    uint64_t total = 0;
    for (int i = 0; i < frames; i++) total += rr_genesis_run_frame(g);
    rr_genesis_free(g);
    return total;
}

/* At 100% nothing changes: the count is pinned from before the speed scale existed. */
static void test_cpu_speed_100_is_the_original(void) {
    assert(instructions_in_frames(0, 100) == PINNED_100_FRAMES);
    assert(instructions_in_frames(100, 100) == PINNED_100_FRAMES);
}

/* At 800% the 68000 gets eight times the cycles in each video line. */
static void test_cpu_speed_800_runs_eight_times_the_instructions_per_frame(void) {
    uint64_t once = instructions_in_frames(100, 1), eight = instructions_in_frames(800, 1);
    assert(eight + RR_LINES_PER_FRAME >= once * 8 && eight <= once * 8 + RR_LINES_PER_FRAME);
}

/* The video side is unchanged as the game sees it: one frame interrupt, on line 224. */
static void test_cpu_speed_keeps_the_frame_interrupt_on_line_224(void) {
    static RrMachine m;
    RrGenesis *g = timing_machine(&m);
    timing_rom[0x78 + 2] = 0x03; timing_rom[0x78 + 3] = 0x40;
    g->vdp.reg[1] = 0x64;
    rr_genesis_set_cpu_speed(g, 800);
    rr_genesis_run_frame(g);
    assert(m.ram[2] == 1 && m.ram[3] == 224);
    rr_genesis_run_frame(g);
    assert(m.ram[2] == 2 && g->vblank_entries == 2);
    rr_genesis_free(g);
}

static void test_cpu_speed_is_clamped_and_the_fastest_setting_still_ends_a_frame(void) {
    static RrMachine m;
    RrGenesis *g = timing_machine(&m);
    assert(rr_genesis_cpu_speed(g) == 100);
    rr_genesis_set_cpu_speed(g, 50);
    assert(rr_genesis_cpu_speed(g) == 100);
    rr_genesis_set_cpu_speed(g, 1000000);
    assert(rr_genesis_cpu_speed(g) == 6400);
    uint64_t ran = rr_genesis_run_frame(g);
    assert(ran > 0 && rr_genesis_master_time(g) / RR_MASTER_PER_FRAME == 1);
    rr_genesis_free(g);
}

/* Changing speed between frames loses no time: frames still follow each other exactly. */
static void test_cpu_speed_change_keeps_frames_whole(void) {
    static RrMachine m;
    RrGenesis *g = timing_machine(&m);
    static const unsigned speeds[] = {100, 800, 200, 6400, 100, 400};
    for (uint64_t frame = 1; frame <= 60; frame++) {
        rr_genesis_set_cpu_speed(g, speeds[frame % 6]);
        rr_genesis_run_frame(g);
        uint64_t master = rr_genesis_master_time(g);
        assert(master / RR_MASTER_PER_FRAME == frame);
        assert(master % RR_MASTER_PER_FRAME < 20 * 7);
    }
    rr_genesis_free(g);
}

static void test_console_cpu_speed_survives_a_reset(void) {
    RrConsole *c = open_console();
    assert(rr_console_cpu_speed(c) == 100);
    rr_console_set_cpu_speed(c, 400);
    assert(rr_console_cpu_speed(c) == 400);
    rr_console_reset(c);
    assert(rr_console_cpu_speed(c) == 400);
    assert(rr_console_run_frame(c) == 1);
    rr_console_set_cpu_speed(c, 7);
    assert(rr_console_cpu_speed(c) == 100);
    rr_console_close(c);
}

static void put16(uint8_t *at, int value) {
    at[0] = (uint8_t)(value >> 8);
    at[1] = (uint8_t)value;
}

/* A console at scale 4 with the crisp road on, whose memory is a one-player
 * race with a sloping road and whose video memory shows striped grass on plane
 * A under the shift table that road gives. */
static RrConsole *race_console(void) {
    static const int line[3] = {200, 120, 100}, clipped[3] = {152, 120, 100};
    static const int left[3] = {-300, -100, -20}, right[3] = {300, 100, 20};
    static RrRoad road;
    static RrRoadShifts shifts;
    RrConsole *c = open_console();
    rr_console_set_scale(c, 4);
    rr_console_set_crisp_road(c, 1);
    assert(rr_console_run_frame(c) == 1); /* so the hardware is known */
    uint8_t *ram = (uint8_t *)rr_console_ram(c);
    put16(ram + 0x0302, 0xE038);
    put16(ram + 0x1CA0, 3);
    put16(ram + 0xF2B2, 152);
    put16(ram + 0xF2BE, 160);
    for (int i = 0; i < 3; i++) {
        put16(ram + 0x1E20 + 2 * i, line[i]);
        put16(ram + 0xD2B8 + 2 * i, clipped[i]);
        put16(ram + 0x1E84 + 2 * i, left[i]);
        put16(ram + 0x1F4C + 2 * i, right[i]);
    }
    RrVdp *v = &seen_hw->vdp;
    v->reg[1] = 0x44;  /* display on */
    v->reg[12] = 0x81; /* 40 cells */
    v->reg[11] = 0x03; /* a shift per line */
    v->reg[13] = 0x30; /* shift table at 0xC000 */
    v->reg[2] = 0x38;  /* plane A at 0xE000 */
    v->reg[16] = 0x01; /* planes 64 cells wide */
    v->cram[1] = 0x00E0;
    memset(&v->vram[32], 0x11, 32); /* tile 1: solid colour 1 */
    for (int row = 0; row < 32; row++)
        for (int column = 0; column < 64; column += 2) v->vram[0xE000 + (row * 64 + column) * 2 + 1] = 1;
    assert(rr_road_read(ram, &road) == 1);
    rr_road_shifts(&road, seen_hw->m->rom, seen_hw->m->rom_size, 1, &shifts);
    for (int l = 0; l < RR_CONSOLE_H; l++) {
        if (!shifts.have[l]) continue;
        put16(&v->vram[0xC000 + l * 4], shifts.a[l] >> 16);
        put16(&v->vram[0xC000 + l * 4 + 2], shifts.b[l] >> 16);
    }
    rr_console_note_road(c);
    return c;
}

/* Video memory for a race picture: striped grass on plane A under a per-line shift table. */
static void grass_and_table(const uint8_t *ram) {
    static RrRoad road;
    static RrRoadShifts shifts;
    RrVdp *v = &seen_hw->vdp;
    v->reg[1] = 0x44;
    v->reg[12] = 0x81;
    v->reg[11] = 0x03;
    v->reg[13] = 0x30;
    v->reg[2] = 0x38;
    v->reg[16] = 0x01;
    v->cram[1] = 0x00E0;
    memset(&v->vram[32], 0x11, 32);
    for (int row = 0; row < 32; row++)
        for (int column = 0; column < 64; column += 2) v->vram[0xE000 + (row * 64 + column) * 2 + 1] = 1;
    assert(rr_road_read(ram, &road) == 1);
    rr_road_shifts(&road, seen_hw->m->rom, seen_hw->m->rom_size, 1, &shifts);
    for (int l = 0; l < RR_CONSOLE_H; l++) {
        if (!shifts.have[l]) continue;
        put16(&v->vram[0xC000 + l * 4], shifts.a[l] >> 16);
        put16(&v->vram[0xC000 + l * 4 + 2], shifts.b[l] >> 16);
    }
}

/* As race_console, but the road points in memory are the game's projection of a camera
 * and a straight, flat track that are in memory too, and the console has seen both
 * moments: the projection starting and the points being turned into the picture. */
static RrConsole *projected_race_console(void) {
    static RrRoadInputs inputs;
    static RrFineRoad fine;
    static RrRoad whole;
    RrConsole *c = open_console();
    rr_console_set_scale(c, 4);
    rr_console_set_crisp_road(c, 1);
    assert(rr_console_run_frame(c) == 1);
    uint8_t *ram = (uint8_t *)rr_console_ram(c);
    put16(ram + 0x0302, 0xE038);
    put16(ram + 0x1CA0, 32);
    put16(ram + 0x0A0A, 5);
    put16(ram + 0x0A0C, 0x4000);
    put16(ram + 0x0A02, -72);
    put16(ram + 0x0A08, 60);
    put16(ram + 0xF2B2, 152);
    put16(ram + 0xF2BC, 60);
    put16(ram + 0xF2BE, 160);
    rr_console_note_road_inputs(c);
    assert(rr_road_read_inputs(ram, &inputs) == 1);
    assert(rr_road_project(&inputs, seen_hw->m->rom, seen_hw->m->rom_size, &fine, &whole) == 1);
    for (int i = 0; i < whole.count; i++) {
        put16(ram + 0x1E20 + 2 * i, whole.line[i]);
        put16(ram + 0xD2B8 + 2 * i, whole.clipped_line[i]);
        put16(ram + 0x1E84 + 2 * i, whole.left[i]);
        put16(ram + 0x1F4C + 2 * i, whole.right[i]);
        put16(ram + 0x1DBC + 2 * i, whole.centre[i]);
    }
    grass_and_table(ram);
    rr_console_note_road(c);
    return c;
}

/* Whether the scaled frame still holds the original: the last of each pixel's four, on
 * the first of its four rows. */
static int samples_back(const RrConsole *c) {
    const uint32_t *small = rr_console_frame(c), *large = rr_console_scaled_frame(c);
    for (int y = 0; y < RR_CONSOLE_H; y++)
        for (int x = 0; x < RR_CONSOLE_W; x++)
            if (large[(y * 4) * RR_CONSOLE_W * 4 + x * 4 + 3] != small[y * RR_CONSOLE_W + x]) return 0;
    return 1;
}

static int line_is_plain(const RrConsole *c, int line) {
    const uint32_t *small = rr_console_frame(c), *large = rr_console_scaled_frame(c);
    for (int r = 0; r < 4; r++)
        for (int x = 0; x < RR_CONSOLE_W * 4; x++)
            if (large[(line * 4 + r) * RR_CONSOLE_W * 4 + x] != small[line * RR_CONSOLE_W + x / 4]) return 0;
    return 1;
}

static void test_fine_road_is_on_by_default_and_counts_frames(void) {
    RrConsole *c = projected_race_console();
    assert(rr_console_fine_road(c) == 1);
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_crisp_frames(c) == 1 && rr_console_fine_frames(c) == 1);
    assert(!line_is_plain(c, 129));
    /* The fine edge is not the game's whole-pixel edge, so the road lines no longer hold
     * the original exactly; lines without road still do. */
    assert(!samples_back(c) && line_is_plain(c, 20) && line_is_plain(c, 200));
    rr_console_close(c);
}

static void test_fine_road_off_draws_the_whole_pixel_road(void) {
    RrConsole *c = projected_race_console();
    rr_console_set_fine_road(c, 0);
    assert(rr_console_fine_road(c) == 0);
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_crisp_frames(c) == 1 && rr_console_fine_frames(c) == 0);
    assert(samples_back(c) && !line_is_plain(c, 129));
    rr_console_reset(c); /* the choice is kept */
    assert(rr_console_fine_road(c) == 0);
    rr_console_close(c);
}

static void (*chosen_fine_begin)(RrGenesis *g, void *ctx);
static void choose_then_change_line_130_fine(RrGenesis *g, void *ctx) {
    chosen_fine_begin(g, ctx);
    g->vdp.vram[0xC000 + 130 * 4 + 1] ^= 4;
}

static void test_a_changed_table_entry_makes_the_line_plain_with_fine_points_too(void) {
    RrConsole *c = projected_race_console();
    chosen_fine_begin = seen_hw->hires_begin;
    seen_hw->hires_begin = choose_then_change_line_130_fine;
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_fine_frames(c) == 1);
    assert(line_is_plain(c, 130) && !line_is_plain(c, 129) && !line_is_plain(c, 131));
    rr_console_close(c);
}

/* Whether the scaled frame is the plain frame with every pixel repeated 4 x 4. */
static int plainly_enlarged(const RrConsole *c) {
    const uint32_t *small = rr_console_frame(c), *large = rr_console_scaled_frame(c);
    for (int y = 0; y < RR_CONSOLE_H * 4; y++)
        for (int x = 0; x < RR_CONSOLE_W * 4; x++)
            if (large[y * RR_CONSOLE_W * 4 + x] != small[(y / 4) * RR_CONSOLE_W + x / 4]) return 0;
    return 1;
}

static void test_crisp_road_is_drawn_when_the_table_in_video_memory_matches(void) {
    RrConsole *c = race_console();
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_crisp_frames(c) == 1);
    const uint32_t *small = rr_console_frame(c), *large = rr_console_scaled_frame(c);
    /* The last of the four pixels each original pixel became, on the first of its four
     * rows, is the original picture... */
    for (int y = 0; y < RR_CONSOLE_H; y++)
        for (int x = 0; x < RR_CONSOLE_W; x++)
            assert(large[(y * 4) * RR_CONSOLE_W * 4 + x * 4 + 3] == small[y * RR_CONSOLE_W + x]);
    /* ...and between them the grass stripes slide by parts of a pixel. */
    assert(!plainly_enlarged(c));
    rr_console_close(c);
}

static void test_falls_back_when_the_table_in_video_memory_differs(void) {
    RrConsole *c = race_console();
    seen_hw->vdp.vram[0xC000 + 130 * 4 + 1] ^= 1; /* one line of the table is another road's */
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_crisp_frames(c) == 0);
    assert(plainly_enlarged(c));
    rr_console_close(c);
}

/* The road is chosen before line 0. If the game then rewrites the shift table while the
 * frame is being drawn, the lines drawn from the new table must not get the old road's
 * fractions. */
static void (*chosen_begin)(RrGenesis *g, void *ctx);
static void choose_then_change_line_130(RrGenesis *g, void *ctx) {
    chosen_begin(g, ctx);
    g->vdp.vram[0xC000 + 130 * 4 + 1] ^= 4; /* plane A on line 130 now belongs to another road */
}

static void test_a_line_whose_table_entry_changed_after_the_choice_is_drawn_plainly(void) {
    RrConsole *c = race_console();
    chosen_begin = seen_hw->hires_begin;
    seen_hw->hires_begin = choose_then_change_line_130;
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_crisp_frames(c) == 1); /* the road was chosen: the rest of it is drawn */
    const uint32_t *small = rr_console_frame(c), *large = rr_console_scaled_frame(c);
    int plain_130 = 1, plain_129 = 1;
    for (int r = 0; r < 4; r++)
        for (int x = 0; x < RR_CONSOLE_W * 4; x++) {
            if (large[(130 * 4 + r) * RR_CONSOLE_W * 4 + x] != small[130 * RR_CONSOLE_W + x / 4]) plain_130 = 0;
            if (large[(129 * 4 + r) * RR_CONSOLE_W * 4 + x] != small[129 * RR_CONSOLE_W + x / 4]) plain_129 = 0;
        }
    assert(plain_130 && !plain_129);
    rr_console_close(c);
}

/* Finer pictures for sprites are chosen before line 0 with the video memory
 * of that moment. If video memory changes after that, the sprites and tiles
 * they describe may be gone: the lines are drawn without them. */
static const uint8_t solid_rows[4 * 3] = {0x22, 0x22, 0, 0x22, 0x22, 0, 0x22, 0x22, 0, 0x22, 0x22, 0};
static RrFinePicture one_picture = {.rows = solid_rows, .width = 4, .height = 4, .stride = 3, .size = 0x8000,
                                    .x = 40, .y = 20, .columns = 2, .rows_shown = 2};
static RrFineSprites one_sprite;
static int stamp_behind;
static void (*chosen_before_sprites)(RrGenesis *g, void *ctx);
static void choose_then_set_sprites(RrGenesis *g, void *ctx) {
    chosen_before_sprites(g, ctx);
    g->hires_sprites = &one_sprite;
    g->hires_sprites_stamp = g->vdp.vram_writes - (uint64_t)stamp_behind;
}

static RrConsole *console_with_a_fine_sprite(int behind) {
    RrConsole *c = race_console();
    RrVdp *v = &seen_hw->vdp;
    v->reg[5] = 0x50;                       /* sprite table at 0xA000 */
    v->cram[2] = 0x0E00;
    memset(&v->vram[2 * 32], 0x22, 32);     /* tile 2: solid colour 2 */
    put16(&v->vram[0xA000], 20 + 128);      /* one 1 x 1-cell sprite at (40, 20) */
    put16(&v->vram[0xA004], 0x8002);
    put16(&v->vram[0xA006], 40 + 128);
    one_sprite.pictures = &one_picture;
    for (int i = 0; i < RR_SPRITES_MAX; i++) one_sprite.picture[i] = -1;
    one_sprite.picture[0] = 0;
    stamp_behind = behind;
    chosen_before_sprites = seen_hw->hires_begin;
    seen_hw->hires_begin = choose_then_set_sprites;
    return c;
}

static void test_genesis_draws_fine_sprites_while_video_memory_is_unchanged(void) {
    RrConsole *c = console_with_a_fine_sprite(0);
    assert(rr_console_run_frame(c) == 1);
    assert(seen_hw->hires_sprites_stale_lines == 0);
    /* The half-size picture covers 2 x 2 screen pixels of the sprite's 8 x 8. */
    assert(!line_is_plain(c, 20) && !line_is_plain(c, 22) && line_is_plain(c, 28));
    rr_console_close(c);
}

static void test_genesis_drops_fine_sprites_when_video_memory_changes(void) {
    RrConsole *c = console_with_a_fine_sprite(1);
    assert(rr_console_run_frame(c) == 1);
    assert(seen_hw->hires_sprites_stale_lines == RR_CONSOLE_H);
    for (int line = 16; line < 32; line++) assert(line_is_plain(c, line));
    rr_console_close(c);
}

/* --- the frame loop draws wide ----------------------------------------- */

enum { WIDE_STRIDE = (RR_CONSOLE_W + 2 * 39) * 4 };
static uint32_t wide_frame[WIDE_STRIDE * RR_CONSOLE_H * 4];
static uint32_t narrow_frame[RR_CONSOLE_W * 4 * RR_CONSOLE_H * 4], plain_frame[RR_CONSOLE_W * 4 * RR_CONSOLE_H * 4];
static RrPixelSource wide_frame_sources[WIDE_STRIDE * RR_CONSOLE_H * 4];
static int32_t far_beyond[RR_CONSOLE_H * 4];
static RrWide frame_wide;
static RrFineSprites margin_sprites;
static int wide_change_line_130, wide_margin_stamp_behind = -1;
static void (*chosen_before_wide)(RrGenesis *g, void *ctx);
static void choose_then_go_wide(RrGenesis *g, void *ctx) {
    chosen_before_wide(g, ctx);
    if (wide_change_line_130) g->vdp.vram[0xC000 + 130 * 4 + 1] ^= 4;
    g->hires = wide_frame;
    g->hires_wide = &frame_wide;
    g->hires_beyond_a = g->hires_beyond_b = far_beyond;
    g->hires_narrow = narrow_frame;
    g->hires_sources = wide_frame_sources;
    if (wide_margin_stamp_behind >= 0) {
        g->hires_margin_sprites = &margin_sprites;
        g->hires_sprites_stamp = g->vdp.vram_writes - (uint64_t)wide_margin_stamp_behind;
    }
}

/* Plane A made 1,024 wide, holding the game's ground block (columns 352 to 671)
 * on lines 128 to 135 and nothing else. */
static void stage_ground_rows(void) {
    RrVdp *v = &seen_hw->vdp;
    v->reg[16] = 0x03;
    memset(&v->vram[0xE000], 0, 0x2000);
    for (int column = 44; column < 84; column++) v->vram[0xE000 + (16 * 128 + column) * 2 + 1] = 1;
}

/* A race console whose enlarged picture is drawn 39 pixels wider on either side
 * into wide_frame; plain_frame holds the same frame as an ordinary console draws it. */
static RrConsole *wide_race_console(int change_line_130, int margin_stamp_behind) {
    RrConsole *plain = race_console();
    stage_ground_rows();
    if (change_line_130) {
        chosen_begin = seen_hw->hires_begin;
        seen_hw->hires_begin = choose_then_change_line_130;
    }
    assert(rr_console_run_frame(plain) == 1);
    memcpy(plain_frame, rr_console_scaled_frame(plain), sizeof plain_frame);
    rr_console_close(plain);
    RrConsole *c = race_console();
    stage_ground_rows();
    memset(&frame_wide, 0, sizeof frame_wide);
    frame_wide.margin = 39;
    frame_wide.lines = 152;
    frame_wide.panel = 0x123456;
    frame_wide.block[0] = (RrWideBlock){352, 672, 24, 1, 8, -352, 320};
    frame_wide.block[1] = (RrWideBlock){352, 672, 24, 0, 8, 0, 384};
    frame_wide.ground_backdrop = -1;
    for (int i = 0; i < RR_CONSOLE_H * 4; i++) far_beyond[i] = 400 * 65536; /* grass right across */
    memset(wide_frame, 0xEE, sizeof wide_frame);
    memset(narrow_frame, 0xEE, sizeof narrow_frame);
    wide_change_line_130 = change_line_130;
    wide_margin_stamp_behind = margin_stamp_behind;
    chosen_before_wide = seen_hw->hires_begin;
    seen_hw->hires_begin = choose_then_go_wide;
    assert(rr_console_run_frame(c) == 1);
    return c;
}

static void test_genesis_draws_wide_rows_at_the_wide_stride(void) {
    RrConsole *c = wide_race_console(0, -1);
    assert(rr_console_crisp_frames(c) == 1);
    for (int row = 0; row < RR_CONSOLE_H * 4; row++) {
        const uint32_t *wide = &wide_frame[row * WIDE_STRIDE];
        /* The middle of every row is the ordinary picture's row. */
        assert(memcmp(&wide[39 * 4], &plain_frame[row * RR_CONSOLE_W * 4], RR_CONSOLE_W * 4 * sizeof *wide) == 0);
        /* Panels from line 152 down, picture above. */
        int panel = row >= 152 * 4;
        assert((wide[0] == 0x123456) == panel && (wide[WIDE_STRIDE - 1] == 0x123456) == panel);
        assert((wide_frame_sources[row * WIDE_STRIDE].layer == RR_SRC_PANEL) == panel);
        for (int x = 0; x < 39 * 4; x++) assert((wide[x] >> 24) == 0 && (wide[WIDE_STRIDE - 1 - x] >> 24) == 0);
    }
    rr_console_close(c);
}

/* What the renderer gives for one line of the finished frame's video memory. */
static void expect_wide_line(int line, int with_shifts, int with_beyond) {
    static uint32_t expected[WIDE_STRIDE * 4];
    const RrGenesis *g = seen_hw;
    rr_vdp_render_line_wide(&g->vdp, line, 4, with_shifts ? &g->hires_shift_a[line * 4] : NULL,
                            with_shifts ? &g->hires_shift_b[line * 4] : NULL,
                            with_beyond ? &far_beyond[line * 4] : NULL, with_beyond ? &far_beyond[line * 4] : NULL,
                            NULL, NULL, &frame_wide, expected, NULL);
    assert(memcmp(&wide_frame[line * 4 * WIDE_STRIDE], expected, sizeof expected) == 0);
}

static void test_beyond_is_not_used_on_a_line_whose_entry_changed(void) {
    RrConsole *c = wide_race_console(1, -1);
    assert(rr_console_crisp_frames(c) == 1);
    assert(seen_hw->hires_have[129] && seen_hw->hires_have[130]);
    /* Line 129 keeps its shifts and gets its `beyond`; line 130's entry in the table
     * changed after the road was chosen: still wide, but with neither. */
    expect_wide_line(129, 1, 1);
    expect_wide_line(130, 0, 0);
    /* (The `beyond` matters on line 129: without it the row is another.) */
    static uint32_t without[WIDE_STRIDE * 4];
    rr_vdp_render_line_wide(&seen_hw->vdp, 129, 4, &seen_hw->hires_shift_a[129 * 4], &seen_hw->hires_shift_b[129 * 4],
                            NULL, NULL, NULL, NULL, &frame_wide, without, NULL);
    assert(memcmp(&wide_frame[129 * 4 * WIDE_STRIDE], without, sizeof without) != 0);
    /* With it the grass (colour 1) runs to the right end of line 129; on line 130 it stops at the road. */
    assert(wide_frame[129 * 4 * WIDE_STRIDE + WIDE_STRIDE - 1] == rr_vdp_colour(0x00E0));
    assert(wide_frame[130 * 4 * WIDE_STRIDE + WIDE_STRIDE - 1] != rr_vdp_colour(0x00E0));
    assert(wide_frame[130 * 4 * WIDE_STRIDE] == rr_vdp_colour(0x00E0));
    /* And a line the road does not reach has no `beyond` either. */
    assert(!seen_hw->hires_have[20]);
    expect_wide_line(20, 0, 0);
    rr_console_close(c);
}

static void test_margin_sprites_are_dropped_when_video_memory_changed(void) {
    RrConsole *c = wide_race_console(0, 0);
    assert(seen_hw->hires_sprites_stale_lines == 0);
    rr_console_close(c);
    c = wide_race_console(0, 1);
    assert(seen_hw->hires_sprites_stale_lines == RR_CONSOLE_H);
    rr_console_close(c);
}

static void test_hires_narrow_is_the_picture_without_margins(void) {
    RrConsole *c = wide_race_console(0, -1);
    assert(memcmp(narrow_frame, plain_frame, sizeof plain_frame) == 0);
    rr_console_close(c);
    c = wide_race_console(1, -1);
    assert(memcmp(narrow_frame, plain_frame, sizeof plain_frame) == 0);
    rr_console_close(c);
}

/* --- the console's wide setting ----------------------------------------- */

static uint32_t narrow_kept[(RR_CONSOLE_W + 78) * 4 * RR_CONSOLE_H * 4];

static void test_wide_is_off_by_default_and_needs_crisp_fine_road_and_objects(void) {
    RrConsole *c = projected_race_console(); /* scale 4, the crisp road, fine road and fine objects on */
    assert(rr_console_wide(c) == 0 && rr_console_scaled_width(c) == 1280 && rr_console_wide_frames(c) == 0);
    rr_console_set_wide(c, 1);
    assert(rr_console_wide(c) == 1 && rr_console_scaled_width(c) == 398 * 4);
    /* Each thing it needs, taken away and given back: the setting stays, the width follows. */
    rr_console_set_crisp_road(c, 0);
    assert(rr_console_wide(c) == 1 && rr_console_scaled_width(c) == 1280);
    rr_console_set_crisp_road(c, 1);
    assert(rr_console_scaled_width(c) == 1592);
    rr_console_set_fine_road(c, 0);
    assert(rr_console_scaled_width(c) == 1280);
    rr_console_set_fine_road(c, 1);
    rr_console_set_fine_objects(c, 0);
    assert(rr_console_scaled_width(c) == 1280);
    rr_console_set_fine_objects(c, 1);
    assert(rr_console_scaled_width(c) == 1592);
    rr_console_set_scale(c, 1);
    assert(rr_console_wide(c) == 1 && rr_console_scaled_width(c) == 320);
    rr_console_set_scale(c, 2);
    assert(rr_console_scaled_width(c) == 796);
    rr_console_set_wide(c, 0);
    assert(rr_console_wide(c) == 0 && rr_console_scaled_width(c) == 640);
    rr_console_close(c);
}

static void test_wide_off_frames_are_todays(void) {
    RrConsole *plain = projected_race_console();
    assert(rr_console_run_frame(plain) == 1);
    memcpy(narrow_kept, rr_console_scaled_frame(plain), 1280 * 896 * sizeof *narrow_kept);
    rr_console_close(plain);
    RrConsole *c = projected_race_console();
    rr_console_set_wide(c, 1);
    rr_console_set_wide(c, 0);
    assert(rr_console_run_frame(c) == 1 && rr_console_wide_frames(c) == 0);
    assert(memcmp(narrow_kept, rr_console_scaled_frame(c), 1280 * 896 * sizeof *narrow_kept) == 0);
    rr_console_close(c);
}

/* Whether every row's middle is the kept narrow frame; and the colours at the two ends of a row. */
static int middle_is_kept(const RrConsole *c, int scale) {
    const uint32_t *wide = rr_console_scaled_frame(c);
    int stride = rr_console_scaled_width(c), across = RR_CONSOLE_W * scale;
    for (int row = 0; row < RR_CONSOLE_H * scale; row++)
        if (memcmp(&wide[row * stride + 39 * scale], &narrow_kept[row * across], (size_t)across * sizeof *wide) != 0)
            return 0;
    return 1;
}

static int margins_are_black(const RrConsole *c, int scale, int from_line, int to_line) {
    const uint32_t *wide = rr_console_scaled_frame(c);
    int stride = rr_console_scaled_width(c);
    for (int row = from_line * scale; row < to_line * scale; row++)
        for (int x = 0; x < 39 * scale; x++)
            if (wide[row * stride + x] != 0 || wide[row * stride + stride - 1 - x] != 0) return 0;
    return 1;
}

static void test_the_centre_of_a_wide_race_frame_is_the_narrow_frame(void) {
    RrConsole *plain = projected_race_console();
    stage_ground_rows();
    assert(rr_console_run_frame(plain) == 1);
    memcpy(narrow_kept, rr_console_scaled_frame(plain), 1280 * 896 * sizeof *narrow_kept);
    rr_console_close(plain);
    RrConsole *c = projected_race_console();
    stage_ground_rows();
    seen_hw->vdp.reg[7] = 62; /* the road's backdrop, as on a ground line */
    seen_hw->vdp.cram[62] = seen_hw->vdp.cram[0];
    rr_console_set_wide(c, 1);
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_wide_frames(c) == 1 && rr_console_fine_frames(c) == 1);
    assert(middle_is_kept(c, 4));
    /* The dashboard's lines lie between black margins; the view's lines do not: line 129
     * holds the ground block, whose grass runs on to the picture's left end. */
    assert(margins_are_black(c, 4, 152, RR_CONSOLE_H));
    const uint32_t *wide = rr_console_scaled_frame(c);
    assert(wide[129 * 4 * 1592] == rr_vdp_colour(0x00E0));
    rr_console_close(c);
}

static void test_a_menu_frame_is_the_original_between_black_margins(void) {
    RrConsole *plain = open_console();
    rr_console_set_scale(plain, 3);
    rr_console_set_crisp_road(plain, 1);
    assert(rr_console_run_frame(plain) == 1);
    memcpy(narrow_kept, rr_console_scaled_frame(plain), 960 * 672 * sizeof *narrow_kept);
    rr_console_close(plain);
    RrConsole *c = open_console();
    rr_console_set_scale(c, 3);
    rr_console_set_crisp_road(c, 1);
    rr_console_set_wide(c, 1);
    assert(rr_console_scaled_width(c) == 1194);
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_wide_frames(c) == 0);
    assert(middle_is_kept(c, 3) && margins_are_black(c, 3, 0, RR_CONSOLE_H));
    rr_console_close(c);
}

static void test_a_two_player_frame_is_the_original_between_black_margins(void) {
    RrConsole *plain = projected_race_console();
    put16((uint8_t *)rr_console_ram(plain) + 0x0304, 1); /* two players: the split screen */
    assert(rr_console_run_frame(plain) == 1);
    memcpy(narrow_kept, rr_console_scaled_frame(plain), 1280 * 896 * sizeof *narrow_kept);
    rr_console_close(plain);
    RrConsole *c = projected_race_console();
    put16((uint8_t *)rr_console_ram(c) + 0x0304, 1);
    rr_console_set_wide(c, 1);
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_wide_frames(c) == 0 && rr_console_crisp_frames(c) == 0);
    assert(middle_is_kept(c, 4) && margins_are_black(c, 4, 0, RR_CONSOLE_H));
    rr_console_close(c);
}

static void test_the_session_lists_wide_and_replays_it(void) {
    RrConsole *a = open_console();
    rr_console_set_scale(a, 3);
    rr_console_set_crisp_road(a, 1);
    rr_console_run_frame(a);
    rr_console_set_wide(a, 1);
    rr_console_run_frame(a);
    rr_console_set_wide(a, 0);
    rr_console_set_wide(a, 1);
    rr_console_run_frame(a);
    char *text = rr_console_session_text(a);
    assert(text != NULL);
    assert(strcmp(text, "rr2-session 1\n0 scale 3\n0 crisp 1\n1 wide 1\n2 wide 0\n2 wide 1\nend 3\n") == 0);
    RrConsole *b = open_console();
    char error[256] = "";
    assert(rr_console_replay_session(b, text, NULL, NULL, error, sizeof error) == 1);
    assert(rr_console_wide(b) == 1 && rr_console_scaled_width(b) == 1194);
    assert(memcmp(rr_console_scaled_frame(a), rr_console_scaled_frame(b), 1194 * 672 * sizeof(uint32_t)) == 0);
    free(text);
    rr_console_close(a);
    rr_console_close(b);
}

static void test_switching_f_o_r_scale_or_wide_mid_race_never_draws_at_a_stale_width(void) {
    RrConsole *c = projected_race_console();
    static const int scales[3] = {1, 2, 5};
    for (int round = 0; round < 2; round++)
        for (int s = 0; s < 3; s++)
            for (int bits = 0; bits < 16; bits++) {
                /* A different setter goes last each time, so each of them resizes in its turn. */
                if (round == 0) rr_console_set_scale(c, scales[s]);
                rr_console_set_wide(c, bits & 1);
                rr_console_set_crisp_road(c, (bits >> 1) & 1);
                rr_console_set_fine_road(c, (bits >> 2) & 1);
                rr_console_set_fine_objects(c, (bits >> 3) & 1);
                if (round == 1) rr_console_set_scale(c, scales[s]);
                int scale = scales[s], margin = bits == 15 && scale > 1 ? 39 : 0;
                int stride = scale > 1 ? (RR_CONSOLE_W + 2 * margin) * scale : RR_CONSOLE_W;
                assert(rr_console_scaled_width(c) == stride);
                for (int frame = 0; frame < 3; frame++) {
                    assert(rr_console_run_frame(c) == 1);
                    const uint32_t *px = rr_console_scaled_frame(c);
                    /* Every pixel of the buffer at this width was drawn as a colour. */
                    size_t count = (size_t)stride * RR_CONSOLE_H * (size_t)scale;
                    for (size_t i = 0; i < count; i += 97) assert((px[i] >> 24) == 0);
                    assert((px[count - 1] >> 24) == 0);
                }
            }
    rr_console_close(c);
}

static void test_wide_survives_a_reset(void) {
    RrConsole *c = projected_race_console();
    rr_console_set_wide(c, 1);
    rr_console_reset(c);
    assert(rr_console_wide(c) == 1 && rr_console_scaled_width(c) == 1592);
    assert(rr_console_run_frame(c) == 1);
    assert(margins_are_black(c, 4, 0, RR_CONSOLE_H)); /* the power-on picture is no race */
    rr_console_close(c);
}

/* A race picture with one object: a 16 x 16 picture drawn at half size, so
 * its sprite is 8 x 8 at (96, 52), well above the road. The game's drawing of
 * it is put in video memory where the game's block copies would put it. */
static int object_x = 100; /* where stage_one_object stands its picture */
static void stage_one_object(RrConsole *c) {
    static RrObjectSet staged;
    static uint8_t work[0x10000];
    uint8_t *ram = (uint8_t *)rr_console_ram(c);
    put16(ram + 0x27A6, 0x00FF); put16(ram + 0x27A8, 0x3E40); /* the sprite list being built */
    put16(ram + 0x27CE, 0x00FF); put16(ram + 0x27D0, 0x4742); /* the sprite pictures being built */
    put16(ram + 0xCE86, 0x634);
    put16(ram + 0xDC30, 0);
    put16(ram + 0xDC34, 0);
    put16(ram + 0xCEB8, object_x);    /* x */
    put16(ram + 0xCEB8 + 2, 60);      /* foot line */
    put16(ram + 0xCEB8 + 4, 0x2000);  /* half size */
    put16(ram + 0xCEB8 + 6, 0x98);
    put16(ram + 0xCEB8 + 8, 1);       /* picture 1 */
    put16(ram + 0xCEB8 + 0x0A, 0x1000);
    put16(ram + 0x08D6, 0);
    put16(ram + 0x08D6 + 2, 0xFFFF);
    assert(rr_objects_draw(ram, seen_hw->m->rom, seen_hw->m->rom_size, &staged, work));
    assert(staged.sprites == 1 && staged.pictures == 1);
    RrVdp *v = &seen_hw->vdp;
    v->reg[5] = 0x62; /* sprite table at 0xC400 */
    for (int i = 1; i < 16; i++) v->cram[16 + i] = (uint16_t)(((i & 3) * 2) << 1 | (((i >> 2) & 3) * 2) << 5 | 0x200);
    memcpy(&v->vram[0xC400], staged.list, sizeof staged.list);
    memcpy(&v->vram[0x634 * 32], staged.tile_bytes, (size_t)staged.tiles * 32);
}

static int object_lines_are_plain(const RrConsole *c) {
    for (int line = 52; line < 60; line++)
        if (!line_is_plain(c, line)) return 0;
    return 1;
}

static void test_fine_objects_are_on_by_default_and_can_be_switched(void) {
    RrConsole *c = open_console();
    assert(rr_console_fine_objects(c) == 1 && rr_console_fine_object_frames(c) == 0);
    rr_console_set_fine_objects(c, 0);
    assert(rr_console_fine_objects(c) == 0);
    rr_console_reset(c); /* the choice is kept */
    assert(rr_console_fine_objects(c) == 0);
    rr_console_set_fine_objects(c, 7);
    assert(rr_console_fine_objects(c) == 1);
    rr_console_close(c);
}

/* The console's own path for the margin objects: the tracker makes them when the
 * set is noted, and the frame draws them beside the screen. */
static void test_console_draws_an_object_across_each_side_of_the_screen(void) {
    for (int side = 0; side < 2; side++) {
        RrConsole *c = projected_race_console();
        rr_console_set_wide(c, 1);
        put16((uint8_t *)rr_console_ram(c) + 0xF2B2, 152);
        object_x = side == 0 ? 2 : 318; /* 8 wide at half size: -2 to 5, or 314 to 321 */
        stage_one_object(c);
        object_x = 100;
        rr_console_note_objects(c);
        assert(rr_console_run_frame(c) == 1);
        assert(rr_console_fine_object_frames(c) == 1 && rr_console_wide_frames(c) == 1);
        /* Every pixel of the ROM picture, 2 x 2 each, right across the screen's side:
         * 32 output columns from where the picture's left end stands. */
        const uint32_t *wide = rr_console_scaled_frame(c);
        int stride = rr_console_scaled_width(c), left = (39 + (side == 0 ? -2 : 314)) * 4;
        assert(stride == 1592);
        for (int r = 0; r < 32; r++)
            for (int column = 0; column < 32; column++) {
                int colour = ((r / 2) * 8 + column / 4) % 15 + 1;
                assert(wide[(52 * 4 + r) * stride + left + column] == rr_vdp_colour(seen_hw->vdp.cram[16 + colour]));
            }
        rr_console_close(c);
    }
}

static void test_console_draws_a_noted_set_fine_and_off_draws_it_plain(void) {
    RrConsole *c = race_console();
    stage_one_object(c);
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_fine_object_frames(c) == 0 && object_lines_are_plain(c)); /* nothing noted yet */
    rr_console_note_objects(c);
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_fine_object_frames(c) == 1);
    /* Half size: the sprite keeps one picture pixel in four; the fine picture shows them all. */
    assert(!object_lines_are_plain(c));
    assert(line_is_plain(c, 51) && line_is_plain(c, 60));
    /* At scale 4 the half-size picture is 32 x 32 output pixels at (96, 52) x 4: every pixel
     * of the ROM picture, 2 x 2 each. */
    const uint32_t *large = rr_console_scaled_frame(c);
    for (int r = 0; r < 32; r++)
        for (int column = 0; column < 32; column++) {
            int colour = ((r / 2) * 8 + column / 4) % 15 + 1;
            assert(large[(52 * 4 + r) * RR_CONSOLE_W * 4 + 96 * 4 + column] ==
                   rr_vdp_colour(seen_hw->vdp.cram[16 + colour]));
        }
    rr_console_set_fine_objects(c, 0);
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_fine_object_frames(c) == 1 && object_lines_are_plain(c));
    rr_console_set_fine_objects(c, 1);
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_fine_object_frames(c) == 2 && !object_lines_are_plain(c));
    /* Video memory now shows something else: back to the tiles. */
    seen_hw->vdp.vram[0x634 * 32 + 3] ^= 0x10;
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_fine_object_frames(c) == 2 && object_lines_are_plain(c));
    rr_console_close(c);
}

static void test_console_notes_the_objects_when_the_game_reaches_its_drawing(void) {
    RrConsole *c = race_console();
    stage_one_object(c);
    visit_object_drawing = 1; /* during this frame the game passes 0xA4EE once */
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_fine_object_frames(c) >= 1 && !object_lines_are_plain(c));
    rr_console_close(c);
    /* Switched off, the console does not note them. */
    c = race_console();
    stage_one_object(c);
    rr_console_set_fine_objects(c, 0);
    visit_object_drawing = 1;
    assert(rr_console_run_frame(c) == 1);
    rr_console_set_fine_objects(c, 1);
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_fine_object_frames(c) == 0 && object_lines_are_plain(c));
    rr_console_close(c);
}

static void test_fine_objects_need_the_crisp_picture(void) {
    RrConsole *c = race_console();
    stage_one_object(c);
    rr_console_note_objects(c);
    rr_console_set_crisp_road(c, 0);
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_fine_object_frames(c) == 0 && plainly_enlarged(c));
    rr_console_close(c);
}

static void test_reset_forgets_noted_objects(void) {
    RrConsole *c = race_console();
    stage_one_object(c);
    rr_console_note_objects(c);
    rr_console_reset(c);
    rr_console_set_crisp_road(c, 1);
    assert(rr_console_run_frame(c) == 1); /* so the new hardware is known */
    stage_one_object(c);                  /* the same picture in video memory, but nothing noted */
    seen_hw->vdp.reg[1] = 0x44;
    seen_hw->vdp.reg[12] = 0x81;
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_fine_object_frames(c) == 0 && object_lines_are_plain(c));
    rr_console_close(c);
}

/* ---- in-between pictures ---- */

static int dashes_wrong; /* game_update: stage dash arrays the model does not make */
static int tree_size = 0x2000; /* game_update: the object's size (0x4000 is natural) */
static RrConsole *between_console(int mode);

/* The game makes an update: the camera `sideways` on a straight road, a tree at x. Both
 * moments are noted (the projection and the drawing) and video memory shows it. */
static void game_update(RrConsole *c, int sideways, int x) {
    static RrRoadInputs inputs;
    static RrFineRoad fine;
    static RrRoad whole;
    static RrObjectSet staged;
    static uint8_t work[0x10000];
    uint8_t *ram = (uint8_t *)rr_console_ram(c);
    put16(ram + 0x0302, 0xE038);
    put16(ram + 0x1CA0, 32);
    put16(ram + 0x0A0A, 4); /* an even segment: the stretch of road nearest the camera is painted */
    put16(ram + 0x0A0C, 0x4000);
    put16(ram + 0x0A02, sideways);
    put16(ram + 0x0A08, 60);
    put16(ram + 0xF2B2, 152);
    put16(ram + 0xF2BC, 60);
    put16(ram + 0xF2BE, 160);
    rr_console_note_road_inputs(c);
    assert(rr_road_read_inputs(ram, &inputs) == 1);
    assert(rr_road_project(&inputs, seen_hw->m->rom, seen_hw->m->rom_size, &fine, &whole) == 1);
    for (int i = 0; i < whole.count; i++) {
        put16(ram + 0x1E20 + 2 * i, whole.line[i]);
        put16(ram + 0xD2B8 + 2 * i, whole.clipped_line[i]);
        put16(ram + 0x1E84 + 2 * i, whole.left[i]);
        put16(ram + 0x1F4C + 2 * i, whole.right[i]);
        put16(ram + 0x1DBC + 2 * i, whole.centre[i]);
    }
    grass_and_table(ram);
    rr_console_note_road(c);
    /* The game's description of the centre line, as the port's model makes it. */
    static RrRoad as_read;
    static RrDashLines lines;
    assert(rr_road_read(ram, &as_read) == 1);
    rr_dashes_lines(&as_read, 0x00044000, rr_dashes_stop_line(ram), seen_hw->m->rom, seen_hw->m->rom_size, &lines);
    for (int l = 0; l < RR_CONSOLE_H; l++) {
        put16(ram + 0x2EDE + 2 * l, lines.start[l]);
        put16(ram + 0x309E + 2 * l, lines.width[l] + (dashes_wrong ? 1 : 0));
    }
    rr_console_note_dashes(c);
    /* One roadside object: picture 1 at half size, its sprite 8 x 8 at (x - 4, 52). */
    put16(ram + 0x27A6, 0x00FF); put16(ram + 0x27A8, 0x3E40);
    put16(ram + 0x27CE, 0x00FF); put16(ram + 0x27D0, 0x4742);
    put16(ram + 0xCE86, 0x634);
    put16(ram + 0xDC30, 0);
    put16(ram + 0xDC34, 0);
    put16(ram + 0xDC42, 1);
    put16(ram + 0x04F8, 0xFFFF);
    put16(ram + 0xCEB8, x);
    put16(ram + 0xCEB8 + 2, 60);
    put16(ram + 0xCEB8 + 4, tree_size);
    put16(ram + 0xCEB8 + 6, 0x98);
    put16(ram + 0xCEB8 + 8, 1);
    put16(ram + 0xCEB8 + 0x0A, 0x1000);
    put16(ram + 0xCEB8 + 0x0E, 500);  /* distance: a roadside object */
    put16(ram + 0xCEB8 + 0x10, 0x28);
    put16(ram + 0xCEB8 + 0x14, 9);    /* of track segment 9 */
    put16(ram + 0x08D6, 0);
    put16(ram + 0x08D6 + 2, 0xFFFF);
    assert(rr_objects_draw(ram, seen_hw->m->rom, seen_hw->m->rom_size, &staged, work));
    RrVdp *v = &seen_hw->vdp;
    v->reg[5] = 0x62;
    for (int i = 1; i < 16; i++) v->cram[16 + i] = (uint16_t)(((i & 3) * 2) << 1 | (((i >> 2) & 3) * 2) << 5 | 0x200);
    memcpy(&v->vram[0xC400], staged.list, sizeof staged.list);
    memcpy(&v->vram[0x634 * 32], staged.tile_bytes, (size_t)staged.tiles * 32);
    /* One of the game's dash sprites after the object: 8 x 8 of colour 12 in set 3, at (20, 140). */
    v->cram[0x3C] = 0x00EE;
    memset(&v->vram[0x7F0 * 32], 0xCC, 32);
    v->vram[0xC400 + 3] = 1;
    put16(&v->vram[0xC408], 140 + 128);
    v->vram[0xC40A] = 0;
    v->vram[0xC40B] = 0;
    put16(&v->vram[0xC40C], 0x6000 | 0x7F0);
    put16(&v->vram[0xC40E], 20 + 128);
    rr_console_note_objects(c);
}


/* How many output pixels of row `row` show the dashes' colour, and the first such column. */
static int dash_pixels(const RrConsole *c, int row, int *first) {
    const uint32_t *large = rr_console_scaled_frame(c);
    uint32_t colour = rr_vdp_colour(seen_hw->vdp.cram[0x3C]);
    int count = 0;
    *first = -1;
    for (int column = 0; column < RR_CONSOLE_W * 4; column++) {
        if (large[row * RR_CONSOLE_W * 4 + column] != colour) continue;
        if (count++ == 0) *first = column;
    }
    return count;
}

/* The rows of the road with a strip of the dashes' colour near the road's middle. */
static int strip_rows(const RrConsole *c, int *a_row, int *its_first) {
    int rows = 0, first;
    for (int row = 100 * 4; row < 152 * 4; row++) {
        if (dash_pixels(c, row, &first) == 0 || first < 100 * 4) continue; /* (the sprite at x 20 is not it) */
        if (rows++ == 0) {
            *a_row = row;
            *its_first = first;
        }
    }
    return rows;
}

static void test_dashes_are_strips_when_the_fine_road_is_drawn(void) {
    int row, first, unused;
    RrConsole *c = between_console(0);
    dashes_wrong = 0;
    game_update(c, -72, 100);
    assert(rr_console_run_frame(c) == 1);
    /* Strips on the painted segments' rows; the game's dash sprite is left out. */
    assert(strip_rows(c, &row, &first) > 40);
    assert(dash_pixels(c, 141 * 4, &unused) == 0 || unused >= 100 * 4);
    /* On a sloping centre line the strip moves by output pixels from row to row. */
    int next_first;
    assert(dash_pixels(c, row + 1, &next_first) > 0 && next_first != first && abs(next_first - first) < 4);
    /* With the fine road points off the dashes are the game's sprites again. */
    rr_console_set_fine_road(c, 0);
    assert(rr_console_run_frame(c) == 1);
    assert(strip_rows(c, &row, &first) == 0);
    assert(dash_pixels(c, 141 * 4, &first) == 32 && first == 20 * 4);
    rr_console_close(c);
}

static void test_dashes_stay_sprites_when_the_description_is_not_proven(void) {
    int row, first;
    RrConsole *c = between_console(0);
    dashes_wrong = 1; /* the game's arrays are not what the model makes */
    game_update(c, -72, 100);
    dashes_wrong = 0;
    assert(rr_console_run_frame(c) == 1);
    assert(strip_rows(c, &row, &first) == 0);
    assert(dash_pixels(c, 141 * 4, &first) == 32 && first == 20 * 4);
    rr_console_close(c);
}

static void test_dashes_slide_with_in_between_pictures(void) {
    int row, first, later;
    RrConsole *c = between_console(RR_BETWEEN_PREDICT);
    dashes_wrong = 0;
    game_update(c, -72, 100);
    for (int i = 0; i < 4; i++) assert(rr_console_run_frame(c) == 1);
    game_update(c, -64, 104);
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_between_frames(c) == 1 && strip_rows(c, &row, &first) > 40);
    assert(rr_console_run_frame(c) == 1); /* a quarter of an update on: the camera is further across */
    assert(dash_pixels(c, row, &later) > 0 && later != first);
    assert(dash_pixels(c, 141 * 4, &first) == 0 || first >= 100 * 4); /* and no sprite beside the strips */
    rr_console_close(c);
}


static RrConsole *between_console(int mode) {
    RrConsole *c = open_console();
    rr_console_set_scale(c, 4);
    rr_console_set_crisp_road(c, 1);
    rr_console_set_between(c, mode);
    assert(rr_console_run_frame(c) == 1); /* so the hardware is known */
    return c;
}

enum { FRAME_BYTES = RR_CONSOLE_W * 4 * RR_CONSOLE_H * 4 * 4 };
static uint32_t kept_frame[RR_CONSOLE_W * 4 * RR_CONSOLE_H * 4];

/* The output column where the tree's picture begins on its first row, or -1. */
static int tree_starts_at(const RrConsole *c) {
    const uint32_t *large = rr_console_scaled_frame(c);
    uint32_t first = rr_vdp_colour(seen_hw->vdp.cram[16 + 1]); /* the picture's first pixel is colour 1 */
    for (int column = 0; column < RR_CONSOLE_W * 4; column++)
        if (large[(52 * 4) * RR_CONSOLE_W * 4 + column] == first) return column;
    return -1;
}

static void test_between_is_off_by_default_in_the_library(void) {
    RrConsole *c = open_console();
    assert(rr_console_between(c) == RR_BETWEEN_OFF && rr_console_between_frames(c) == 0);
    rr_console_set_between(c, RR_BETWEEN_BLEND);
    assert(rr_console_between(c) == RR_BETWEEN_BLEND);
    rr_console_reset(c); /* the choice is kept */
    assert(rr_console_between(c) == RR_BETWEEN_BLEND);
    rr_console_set_between(c, 17); /* not a mode */
    rr_console_set_between(c, -1);
    assert(rr_console_between(c) == RR_BETWEEN_BLEND);
    rr_console_close(c);
}

static void test_between_draws_in_between_frames(void) {
    RrConsole *c = between_console(RR_BETWEEN_PREDICT);
    game_update(c, -72, 100);
    for (int i = 0; i < 4; i++) assert(rr_console_run_frame(c) == 1);
    assert(rr_console_between_frames(c) == 0); /* one update is not a pair */
    assert(tree_starts_at(c) == 96 * 4);
    game_update(c, -64, 104);
    assert(rr_console_run_frame(c) == 1);
    /* The frame the update arrives in is the update itself. */
    assert(rr_console_between_frames(c) == 1 && tree_starts_at(c) == 100 * 4);
    /* And then the motion is carried on: four pixels in four frames, one a frame. */
    for (int i = 1; i <= 3; i++) {
        assert(rr_console_run_frame(c) == 1);
        assert(rr_console_between_frames(c) == (uint64_t)(1 + i));
        assert(tree_starts_at(c) == (100 + i) * 4);
    }
    rr_console_close(c);
}

static void test_between_off_frames_are_todays(void) {
    /* The same game on two consoles, one predicting. The frame an update arrives in is the
     * same picture on both; the one after is not. */
    for (int pass = 0; pass < 2; pass++) {
        RrConsole *c = between_console(pass == 0 ? RR_BETWEEN_OFF : RR_BETWEEN_PREDICT);
        game_update(c, -72, 100);
        for (int i = 0; i < 4; i++) assert(rr_console_run_frame(c) == 1);
        game_update(c, -64, 104);
        assert(rr_console_run_frame(c) == 1);
        if (pass == 0) {
            assert(rr_console_between_frames(c) == 0);
            memcpy(kept_frame, rr_console_scaled_frame(c), FRAME_BYTES);
            assert(rr_console_run_frame(c) == 1);
            assert(memcmp(kept_frame, rr_console_scaled_frame(c), FRAME_BYTES) == 0); /* today: it stands still */
        } else {
            assert(memcmp(kept_frame, rr_console_scaled_frame(c), FRAME_BYTES) == 0);
            assert(rr_console_run_frame(c) == 1);
            assert(memcmp(kept_frame, rr_console_scaled_frame(c), FRAME_BYTES) != 0);
            assert(!line_is_plain(c, 129)); /* the road moved too */
        }
        rr_console_close(c);
    }
}

static void test_blend_is_behind_the_newest_update(void) {
    RrConsole *c = between_console(RR_BETWEEN_BLEND);
    game_update(c, -72, 100);
    for (int i = 0; i < 4; i++) assert(rr_console_run_frame(c) == 1);
    game_update(c, -64, 104);
    assert(rr_console_run_frame(c) == 1);
    /* Four frames behind: the frame the second update arrives in shows the first. */
    assert(rr_console_between_frames(c) == 1 && tree_starts_at(c) == 96 * 4);
    assert(rr_console_run_frame(c) == 1);
    assert(tree_starts_at(c) == 97 * 4);
    rr_console_close(c);
}

static void test_switching_modes_mid_race_never_uses_stale_updates(void) {
    RrConsole *c = between_console(RR_BETWEEN_PREDICT);
    game_update(c, -72, 100);
    for (int i = 0; i < 4; i++) assert(rr_console_run_frame(c) == 1);
    game_update(c, -64, 104);
    for (int i = 0; i < 2; i++) assert(rr_console_run_frame(c) == 1);
    uint64_t drawn = rr_console_between_frames(c);
    assert(drawn == 2);
    rr_console_set_between(c, RR_BETWEEN_OFF);
    for (int i = 0; i < 3; i++) assert(rr_console_run_frame(c) == 1);
    assert(rr_console_between_frames(c) == drawn && tree_starts_at(c) == 100 * 4);
    rr_console_set_between(c, RR_BETWEEN_BLEND);
    for (int i = 0; i < 3; i++) assert(rr_console_run_frame(c) == 1);
    assert(rr_console_between_frames(c) == drawn); /* the update on screen is known again, but it is one */
    game_update(c, -56, 108);
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_between_frames(c) == drawn + 1);
    /* Switching the fine objects or the crisp road off and on forgets them too. */
    rr_console_set_fine_objects(c, 0);
    rr_console_set_fine_objects(c, 1);
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_between_frames(c) == drawn + 1);
    game_update(c, -48, 112); /* with the one on screen, noted again, that is a pair */
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_between_frames(c) == drawn + 2);
    rr_console_set_crisp_road(c, 0);
    rr_console_set_crisp_road(c, 1);
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_between_frames(c) == drawn + 2);
    rr_console_reset(c);
    assert(rr_console_between_frames(c) == 0 && rr_console_between(c) == RR_BETWEEN_BLEND);
    rr_console_close(c);
}

static void test_between_needs_the_fine_objects(void) {
    RrConsole *c = between_console(RR_BETWEEN_PREDICT);
    rr_console_set_fine_objects(c, 0);
    game_update(c, -72, 100);
    for (int i = 0; i < 4; i++) assert(rr_console_run_frame(c) == 1);
    game_update(c, -64, 104);
    for (int i = 0; i < 3; i++) assert(rr_console_run_frame(c) == 1);
    assert(rr_console_between_frames(c) == 0);
    rr_console_close(c);
}

static void test_soft_objects_are_off_by_default_and_soften_small_pictures(void) {
    static uint32_t sharp[RR_CONSOLE_W * 4 * 16];
    RrConsole *c = between_console(0);
    assert(rr_console_soft_objects(c) == 0);
    tree_size = 0x0800; /* an eighth of its natural size: at scale 4, four picture pixels to an output pixel */
    game_update(c, -72, 100);
    tree_size = 0x2000;
    assert(rr_console_run_frame(c) == 1);
    memcpy(sharp, rr_console_scaled_frame(c) + 56 * 4 * RR_CONSOLE_W * 4, sizeof sharp);
    rr_console_set_soft_objects(c, 1);
    assert(rr_console_soft_objects(c) == 1 && rr_console_run_frame(c) == 1);
    const uint32_t *soft = rr_console_scaled_frame(c) + 56 * 4 * RR_CONSOLE_W * 4;
    assert(memcmp(sharp, soft, sizeof sharp) != 0); /* the object's lines (56 to 59) changed */
    /* A softened pixel is a mix: not one of the sixteen colours the picture is made of. */
    int mixed = 0;
    for (size_t i = 0; i < sizeof sharp / sizeof sharp[0]; i++) {
        if (soft[i] == sharp[i]) continue;
        int in_palette = 0;
        for (int k = 0; k < 64; k++) in_palette |= soft[i] == rr_vdp_colour(seen_hw->vdp.cram[k]);
        mixed += !in_palette;
    }
    assert(mixed > 0);
    rr_console_reset(c); /* the choice is kept */
    assert(rr_console_soft_objects(c) == 1);
    rr_console_set_soft_objects(c, 0);
    assert(rr_console_soft_objects(c) == 0);
    rr_console_close(c);
}

static void test_soft_objects_leave_a_half_size_object_at_scale_4_alone(void) {
    static uint32_t sharp[RR_CONSOLE_W * 4 * RR_CONSOLE_H * 4];
    RrConsole *c = between_console(0);
    game_update(c, -72, 100); /* half size at scale 4: two output pixels to a picture pixel */
    assert(rr_console_run_frame(c) == 1);
    memcpy(sharp, rr_console_scaled_frame(c), sizeof sharp);
    rr_console_set_soft_objects(c, 1);
    assert(rr_console_run_frame(c) == 1);
    assert(memcmp(sharp, rr_console_scaled_frame(c), sizeof sharp) == 0);
    rr_console_close(c);
}

static void test_soft_objects_soften_in_between_frames_too(void) {
    static uint32_t sharp[RR_CONSOLE_W * 4 * 16];
    for (int pass = 0; pass < 2; pass++) {
        RrConsole *c = between_console(RR_BETWEEN_PREDICT);
        rr_console_set_soft_objects(c, pass);
        tree_size = 0x0800;
        game_update(c, -72, 100);
        for (int i = 0; i < 4; i++) assert(rr_console_run_frame(c) == 1);
        game_update(c, -64, 104);
        tree_size = 0x2000;
        for (int i = 0; i < 2; i++) assert(rr_console_run_frame(c) == 1);
        assert(rr_console_between_frames(c) == 2);
        const uint32_t *rows = rr_console_scaled_frame(c) + 56 * 4 * RR_CONSOLE_W * 4;
        if (pass == 0) memcpy(sharp, rows, sizeof sharp);
        else assert(memcmp(sharp, rows, sizeof sharp) != 0);
        rr_console_close(c);
    }
}

static void test_menu_frame_is_a_plain_enlargement(void) {
    RrConsole *c = race_console();
    put16((uint8_t *)rr_console_ram(c) + 0x0302, 0x1234); /* not the race's frame callback */
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_crisp_frames(c) == 0);
    assert(plainly_enlarged(c));
    rr_console_close(c);
}

static void test_crisp_road_off_is_a_plain_enlargement(void) {
    RrConsole *c = race_console();
    rr_console_set_crisp_road(c, 0);
    assert(rr_console_run_frame(c) == 1);
    assert(rr_console_crisp_frames(c) == 0 && plainly_enlarged(c));
    rr_console_close(c);
}

static void test_scale_1_is_the_plain_frame(void) {
    RrConsole *c = open_console();
    assert(rr_console_scale(c) == 1 && rr_console_scaled_frame(c) == rr_console_frame(c));
    rr_console_set_scale(c, 99);
    assert(rr_console_scale(c) == RR_CONSOLE_SCALE_MAX);
    rr_console_set_scale(c, 3);
    rr_console_reset(c); /* the scale is kept */
    assert(rr_console_scale(c) == 3 && rr_console_run_frame(c) == 1);
    assert(rr_console_scaled_frame(c) != rr_console_frame(c));
    rr_console_set_scale(c, 0);
    assert(rr_console_scale(c) == 1 && rr_console_scaled_frame(c) == rr_console_frame(c));
    rr_console_close(c);
}

/* ---- the session: what was done to the console since it was opened ---- */

static int frames_seen;
static void count_frame(void *ctx, RrConsole *c) {
    (void)c;
    (*(int *)ctx)++;
}

static void test_session_lists_what_was_done_and_when(void) {
    RrConsole *c = open_console();
    assert(rr_console_session_frames(c) == 0);
    rr_console_set_scale(c, 4);
    rr_console_set_crisp_road(c, 1);
    rr_console_set_buttons(c, 0, RR_PAD_START);
    rr_console_set_buttons(c, 0, RR_PAD_START); /* no change: not listed again */
    for (int i = 0; i < 3; i++) rr_console_run_frame(c);
    rr_console_set_buttons(c, 0, RR_PAD_LEFT | RR_PAD_RIGHT); /* listed as sent to the game */
    rr_console_set_buttons(c, 1, RR_PAD_A);
    rr_console_set_buttons(c, 7, RR_PAD_A); /* no such port */
    rr_console_set_cpu_speed(c, 200);
    rr_console_run_frame(c);
    rr_console_reset(c);
    rr_console_set_between(c, 2);
    rr_console_set_between(c, 9); /* ignored by the console: not listed */
    rr_console_set_fine_road(c, 0);
    rr_console_set_fine_objects(c, 0);
    rr_console_set_soft_objects(c, 1);
    rr_console_set_sound(c, 1); /* changes neither the game nor the picture: not listed */
    rr_console_run_frame(c);
    rr_console_run_frame(c);
    assert(rr_console_session_frames(c) == 6 && rr_console_frames(c) == 2);
    char *text = rr_console_session_text(c);
    char expected[512];
    snprintf(expected, sizeof expected,
             "rr2-session 1\n0 scale 4\n0 crisp 1\n0 pad 0 %02X\n3 pad 0 %02X\n3 pad 1 %02X\n3 speed 200\n"
             "4 reset\n4 between 2\n4 fine-road 0\n4 fine-objects 0\n4 soft-objects 1\nend 6\n",
             (unsigned)RR_PAD_START, (unsigned)RR_PAD_RIGHT, (unsigned)RR_PAD_A);
    assert(text != NULL);
    if (strcmp(text, expected) != 0) fprintf(stderr, "got:\n%s", text);
    assert(strcmp(text, expected) == 0);
    free(text);
    rr_console_close(c);
}

static void test_replaying_a_session_gives_the_same_console(void) {
    RrConsole *a = open_console();
    rr_console_set_scale(a, 3);
    rr_console_set_crisp_road(a, 1);
    for (int i = 0; i < 5; i++) rr_console_run_frame(a);
    rr_console_set_buttons(a, 0, RR_PAD_B | RR_PAD_UP);
    rr_console_set_cpu_speed(a, 400);
    rr_console_set_soft_objects(a, 1);
    rr_console_set_between(a, 1);
    for (int i = 0; i < 4; i++) rr_console_run_frame(a);
    rr_console_reset(a);
    rr_console_set_fine_road(a, 0);
    for (int i = 0; i < 7; i++) rr_console_run_frame(a);
    char *text = rr_console_session_text(a);
    assert(text != NULL);

    RrConsole *b = open_console();
    char error[200];
    frames_seen = 0;
    assert(rr_console_replay_session(b, text, count_frame, &frames_seen, error, sizeof error) == 1);
    assert(error[0] == 0 && frames_seen == 16);
    assert(rr_console_session_frames(b) == 16 && rr_console_frames(b) == 7);
    assert(rr_console_buttons(b, 0) == (RR_PAD_B | RR_PAD_UP) && rr_console_cpu_speed(b) == 400);
    assert(rr_console_scale(b) == 3 && rr_console_crisp_road(b) && !rr_console_fine_road(b));
    assert(rr_console_soft_objects(b) && rr_console_between(b) == 1);
    assert(memcmp(rr_console_ram(a), rr_console_ram(b), 65536) == 0);
    assert(memcmp(rr_console_scaled_frame(a), rr_console_scaled_frame(b),
                  sizeof(uint32_t) * RR_CONSOLE_W * RR_CONSOLE_H * 9) == 0);
    /* And the replayed console's own list is the same list. */
    char *again = rr_console_session_text(b);
    assert(again != NULL && strcmp(again, text) == 0);
    free(again);
    free(text);
    rr_console_close(a);
    rr_console_close(b);
}

static void test_replay_reaches_a_halt_and_goes_on_after_the_reset(void) {
    RrConsole *a = open_console();
    rr_console_run_frame(a);
    ((uint8_t *)rr_console_ram(a))[0] = 1; /* the test's way to make the game stop; not in a session */
    assert(rr_console_run_frame(a) == 0);
    rr_console_reset(a);
    rr_console_run_frame(a);
    char *text = rr_console_session_text(a);
    assert(text != NULL && strcmp(text, "rr2-session 1\n1 reset\nend 2\n") == 0);
    RrConsole *b = open_console();
    char error[200];
    assert(rr_console_replay_session(b, text, NULL, NULL, error, sizeof error) == 1);
    assert(rr_console_session_frames(b) == 2 && rr_console_frames(b) == 1);
    free(text);
    rr_console_close(a);
    rr_console_close(b);
}

static void replay_fails(const char *text, const char *reason) {
    RrConsole *c = open_console();
    char error[200] = "";
    assert(rr_console_replay_session(c, text, NULL, NULL, error, sizeof error) == 0);
    if (strstr(error, reason) == NULL) fprintf(stderr, "error was: %s (wanted: %s)\n", error, reason);
    assert(strstr(error, reason) != NULL);
    rr_console_close(c);
}

static void test_replay_refuses_text_that_is_not_a_session(void) {
    replay_fails("", "not a session");
    replay_fails("hello\n", "not a session");
    replay_fails("rr2-session 2\nend 0\n", "not a session");
    replay_fails("rr2-session 1\n0 dance 1\nend 1\n", "line 2");
    replay_fails("rr2-session 1\n0 pad 5 00\nend 1\n", "line 2");
    replay_fails("rr2-session 1\n0 speed\nend 1\n", "line 2");
    replay_fails("rr2-session 1\n3 speed 200\n2 speed 100\nend 4\n", "line 3"); /* goes back in time */
    replay_fails("rr2-session 1\n3 speed 200\nend 2\n", "line 3");
    replay_fails("rr2-session 1\n0 speed 200\n", "no end line");
    replay_fails("rr2-session 1\nend 1\n0 speed 200\n", "line 3"); /* something after the end */
}

static void test_replay_needs_a_console_just_opened(void) {
    RrConsole *c = open_console();
    rr_console_run_frame(c);
    char error[200];
    assert(rr_console_replay_session(c, "rr2-session 1\nend 3\n", NULL, NULL, error, sizeof error) == 0);
    assert(strstr(error, "just opened") != NULL && rr_console_session_frames(c) == 1);
    rr_console_close(c);
}

static void test_replay_stops_where_the_game_stops(void) {
    RrConsole *c = open_console();
    ((uint8_t *)rr_console_ram(c))[0] = 1;
    char error[200];
    assert(rr_console_replay_session(c, "rr2-session 1\nend 3\n", NULL, NULL, error, sizeof error) == 0);
    assert(strstr(error, "halted") != NULL);
    rr_console_close(c);
}

int main(void) {
    write_roms();
    test_session_lists_what_was_done_and_when();
    test_replaying_a_session_gives_the_same_console();
    test_replay_reaches_a_halt_and_goes_on_after_the_reset();
    test_replay_refuses_text_that_is_not_a_session();
    test_replay_needs_a_console_just_opened();
    test_replay_stops_where_the_game_stops();
    test_sound_is_off_as_opened();
    test_sound_is_handed_out_at_the_asked_rate();
    test_a_faster_game_makes_no_more_sound();
    test_sound_stays_on_through_a_reset();
    test_sound_never_asked_for_does_not_pile_up();
    test_small_takes_do_not_pile_up_either();
    test_sound_can_be_turned_off_and_on_mid_run();
    test_sound_does_not_change_the_game();
    test_crisp_road_is_drawn_when_the_table_in_video_memory_matches();
    test_falls_back_when_the_table_in_video_memory_differs();
    test_a_line_whose_table_entry_changed_after_the_choice_is_drawn_plainly();
    test_genesis_draws_wide_rows_at_the_wide_stride();
    test_beyond_is_not_used_on_a_line_whose_entry_changed();
    test_margin_sprites_are_dropped_when_video_memory_changed();
    test_hires_narrow_is_the_picture_without_margins();
    test_wide_is_off_by_default_and_needs_crisp_fine_road_and_objects();
    test_wide_off_frames_are_todays();
    test_the_centre_of_a_wide_race_frame_is_the_narrow_frame();
    test_a_menu_frame_is_the_original_between_black_margins();
    test_a_two_player_frame_is_the_original_between_black_margins();
    test_the_session_lists_wide_and_replays_it();
    test_switching_f_o_r_scale_or_wide_mid_race_never_draws_at_a_stale_width();
    test_wide_survives_a_reset();
    test_console_draws_an_object_across_each_side_of_the_screen();
    test_fine_road_is_on_by_default_and_counts_frames();
    test_fine_road_off_draws_the_whole_pixel_road();
    test_a_changed_table_entry_makes_the_line_plain_with_fine_points_too();
    test_menu_frame_is_a_plain_enlargement();
    test_between_is_off_by_default_in_the_library();
    test_between_draws_in_between_frames();
    test_between_off_frames_are_todays();
    test_blend_is_behind_the_newest_update();
    test_switching_modes_mid_race_never_uses_stale_updates();
    test_between_needs_the_fine_objects();
    test_soft_objects_are_off_by_default_and_soften_small_pictures();
    test_soft_objects_leave_a_half_size_object_at_scale_4_alone();
    test_soft_objects_soften_in_between_frames_too();
    test_dashes_are_strips_when_the_fine_road_is_drawn();
    test_dashes_stay_sprites_when_the_description_is_not_proven();
    test_dashes_slide_with_in_between_pictures();
    test_crisp_road_off_is_a_plain_enlargement();
    test_fine_objects_are_on_by_default_and_can_be_switched();
    test_console_draws_a_noted_set_fine_and_off_draws_it_plain();
    test_console_notes_the_objects_when_the_game_reaches_its_drawing();
    test_fine_objects_need_the_crisp_picture();
    test_reset_forgets_noted_objects();
    test_genesis_draws_fine_sprites_while_video_memory_is_unchanged();
    test_genesis_drops_fine_sprites_when_video_memory_changes();
    test_scale_1_is_the_plain_frame();
    test_frame_length_does_not_drift();
    test_line_interrupt_is_raised_just_past_position_205();
    test_interrupt_late_in_a_shift_multiply_or_divide_waits_one_instruction();
    test_interrupt_step_of_long_compare_and_indexed_lea();
    test_a_waiting_interrupt_is_not_delayed_by_a_newer_raise();
    test_no_line_interrupt_before_the_counter_is_programmed();
    test_cpu_speed_100_is_the_original();
    test_cpu_speed_800_runs_eight_times_the_instructions_per_frame();
    test_cpu_speed_keeps_the_frame_interrupt_on_line_224();
    test_cpu_speed_is_clamped_and_the_fastest_setting_still_ends_a_frame();
    test_cpu_speed_change_keeps_frames_whole();
    test_console_cpu_speed_survives_a_reset();
    test_frame_resumes_from_wherever_the_clock_is_moved_to();
    test_frame_interrupt_is_raised_shortly_after_line_224_starts();
    test_source_map_follows_a_mid_frame_change();
    test_open_matching_rom_and_run_frames();
    test_open_missing_file_names_the_path();
    test_open_wrong_rom_names_both_checksums();
    test_open_error_fits_a_small_buffer();
    test_fatal_error_halts_instead_of_exiting();
    test_reset_clears_a_halt();
    test_opposite_directions_are_never_sent_together();
    test_buttons_survive_a_reset();
    test_fatal_handler_is_removed_after_a_frame();
    remove(rom_path);
    remove(other_path);
    puts("all console tests passed");
    return 0;
}
