#include "rr_run.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "rr_mem.h"

#define SR_SUPERVISOR 0x2000
#define SR_TRACE 0x8000
#define SR_INT_MASK 0x0700
/* An autovectored interrupt waits for the E clock, which runs at a tenth of the
 * CPU clock, so entry takes 49 to 58 cycles and always ends on the same phase
 * (cycle count 5 modulo 10). Fitted to MAME 0.289, where it holds for every
 * one of 884 interrupts in the race trace window. */
#define INTERRUPT_MIN_CYCLES 49
#define E_CLOCK_PERIOD 10
#define E_CLOCK_PHASE 5

static const char SAVE_MAGIC[8] = {'R', 'R', '2', 'S', 'A', 'V', 'E', '1'};

static RrFatalHandler fatal_handler;
static void *fatal_handler_ctx;

void rr_set_fatal_handler(RrFatalHandler handler, void *ctx) {
    fatal_handler = handler;
    fatal_handler_ctx = ctx;
}

void rr_fatal(const RrMachine *m, const char *fmt, ...) {
    char message[512];
    int used;
    va_list args;
    if (m != NULL) {
        used = snprintf(message, sizeof message, "RR2FATAL pc=%06X insn=%llu: ",
                        m->cpu.pc & 0xFFFFFF, (unsigned long long)m->cpu.insns);
    } else {
        used = snprintf(message, sizeof message, "RR2FATAL: ");
    }
    va_start(args, fmt);
    vsnprintf(message + used, sizeof message - (size_t)used, fmt, args);
    va_end(args);
    if (fatal_handler != NULL) {
        fatal_handler(message, fatal_handler_ctx);
    } else {
        fprintf(stderr, "%s\n", message);
    }
    exit(2);
}

void rr_step(RrMachine *m) {
    RrInsnFn fn = rr_lookup(m->cpu.pc & 0xFFFFFF);
    if (fn == NULL) {
        rr_fatal(m, "jump to untranslated address %06X from instruction at %06X",
                 m->cpu.pc & 0xFFFFFF, m->cpu.last_pc & 0xFFFFFF);
    }
    m->cpu.last_pc = m->cpu.pc;
    fn(m);
    m->cpu.insns++;
}

void rr_interrupt(RrMachine *m, int level) {
    uint16_t old_sr = m->cpu.sr;
    if (level < 1 || level > 7) rr_fatal(m, "invalid interrupt level %d", level);
    if (!(old_sr & SR_SUPERVISOR)) rr_fatal(m, "interrupt in user mode is not supported");
    m->cpu.sr = (uint16_t)((old_sr & ~(SR_TRACE | SR_INT_MASK)) | (level << 8));
    m->cpu.a[7] -= 4;
    rr_write32(m, m->cpu.a[7], m->cpu.pc);
    m->cpu.a[7] -= 2;
    rr_write16(m, m->cpu.a[7], old_sr);
    m->cpu.pc = rr_read32(m, (uint32_t)(24 + level) * 4);
    uint64_t done = m->cpu.cycles + INTERRUPT_MIN_CYCLES;
    done += (E_CLOCK_PHASE + E_CLOCK_PERIOD - done % E_CLOCK_PERIOD) % E_CLOCK_PERIOD;
    m->cpu.cycles = done;
}

uint32_t rr_crc32(const uint8_t *data, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static void put_u32(uint8_t **p, uint32_t v) {
    for (int i = 0; i < 4; i++) *(*p)++ = (uint8_t)(v >> (8 * i));
}
static void put_u64(uint8_t **p, uint64_t v) {
    for (int i = 0; i < 8; i++) *(*p)++ = (uint8_t)(v >> (8 * i));
}
static uint32_t get_u32(const uint8_t **p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)*(*p)++ << (8 * i);
    return v;
}
static uint64_t get_u64(const uint8_t **p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)*(*p)++ << (8 * i);
    return v;
}

/* magic, rom crc, d[8], a[8], pc, usp, last_pc, sr, cycles, insns */
#define SAVE_HEADER_SIZE (8 + 4 + 16 * 4 + 4 + 4 + 4 + 4 + 8 + 8)

int rr_save(const RrMachine *m, FILE *f) {
    uint8_t header[SAVE_HEADER_SIZE];
    uint8_t *p = header;
    memcpy(p, SAVE_MAGIC, sizeof SAVE_MAGIC);
    p += sizeof SAVE_MAGIC;
    put_u32(&p, m->rom_crc32);
    for (int i = 0; i < 8; i++) put_u32(&p, m->cpu.d[i]);
    for (int i = 0; i < 8; i++) put_u32(&p, m->cpu.a[i]);
    put_u32(&p, m->cpu.pc);
    put_u32(&p, m->cpu.usp);
    put_u32(&p, m->cpu.last_pc);
    put_u32(&p, m->cpu.sr);
    put_u64(&p, m->cpu.cycles);
    put_u64(&p, m->cpu.insns);
    if (fwrite(header, 1, sizeof header, f) != sizeof header) return 1;
    if (fwrite(m->ram, 1, sizeof m->ram, f) != sizeof m->ram) return 1;
    return 0;
}

int rr_load(RrMachine *m, FILE *f) {
    uint8_t header[SAVE_HEADER_SIZE];
    const uint8_t *p = header;
    if (fread(header, 1, sizeof header, f) != sizeof header ||
        memcmp(header, SAVE_MAGIC, sizeof SAVE_MAGIC) != 0) {
        fprintf(stderr, "rr_load: not an RR2SAVE1 file\n");
        return 1;
    }
    p += sizeof SAVE_MAGIC;
    uint32_t crc = get_u32(&p);
    if (crc != m->rom_crc32) {
        fprintf(stderr, "rr_load: save is for ROM CRC-32 %08X, this machine has %08X\n", crc,
                m->rom_crc32);
        return 1;
    }
    RrCpu cpu;
    memset(&cpu, 0, sizeof cpu);
    for (int i = 0; i < 8; i++) cpu.d[i] = get_u32(&p);
    for (int i = 0; i < 8; i++) cpu.a[i] = get_u32(&p);
    cpu.pc = get_u32(&p);
    cpu.usp = get_u32(&p);
    cpu.last_pc = get_u32(&p);
    cpu.sr = (uint16_t)get_u32(&p);
    cpu.cycles = get_u64(&p);
    cpu.insns = get_u64(&p);
    uint8_t *ram = malloc(sizeof m->ram);
    if (ram == NULL) return 1;
    if (fread(ram, 1, sizeof m->ram, f) != sizeof m->ram) {
        fprintf(stderr, "rr_load: truncated save file\n");
        free(ram);
        return 1;
    }
    m->cpu = cpu;
    memcpy(m->ram, ram, sizeof m->ram);
    free(ram);
    return 0;
}
