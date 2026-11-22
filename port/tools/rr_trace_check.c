/* rr-trace-check: replay a MAME trace window through the translated code.
 *
 * Usage: rr-trace-check --rom ROM --window DIR [--cycles-out FILE]
 *                       [--slice N] [--save-restore-at K]
 *                       [--shadow-hw [--frame-out FILE]]
 *
 * --slice N            return to the caller after every N instructions, to
 *                      show that execution can stop between any two.
 * --save-restore-at K  before instruction K, save the whole state to a file,
 *                      wipe the machine, and reload it.
 * --shadow-hw          run the port's own hardware model beside the recording.
 *                      The recording stays authoritative; the model sees every
 *                      access and its answers to reads are compared and
 *                      counted per address ("SHADOW" lines; high_differ counts
 *                      word reads whose high byte differs, which for the HV
 *                      counter is the line). Differences are reported, not
 *                      fatal: read values depend on timing.
 * --frame-out FILE     with --shadow-hw, write the model's final picture (PPM).
 *
 * The window directory holds trace.txt, hw.txt, ram-start.bin, ram-end.bin and
 * optionally handoff-NN.bin (see mame/README.md). The machine starts from the
 * first trace line and ram-start.bin. Before every instruction its registers
 * are compared with the trace; hardware reads come from hw.txt; interrupts are
 * injected where the trace shows a handler being entered.
 *
 * Exit status: 0 match; 1 divergence from the trace; 2 usage or input error,
 * or a fatal runtime error (RR2FATAL: untranslated address, hardware replay
 * out of step, unimplemented instruction).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rr_genesis.h"
#include "rr_mem.h"
#include "rr_run.h"

#define RR206_CRC32 0x0876E992u
#ifndef HANDOFF_PC
#define HANDOFF_PC 0x00DF48u /* game-loop timestep handoff; overridable for tests */
#endif
#define MAX_HANDOFF_FILES 100
#define RAM_SIZE 0x10000

typedef struct {
    uint32_t pc, sr, d[8], a[8];
    uint64_t cycles;
} Step;

#define MAX_SHADOW_ADDRESSES 64

/* Hardware that answers from the recording and also drives the model. */
typedef struct {
    RrHw hw; /* must be first */
    RrHw *recording;
    RrGenesis *model;
    struct {
        uint32_t addr;
        uint64_t reads, differ;
        uint64_t high_differ; /* word reads whose high byte differs */
    } stats[MAX_SHADOW_ADDRESSES];
    int count;
} Shadow;

/* The bits of a hardware read that are defined. The Z80 bus request register
 * drives only bit 0 of its byte; MAME returns open-bus leftovers in the rest. */
static uint16_t shadow_defined_bits(uint32_t addr, int size) {
    if ((addr & 0xFFFFFE) == 0xA11100) return size == 2 ? 0x0100 : (addr & 1) ? 0 : 0x01;
    return 0xFFFF;
}

static uint16_t shadow_read(RrHw *hw, uint32_t addr, int size) {
    Shadow *s = (Shadow *)hw;
    uint16_t recorded = s->recording->read(s->recording, addr, size);
    uint16_t modelled = s->model->hw.read(&s->model->hw, addr, size);
    uint16_t defined = shadow_defined_bits(addr, size);
    int i = 0;
    while (i < s->count && s->stats[i].addr != addr) i++;
    if (i == s->count && s->count < MAX_SHADOW_ADDRESSES) {
        s->stats[s->count].addr = addr;
        s->count++;
    }
    if (i < s->count) {
        s->stats[i].reads++;
        if ((recorded ^ modelled) & defined) s->stats[i].differ++;
        if (size == 2 && ((recorded ^ modelled) & defined & 0xFF00)) s->stats[i].high_differ++;
    }
    return recorded;
}

static void shadow_write(RrHw *hw, uint32_t addr, int size, uint16_t value) {
    Shadow *s = (Shadow *)hw;
    s->recording->write(s->recording, addr, size, value);
    s->model->hw.write(&s->model->hw, addr, size, value);
}

/* Everything the checker carries between instructions. */
typedef struct {
    RrMachine m;
    FILE *trace;
    FILE *cycles_out;
    const char *window;
    Step cur, next;
    unsigned long line; /* line number of `next` in trace.txt */
    uint64_t index;     /* index of `cur` */
    uint64_t interrupts, handoffs;
    uint32_t vector[8];
    int have_next;
    Shadow *shadow;     /* NULL unless --shadow-hw */
    RrHw *recording;    /* the replayed hardware log */
    int end_seen;       /* the trace's "E <count>" line has been read */
    uint64_t end_count; /* number of steps that line promises */
} Check;

static _Noreturn void input_error(const char *fmt, ...) {
    va_list args;
    fprintf(stderr, "rr-trace-check: ");
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    exit(2);
}

static void path_in(char *out, size_t n, const char *dir, const char *name) {
    if ((size_t)snprintf(out, n, "%s/%s", dir, name) >= n) input_error("path too long");
}

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) input_error("cannot open %s", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *data = malloc(n > 0 ? (size_t)n : 1);
    if (data == NULL || fread(data, 1, (size_t)n, f) != (size_t)n) input_error("cannot read %s", path);
    fclose(f);
    *size = (size_t)n;
    return data;
}

/* Parse "T pc sr d0..d7 a0..a7 cycles". Returns 0 at the trace's end line
 * "E <count>". Reaching end of file without that line means the trace was cut
 * short, which is an error even when the cut falls between two lines. */
static int read_step(Check *ck, Step *step) {
    char buf[512];
    if (ck->end_seen) return 0;
    if (fgets(buf, sizeof buf, ck->trace) == NULL) {
        input_error("trace.txt line %lu: missing end line; the trace is truncated", ck->line + 1);
    }
    ck->line++;
    char *p = buf, *end;
    uint64_t v[19];
    if (p[0] == 'E' && p[1] == ' ') {
        ck->end_count = strtoull(p + 2, &end, 10);
        if (end == p + 2 || *end != '\n') {
            input_error("trace.txt line %lu: malformed end line", ck->line);
        }
        ck->end_seen = 1;
        return 0;
    }
    if (p[0] != 'T' || p[1] != ' ') input_error("trace.txt line %lu: expected 'T'", ck->line);
    p += 2;
    for (int i = 0; i < 19; i++) {
        v[i] = strtoull(p, &end, i == 18 ? 10 : 16);
        if (end == p) input_error("trace.txt line %lu: truncated (field %d)", ck->line, i + 2);
        p = end;
    }
    while (*p == ' ') p++;
    if (*p != '\n') input_error("trace.txt line %lu: truncated or malformed", ck->line);
    step->pc = (uint32_t)v[0];
    step->sr = (uint32_t)v[1];
    for (int i = 0; i < 8; i++) step->d[i] = (uint32_t)v[2 + i];
    for (int i = 0; i < 8; i++) step->a[i] = (uint32_t)v[10 + i];
    step->cycles = v[18];
    return 1;
}

static _Noreturn void diverge(const Check *ck, const char *field, uint32_t want, uint32_t got) {
    printf("DIVERGE insn=%llu pc=%06X field=%s want=%08X got=%08X prev_pc=%06X\n",
           (unsigned long long)ck->index, ck->cur.pc & 0xFFFFFF, field, want, got,
           ck->m.cpu.last_pc & 0xFFFFFF);
    exit(1);
}

static void compare_registers(const Check *ck) {
    static const char *const dn[8] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7"};
    static const char *const an[8] = {"a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"};
    const RrCpu *c = &ck->m.cpu;
    const Step *s = &ck->cur;
    if ((s->pc & 0xFFFFFF) != (c->pc & 0xFFFFFF)) diverge(ck, "pc", s->pc, c->pc);
    if (s->sr != c->sr) diverge(ck, "sr", s->sr, c->sr);
    for (int i = 0; i < 8; i++) {
        if (s->d[i] != c->d[i]) diverge(ck, dn[i], s->d[i], c->d[i]);
    }
    for (int i = 0; i < 8; i++) {
        if (s->a[i] != c->a[i]) diverge(ck, an[i], s->a[i], c->a[i]);
    }
}

/* Compare all of RAM with a 64 KiB image; a missing optional file is skipped. */
static int compare_ram(const Check *ck, const char *name, const char *label, int required) {
    char path[1024];
    path_in(path, sizeof path, ck->window, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        if (required) input_error("cannot open %s", path);
        return 0;
    }
    static uint8_t want[RAM_SIZE];
    if (fread(want, 1, RAM_SIZE, f) != RAM_SIZE) input_error("%s is not 65536 bytes", path);
    fclose(f);
    long first = -1;
    unsigned count = 0;
    for (long i = 0; i < RAM_SIZE; i++) {
        if (want[i] != ck->m.ram[i]) {
            if (first < 0) first = i;
            count++;
        }
    }
    if (first >= 0) {
        printf("RAMDIFF %s first_offset=%04lX want=%02X got=%02X count=%u insn=%llu\n", label,
               first, want[first], ck->m.ram[first], count, (unsigned long long)ck->index);
        exit(1);
    }
    return 1;
}

static void check_handoff(Check *ck) {
    if ((ck->cur.pc & 0xFFFFFF) != HANDOFF_PC) return;
    char name[32], label[32];
    snprintf(name, sizeof name, "handoff-%02llu.bin", (unsigned long long)ck->handoffs);
    snprintf(label, sizeof label, "handoff=%02llu", (unsigned long long)ck->handoffs);
    if (compare_ram(ck, name, label, 0)) ck->handoffs++;
}

/* The level of the interrupt entered before `next`, or 0. Called after the
 * instruction at `cur` has run.
 *
 * `next` is a handler entry when the machine would not have gone there by
 * itself, its PC is a vector target, and its interrupt mask equals that
 * vector's level. The trace's previous mask is no guide: it may be lower
 * (ordinary entry), higher (the instruction lowered it from 7 with an
 * interrupt pending) or equal (an RTE followed at once by the next interrupt). */
static int interrupt_before_next(const Check *ck) {
    int level = (ck->next.sr >> 8) & 7;
    uint32_t next_pc = ck->next.pc & 0xFFFFFF;
    if (level == 0 || (ck->m.cpu.pc & 0xFFFFFF) == next_pc) return 0;
    if (level <= ((ck->m.cpu.sr >> 8) & 7)) return 0;
    return next_pc == ck->vector[level] ? level : 0;
}

/* Check and execute up to `limit` instructions. Returns 0 when the trace is finished. */
static int run_slice(Check *ck, uint64_t limit) {
    for (uint64_t n = 0; n < limit; n++) {
        compare_registers(ck);
        check_handoff(ck);
        if (!ck->have_next) return 0;
        uint64_t before = ck->m.cpu.cycles;
        rr_step(&ck->m);
        uint64_t cost = ck->m.cpu.cycles - before;
        int level = interrupt_before_next(ck);
        uint64_t before_entry = ck->m.cpu.cycles;
        if (level != 0) {
            rr_interrupt(&ck->m, level);
            ck->interrupts++;
        }
        if (ck->cycles_out != NULL) {
            /* index, pc, cycles for the instruction, cycles for an interrupt entry after it */
            fprintf(ck->cycles_out, "%llu %06X %llu %llu\n", (unsigned long long)ck->index,
                    ck->cur.pc & 0xFFFFFF, (unsigned long long)cost,
                    (unsigned long long)(ck->m.cpu.cycles - before_entry));
        }
        ck->cur = ck->next;
        ck->index++;
        ck->have_next = read_step(ck, &ck->next);
    }
    return 1;
}

/* What the checker itself must remember across a save, besides the machine. */
typedef struct {
    Step cur, next;
    unsigned long line;
    uint64_t index, interrupts, handoffs;
    int have_next, end_seen;
    uint64_t end_count;
    long trace_offset;
    uint64_t hw_position;
} Bookmark;

/* Save everything to a temporary file, destroy the live state, and load it back. */
static void save_and_restore(Check *ck) {
    FILE *f = tmpfile();
    if (f == NULL) input_error("cannot create a temporary save file");
    Bookmark mark = {ck->cur, ck->next, ck->line, ck->index, ck->interrupts, ck->handoffs,
                     ck->have_next, ck->end_seen, ck->end_count, ftell(ck->trace),
                     rr_hw_replay_position(ck->recording)};
    if (rr_save(&ck->m, f) != 0 || fwrite(&mark, sizeof mark, 1, f) != 1) {
        input_error("cannot write the save file");
    }

    memset(&ck->m.cpu, 0xEE, sizeof ck->m.cpu);
    memset(ck->m.ram, 0xEE, sizeof ck->m.ram);
    memset(&ck->cur, 0xEE, sizeof ck->cur);
    memset(&ck->next, 0xEE, sizeof ck->next);
    ck->line = 0;
    ck->index = ck->interrupts = ck->handoffs = 0;
    ck->have_next = ck->end_seen = 0;
    ck->end_count = 0;
    rewind(ck->trace);
    rr_hw_replay_seek(ck->recording, 0);

    rewind(f);
    memset(&mark, 0, sizeof mark);
    if (rr_load(&ck->m, f) != 0 || fread(&mark, sizeof mark, 1, f) != 1) {
        input_error("cannot read the save file back");
    }
    fclose(f);
    ck->cur = mark.cur;
    ck->next = mark.next;
    ck->line = mark.line;
    ck->index = mark.index;
    ck->interrupts = mark.interrupts;
    ck->handoffs = mark.handoffs;
    ck->have_next = mark.have_next;
    ck->end_seen = mark.end_seen;
    ck->end_count = mark.end_count;
    fseek(ck->trace, mark.trace_offset, SEEK_SET);
    rr_hw_replay_seek(ck->recording, (size_t)mark.hw_position);
    fprintf(stderr, "restored at insn=%llu\n", (unsigned long long)ck->index);
}

static uint64_t parse_count(const char *text, const char *option) {
    char *end;
    uint64_t value = strtoull(text, &end, 10);
    if (end == text || *end != 0) input_error("%s needs a number, got '%s'", option, text);
    return value;
}

int main(int argc, char **argv) {
    const char *rom_path = NULL, *window = NULL, *cycles_path = NULL, *frame_path = NULL;
    int shadow_hw = 0;
    uint64_t slice = UINT64_MAX, restore_at = UINT64_MAX;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc) rom_path = argv[++i];
        else if (strcmp(argv[i], "--window") == 0 && i + 1 < argc) window = argv[++i];
        else if (strcmp(argv[i], "--cycles-out") == 0 && i + 1 < argc) cycles_path = argv[++i];
        else if (strcmp(argv[i], "--shadow-hw") == 0) shadow_hw = 1;
        else if (strcmp(argv[i], "--frame-out") == 0 && i + 1 < argc) frame_path = argv[++i];
        else if (strcmp(argv[i], "--slice") == 0 && i + 1 < argc) {
            slice = parse_count(argv[++i], "--slice");
            if (slice == 0) input_error("--slice must be at least 1");
        } else if (strcmp(argv[i], "--save-restore-at") == 0 && i + 1 < argc) {
            restore_at = parse_count(argv[++i], "--save-restore-at");
        }
        else input_error("unknown or incomplete option %s", argv[i]);
    }
    if (rom_path == NULL || window == NULL) {
        input_error("usage: rr-trace-check --rom ROM --window DIR [--cycles-out FILE] "
                    "[--slice N] [--save-restore-at K] [--shadow-hw [--frame-out FILE]]");
    }

    if (shadow_hw && restore_at != UINT64_MAX) {
        input_error("--shadow-hw cannot be combined with --save-restore-at");
    }
    if (frame_path != NULL && !shadow_hw) input_error("--frame-out needs --shadow-hw");

    Check *ck = calloc(1, sizeof *ck);
    if (ck == NULL) input_error("out of memory");
    ck->window = window;

    size_t rom_size;
    uint8_t *rom = read_file(rom_path, &rom_size);
    uint32_t crc = rr_crc32(rom, rom_size);
    if (crc != rr_code_rom_crc32) {
        input_error("ROM CRC-32 %08X does not match the translated code (%08X)", crc,
                    rr_code_rom_crc32);
    }
#ifndef RR_ANY_ROM
    if (crc != RR206_CRC32) input_error("ROM CRC-32 %08X is not RR206 (%08X)", crc, RR206_CRC32);
#endif
    ck->m.rom = rom;
    ck->m.rom_size = (uint32_t)rom_size;
    ck->m.rom_crc32 = crc;
    for (int level = 1; level < 8; level++) {
        ck->vector[level] = rr_read32(&ck->m, (uint32_t)(24 + level) * 4) & 0xFFFFFF;
    }

    char path[1024];
    size_t ram_size;
    path_in(path, sizeof path, window, "ram-start.bin");
    uint8_t *ram = read_file(path, &ram_size);
    if (ram_size != RAM_SIZE) input_error("%s is not 65536 bytes", path);
    memcpy(ck->m.ram, ram, RAM_SIZE);
    free(ram);

    path_in(path, sizeof path, window, "hw.txt");
    ck->recording = rr_hw_replay_open(path, &ck->m);
    ck->m.hw = ck->recording;
    if (shadow_hw) {
        ck->shadow = calloc(1, sizeof *ck->shadow);
        if (ck->shadow == NULL) input_error("out of memory");
        ck->shadow->hw.read = shadow_read;
        ck->shadow->hw.write = shadow_write;
        ck->shadow->recording = ck->recording;
        ck->shadow->model = rr_genesis_new(&ck->m);
        /* The checker's cycle count is MAME's own, so the model's video timing
         * needs no origin of its own. */
        ck->m.hw = &ck->shadow->hw;
    }

    path_in(path, sizeof path, window, "trace.txt");
    ck->trace = fopen(path, "r");
    if (ck->trace == NULL) input_error("cannot open %s", path);
    if (!read_step(ck, &ck->cur)) input_error("%s is empty", path);
    ck->have_next = read_step(ck, &ck->next);

    if (cycles_path != NULL) {
        ck->cycles_out = fopen(cycles_path, "w");
        if (ck->cycles_out == NULL) input_error("cannot write %s", cycles_path);
    }

    for (int i = 0; i < 8; i++) ck->m.cpu.d[i] = ck->cur.d[i];
    for (int i = 0; i < 8; i++) ck->m.cpu.a[i] = ck->cur.a[i];
    ck->m.cpu.pc = ck->cur.pc;
    ck->m.cpu.sr = (uint16_t)ck->cur.sr;
    /* Start the cycle count where MAME's was, so anything that depends on its
     * phase (interrupt entry time) lines up with the recording. */
    ck->m.cpu.cycles = ck->cur.cycles;

    clock_t started = clock();
    int restored = restore_at == UINT64_MAX;
    for (;;) {
        uint64_t limit = slice;
        if (!restored) {
            if (ck->index == restore_at) {
                save_and_restore(ck);
                restored = 1;
            } else if (restore_at - ck->index < limit) {
                limit = restore_at - ck->index;
            }
        }
        if (!run_slice(ck, limit)) break;
    }
    if (!restored) {
        input_error("--save-restore-at %llu was never reached (trace has %llu instructions)",
                    (unsigned long long)restore_at, (unsigned long long)ck->index);
    }
    double seconds = (double)(clock() - started) / CLOCKS_PER_SEC;

    if (ck->end_count != ck->index + 1) {
        input_error("trace.txt end line says %llu steps but %llu were read",
                    (unsigned long long)ck->end_count, (unsigned long long)(ck->index + 1));
    }
    /* Every handoff image in the window must have been compared. A gap in the
     * numbering would otherwise silently skip all the later ones. */
    for (unsigned n = (unsigned)ck->handoffs; n < MAX_HANDOFF_FILES; n++) {
        char name[32];
        snprintf(name, sizeof name, "handoff-%02u.bin", n);
        path_in(path, sizeof path, window, name);
        FILE *f = fopen(path, "rb");
        if (f != NULL) {
            fclose(f);
            printf("HANDOFFS %s exists but only %llu handoff images were compared\n", name,
                   (unsigned long long)ck->handoffs);
            return 1;
        }
    }
    compare_ram(ck, "ram-end.bin", "end", 1);
    size_t left = rr_hw_replay_remaining(ck->recording);
    if (left != 0) {
        printf("HWLEFT %zu hardware accesses in the recording were never made\n", left);
        return 1;
    }
    if (ck->cycles_out != NULL) fclose(ck->cycles_out);
    if (ck->shadow != NULL) {
        for (int i = 0; i < ck->shadow->count; i++) {
            printf("SHADOW addr=%06X reads=%llu differ=%llu high_differ=%llu\n",
                   ck->shadow->stats[i].addr, (unsigned long long)ck->shadow->stats[i].reads,
                   (unsigned long long)ck->shadow->stats[i].differ,
                   (unsigned long long)ck->shadow->stats[i].high_differ);
        }
        if (frame_path != NULL && rr_vdp_write_ppm(&ck->shadow->model->vdp, frame_path) != 0) {
            input_error("cannot write %s", frame_path);
        }
    }
    printf("OK insns=%llu interrupts=%llu handoffs=%llu hw=%zu seconds=%.2f\n",
           (unsigned long long)ck->index, (unsigned long long)ck->interrupts,
           (unsigned long long)ck->handoffs, rr_hw_replay_position(ck->recording), seconds);
    return 0;
}
