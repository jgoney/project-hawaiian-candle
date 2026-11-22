#include "rr_sound.h"

#include <stdlib.h>
#include <string.h>

#include "ym3438.h"
#include "z80.h"

#define BUFFER_FRAMES ((size_t)RR_SOUND_BUFFER_SECONDS * RR_SOUND_HZ)
#define OUTPUT_GAIN 8 /* 24 chip clocks summed, times this, is one 16-bit sample */

struct RrSound {
    z80 cpu;
    ym3438_t fm;
    const uint8_t *rom;
    uint32_t rom_size;
    uint8_t *ram;
    unsigned bank; /* 9 bits: which 32 KB of the ROM the Z80 sees at 0x8000 */
    int bus_held, reset_held;
    uint64_t z80_time;  /* master clocks the Z80 has reached (may be an instruction past `now`) */
    uint64_t now;       /* the latest time the owner has given */
    uint64_t fm_clocks; /* chip clocks run so far */
    int clock_in_sample;
    long sum_left, sum_right;
    uint64_t steps, fm_writes, fm_writes_68000, psg_writes;
    RrSoundFmLog log;
    void *log_ctx;
    int16_t *buffer; /* a ring of BUFFER_FRAMES stereo frames */
    size_t first, count;
};

static int16_t clip(long value) {
    return (int16_t)(value > 32767 ? 32767 : value < -32768 ? -32768 : value);
}

/* Run the FM chip up to `master`. */
static void fm_run_to(RrSound *s, uint64_t master) {
    uint64_t target = master / RR_SOUND_MASTER_PER_FM_CLOCK;
    while (s->fm_clocks < target) {
        Bit16s out[2];
        OPN2_Clock(&s->fm, out);
        s->fm_clocks++;
        s->sum_left += out[0];
        s->sum_right += out[1];
        if (++s->clock_in_sample < 24) continue;
        size_t at = (s->first + s->count) % BUFFER_FRAMES;
        if (s->count == BUFFER_FRAMES) s->first = (s->first + 1) % BUFFER_FRAMES; /* the oldest goes */
        else s->count++;
        s->buffer[2 * at] = clip(s->sum_left * OUTPUT_GAIN);
        s->buffer[2 * at + 1] = clip(s->sum_right * OUTPUT_GAIN);
        s->clock_in_sample = 0;
        s->sum_left = s->sum_right = 0;
    }
}

static void fm_write(RrSound *s, uint64_t master, unsigned port, uint8_t value, int from_68000) {
    fm_run_to(s, master);
    OPN2_Write(&s->fm, port & 3, value);
    s->fm_writes++;
    if (from_68000) s->fm_writes_68000++;
    if (s->log != NULL) s->log(s->log_ctx, master, port & 3, value, from_68000);
}

static uint8_t z80_read(void *ctx, uint16_t addr) {
    RrSound *s = ctx;
    if (addr < 0x4000) return s->ram[addr & 0x1FFF];
    if (addr < 0x6000) {
        fm_run_to(s, s->z80_time);
        return OPN2_Read(&s->fm, addr & 3);
    }
    if (addr >= 0x8000) {
        uint32_t at = ((uint32_t)s->bank << 15) | (addr & 0x7FFF);
        return at < s->rom_size ? s->rom[at] : 0xFF;
    }
    return 0xFF;
}

static void z80_write(void *ctx, uint16_t addr, uint8_t value) {
    RrSound *s = ctx;
    if (addr < 0x4000) s->ram[addr & 0x1FFF] = value;
    else if (addr < 0x6000) fm_write(s, s->z80_time, addr & 3, value, 0);
    else if (addr < 0x6100) s->bank = ((s->bank >> 1) | ((value & 1u) << 8)) & 0x1FF;
    else if (addr >= 0x7F10 && addr <= 0x7F17) s->psg_writes++;
}

static uint8_t z80_port_in(z80 *cpu, uint8_t port) {
    (void)cpu;
    (void)port;
    return 0xFF;
}

static void z80_port_out(z80 *cpu, uint8_t port, uint8_t value) {
    (void)cpu;
    (void)port;
    (void)value;
}

static void z80_start(RrSound *s) {
    z80_init(&s->cpu);
    s->cpu.userdata = s;
    s->cpu.read_byte = z80_read;
    s->cpu.write_byte = z80_write;
    s->cpu.port_in = z80_port_in;
    s->cpu.port_out = z80_port_out;
}

RrSound *rr_sound_new(const uint8_t *rom, uint32_t rom_size, uint8_t *z80_ram) {
    RrSound *s = calloc(1, sizeof *s);
    if (s == NULL) return NULL;
    s->buffer = calloc(BUFFER_FRAMES * 2, sizeof *s->buffer);
    if (s->buffer == NULL) {
        free(s);
        return NULL;
    }
    s->rom = rom;
    s->rom_size = rom_size;
    s->ram = z80_ram;
    s->reset_held = 1;
    z80_start(s);
    OPN2_SetChipType(ym3438_mode_ym2612);
    OPN2_Reset(&s->fm);
    return s;
}

void rr_sound_power_on(RrSound *s) {
    z80_start(s);
    OPN2_Reset(&s->fm);
    s->bank = 0;
    s->bus_held = 0;
    s->reset_held = 1;
    s->z80_time = s->now = s->fm_clocks = 0;
    s->clock_in_sample = 0;
    s->sum_left = s->sum_right = 0;
    s->steps = s->fm_writes = s->fm_writes_68000 = s->psg_writes = 0;
    s->first = s->count = 0;
}

void rr_sound_free(RrSound *s) {
    if (s == NULL) return;
    free(s->buffer);
    free(s);
}

/* Is the Z80's interrupt line up at `master`, and when does that next change? */
static int interrupt_line(uint64_t master, uint64_t *changes_at) {
    uint64_t frame = master / RR_SOUND_MASTER_PER_FRAME * RR_SOUND_MASTER_PER_FRAME;
    uint64_t in_frame = master - frame;
    if (in_frame < RR_SOUND_INTERRUPT_AT) {
        *changes_at = frame + RR_SOUND_INTERRUPT_AT;
        return 0;
    }
    if (in_frame < RR_SOUND_INTERRUPT_AT + RR_SOUND_INTERRUPT_FOR) {
        *changes_at = frame + RR_SOUND_INTERRUPT_AT + RR_SOUND_INTERRUPT_FOR;
        return 1;
    }
    *changes_at = frame + RR_SOUND_MASTER_PER_FRAME + RR_SOUND_INTERRUPT_AT;
    return 0;
}

void rr_sound_set_time(RrSound *s, uint64_t master) {
    s->z80_time = s->now = master;
    s->fm_clocks = master / RR_SOUND_MASTER_PER_FM_CLOCK;
}

void rr_sound_run_to(RrSound *s, uint64_t master) {
    if (master <= s->now) return;
    s->now = master;
    if (s->bus_held || s->reset_held) {
        /* The Z80 stands still; an interrupt it cannot take is not kept. */
        if (s->z80_time < master) s->z80_time = master;
        s->cpu.int_pending = 0;
    }
    while (s->z80_time < master) {
        uint64_t changes_at;
        /* The line is a level: it is offered for as long as it is up. */
        s->cpu.int_pending = interrupt_line(s->z80_time, &changes_at) != 0;
        unsigned long before = s->cpu.cyc;
        z80_step(&s->cpu);
        s->steps++;
        unsigned long took = s->cpu.cyc - before;
        s->z80_time += (uint64_t)(took > 0 ? took : 4) * RR_SOUND_MASTER_PER_Z80;
    }
    fm_run_to(s, master);
}

void rr_sound_set_bus_held(RrSound *s, uint64_t master, int held) {
    rr_sound_run_to(s, master);
    s->bus_held = held != 0;
}

void rr_sound_set_reset_held(RrSound *s, uint64_t master, int held) {
    rr_sound_run_to(s, master);
    held = held != 0;
    if (held && !s->reset_held) OPN2_Reset(&s->fm);
    if (!held && s->reset_held) {
        z80_start(s);
        if (s->z80_time < s->now) s->z80_time = s->now;
    }
    s->reset_held = held;
}

void rr_sound_fm_write(RrSound *s, uint64_t master, unsigned port, uint8_t value) {
    rr_sound_run_to(s, master);
    fm_write(s, master > s->now ? master : s->now, port, value, 1);
}

void rr_sound_set_fm_log(RrSound *s, RrSoundFmLog log, void *ctx) {
    s->log = log;
    s->log_ctx = ctx;
}

size_t rr_sound_available(const RrSound *s) { return s->count; }

size_t rr_sound_take(RrSound *s, int16_t *out, size_t frames) {
    if (frames > s->count) frames = s->count;
    for (size_t i = 0; i < frames; i++) {
        size_t at = (s->first + i) % BUFFER_FRAMES;
        out[2 * i] = s->buffer[2 * at];
        out[2 * i + 1] = s->buffer[2 * at + 1];
    }
    s->first = (s->first + frames) % BUFFER_FRAMES;
    s->count -= frames;
    return frames;
}

uint64_t rr_sound_z80_steps(const RrSound *s) { return s->steps; }
uint64_t rr_sound_fm_writes(const RrSound *s) { return s->fm_writes; }
uint64_t rr_sound_fm_writes_68000(const RrSound *s) { return s->fm_writes_68000; }
uint64_t rr_sound_psg_writes(const RrSound *s) { return s->psg_writes; }
uint64_t rr_sound_now(const RrSound *s) { return s->now; }
