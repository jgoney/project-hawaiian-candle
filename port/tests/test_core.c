/* Unit tests for the port runtime core. Run with: make -C port test */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rr_machine.h"
#include "rr_mem.h"
#include "rr_run.h"

/* The generated code normally provides these. */
const uint32_t rr_code_rom_crc32 = 0x12345678;
static void insn_nop(RrMachine *m) { m->cpu.pc += 2; m->cpu.cycles += 4; }
RrInsnFn rr_lookup(uint32_t pc) { return pc == 0x200 ? insn_nop : NULL; }

static uint8_t rom[0x400];

typedef struct {
    RrHw hw;
    uint32_t addr[8];
    int size[8];
    int count;
} FakeHw;

static uint16_t fake_read(RrHw *hw, uint32_t addr, int size) {
    FakeHw *f = (FakeHw *)hw;
    f->addr[f->count] = addr;
    f->size[f->count] = size;
    f->count++;
    return 0xABCD;
}
static void fake_write(RrHw *hw, uint32_t addr, int size, uint16_t v) {
    (void)hw; (void)addr; (void)size; (void)v;
}

static RrMachine *machine(void) {
    static RrMachine m;
    memset(&m, 0, sizeof m);
    memset(rom, 0, sizeof rom);
    rom[30 * 4 + 2] = 0x11; rom[30 * 4 + 3] = 0xB8; /* level 6 vector = 0x11B8 */
    m.rom = rom;
    m.rom_size = sizeof rom;
    m.rom_crc32 = rr_code_rom_crc32;
    return &m;
}

static void test_ram_is_big_endian(void) {
    RrMachine *m = machine();
    rr_write32(m, 0xFF0000, 0x11223344);
    assert(m->ram[0] == 0x11 && m->ram[1] == 0x22 && m->ram[2] == 0x33 && m->ram[3] == 0x44);
    assert(rr_read16(m, 0xFF0002) == 0x3344);
}

static void test_ram_mirrors(void) {
    RrMachine *m = machine();
    rr_write8(m, 0xE01234, 0x5A);
    assert(rr_read8(m, 0xFF1234) == 0x5A);
}

static void test_address_masks_to_24_bits(void) {
    RrMachine *m = machine();
    m->ram[0] = 0x77;
    assert(rr_read8(m, 0xFFFF0000) == 0x77);
}

static void test_long_hw_read_is_two_words_high_first(void) {
    RrMachine *m = machine();
    FakeHw f = {.hw = {.read = fake_read, .write = fake_write}};
    m->hw = &f.hw;
    assert(rr_read32(m, 0xC00000) == 0xABCDABCD);
    assert(f.count == 2);
    assert(f.addr[0] == 0xC00000 && f.addr[1] == 0xC00002);
    assert(f.size[0] == 2 && f.size[1] == 2);
}

static void test_interrupt_entry(void) {
    RrMachine *m = machine();
    m->cpu.pc = 0x1234;
    m->cpu.sr = 0x2000;
    m->cpu.a[7] = 0xFFFF00;
    rr_interrupt(m, 6);
    assert(m->cpu.pc == 0x11B8);
    assert(m->cpu.sr == 0x2600);
    assert(m->cpu.a[7] == 0xFFFEFA);
    assert(rr_read16(m, m->cpu.a[7]) == 0x2000);
    assert(rr_read32(m, m->cpu.a[7] + 2) == 0x1234);
}

/* Autovectored interrupts wait for the 68000's E clock (one tenth of the CPU
 * clock), so the handler always starts on the same phase of it. Fitted to
 * MAME: entry takes 49 to 58 cycles and ends when the cycle count is 5 mod 10. */
static void test_interrupt_entry_time_follows_the_e_clock(void) {
    static const struct { uint64_t before, after; } cases[] = {
        {1006, 1055}, {1005, 1055}, {1000, 1055}, {1007, 1065}, {1009, 1065}, {1016, 1065},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        RrMachine *m = machine();
        m->cpu.sr = 0x2000;
        m->cpu.a[7] = 0xFFFF00;
        m->cpu.cycles = cases[i].before;
        rr_interrupt(m, 6);
        assert(m->cpu.cycles == cases[i].after);
    }
}

static void test_step_runs_one_instruction_and_counts_it(void) {
    RrMachine *m = machine();
    m->cpu.pc = 0x200;
    rr_step(m);
    assert(m->cpu.pc == 0x202 && m->cpu.insns == 1 && m->cpu.cycles == 4);
}

static void test_save_load_round_trip(void) {
    RrMachine *m = machine();
    m->cpu.d[3] = 0xDEADBEEF;
    m->cpu.a[7] = 0xFFFF00;
    m->cpu.pc = 0x200;
    m->cpu.usp = 0x00FF8000;
    m->cpu.sr = 0x2704;
    m->cpu.cycles = 123456789012ULL;
    m->cpu.insns = 42;
    m->ram[0x1C08] = 5;
    FILE *f = tmpfile();
    assert(rr_save(m, f) == 0);
    RrCpu cpu = m->cpu;
    memset(&m->cpu, 0xEE, sizeof m->cpu);
    memset(m->ram, 0xEE, sizeof m->ram);
    rewind(f);
    assert(rr_load(m, f) == 0);
    assert(memcmp(&cpu, &m->cpu, sizeof cpu) == 0);
    assert(m->ram[0x1C08] == 5 && m->ram[0] == 0);
    fclose(f);
}

static void test_load_rejects_other_rom(void) {
    RrMachine *m = machine();
    FILE *f = tmpfile();
    assert(rr_save(m, f) == 0);
    rewind(f);
    m->rom_crc32 ^= 1;
    assert(rr_load(m, f) != 0);
    fclose(f);
}

static void test_load_rejects_bad_magic(void) {
    RrMachine *m = machine();
    FILE *f = tmpfile();
    fputs("NOTASAVEFILE....", f);
    rewind(f);
    assert(rr_load(m, f) != 0);
    fclose(f);
}

static void test_crc32_matches_standard_check_value(void) {
    assert(rr_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926);
}

/* Run fn in a child; require exit status 2 and `needle` on stderr. */
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
    assert(strstr(buf, "RR2FATAL") != NULL);
    if (strstr(buf, needle) == NULL) {
        fprintf(stderr, "expected '%s' in: %s\n", needle, buf);
        assert(0);
    }
}

static void untranslated_pc(void) {
    RrMachine *m = machine();
    m->cpu.pc = 0x200;
    rr_step(m);          /* executes at 0x200, moves to 0x202 */
    rr_step(m);          /* 0x202 is not translated */
}

static const char *write_log(const char *text) {
    static char path[] = "/tmp/rr_hw_XXXXXX";
    strcpy(path, "/tmp/rr_hw_XXXXXX");
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    close(fd);
    return path;
}

static void replay_address_mismatch(void) {
    RrMachine *m = machine();
    m->hw = rr_hw_replay_open(write_log("R C00004 2 3400\n"), m);
    rr_read16(m, 0xC00008);
}

static void replay_exhausted(void) {
    RrMachine *m = machine();
    m->hw = rr_hw_replay_open(write_log("R C00004 2 3400\n"), m);
    rr_read16(m, 0xC00004);
    rr_read16(m, 0xC00004);
}

static void replay_write_value_mismatch(void) {
    RrMachine *m = machine();
    m->hw = rr_hw_replay_open(write_log("W C00004 2 8004\n"), m);
    rr_write16(m, 0xC00004, 0x8005);
}

static void rom_write(void) {
    RrMachine *m = machine();
    rr_write8(m, 0x000100, 1);
}

static void test_replay_returns_recorded_values_in_order(void) {
    RrMachine *m = machine();
    m->hw = rr_hw_replay_open(write_log("R C00004 2 3400\nW A11100 2 0100\nR A10001 1 A0\n"), m);
    assert(rr_hw_replay_remaining(m->hw) == 3);
    assert(rr_read16(m, 0xC00004) == 0x3400);
    rr_write16(m, 0xA11100, 0x0100);
    assert(rr_hw_replay_position(m->hw) == 2);
    assert(rr_read8(m, 0xA10001) == 0xA0);
    assert(rr_hw_replay_remaining(m->hw) == 0);
    rr_hw_replay_seek(m->hw, 2);
    assert(rr_read8(m, 0xA10001) == 0xA0);
    m->hw->close(m->hw);
}

/* A tool can watch a range of work RAM and be told of every write into it. */
static struct { uint32_t offset; int size; uint32_t value; } watched[8];
static int watched_count;
static void note_write(RrMachine *m, uint32_t offset, int size, uint32_t value, void *ctx) {
    assert(ctx == (void *)watched && m != NULL);
    if (watched_count < 8) {
        watched[watched_count].offset = offset;
        watched[watched_count].size = size;
        watched[watched_count].value = value;
    }
    watched_count++;
}
static void test_ram_write_watch_reports_only_the_range(void) {
    RrMachine *m = machine();
    m->ram_watch = note_write;
    m->ram_watch_ctx = watched;
    m->ram_watch_start = 0x1000;
    m->ram_watch_end = 0x1010; /* one past the last watched byte */
    watched_count = 0;
    rr_write8(m, 0xFF0FFF, 0x11);       /* just below */
    rr_write8(m, 0xFF1000, 0x22);       /* first byte */
    rr_write16(m, 0xFF0FFF + 1, 0x3344);
    rr_write16(m, 0xFF100E, 0x5566);    /* last word */
    rr_write16(m, 0xFF1010, 0x7788);    /* just above */
    rr_write32(m, 0xFF0FFE, 0x99AABBCC); /* a long straddling the start: its second word is inside */
    assert(watched_count == 4);
    assert(watched[0].offset == 0x1000 && watched[0].size == 1 && watched[0].value == 0x22);
    assert(watched[1].offset == 0x1000 && watched[1].size == 2 && watched[1].value == 0x3344);
    assert(watched[2].offset == 0x100E && watched[2].value == 0x5566);
    assert(watched[3].offset == 0x1000 && watched[3].size == 2 && watched[3].value == 0xBBCC);
    assert(m->ram[0x1000] == 0xBB && m->ram[0x0FFF] == 0xAA); /* and the writes still happen */
    m->ram_watch = NULL;
    rr_write8(m, 0xFF1000, 0x01);
    assert(watched_count == 4);
}

int main(void) {
    test_ram_is_big_endian();
    test_ram_mirrors();
    test_address_masks_to_24_bits();
    test_long_hw_read_is_two_words_high_first();
    test_interrupt_entry();
    test_interrupt_entry_time_follows_the_e_clock();
    test_step_runs_one_instruction_and_counts_it();
    test_save_load_round_trip();
    test_load_rejects_other_rom();
    test_load_rejects_bad_magic();
    test_crc32_matches_standard_check_value();
    test_replay_returns_recorded_values_in_order();
    test_ram_write_watch_reports_only_the_range();
    expect_fatal(untranslated_pc, "000202");
    expect_fatal(untranslated_pc, "000200");   /* names the instruction that jumped */
    expect_fatal(replay_address_mismatch, "C00008");
    expect_fatal(replay_address_mismatch, "C00004");
    expect_fatal(replay_exhausted, "exhausted");
    expect_fatal(replay_write_value_mismatch, "8005");
    expect_fatal(rom_write, "ROM");
    puts("all core tests passed");
    return 0;
}
