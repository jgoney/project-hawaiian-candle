#include "rr_genesis.h"
#include "rr_sound.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rr_mem.h"
#include "rr_run.h"

#define IO_BASE 0xA10000u
#define VERSION_OVERSEAS_NTSC 0xA0
/* MAME's stall for a 68000-to-VDP DMA: one cycle for every 228 words after
 * the first word. The trace windows bound the rate between 228 and 228.36
 * words a cycle; this form fits all 347 DMAs in them. */
static uint32_t dma_stall_cycles(uint32_t words) { return words == 0 ? 0 : (words - 1) / 228; }

/* Time is kept the way MAME keeps it, in attoseconds, because the reference
 * recordings were made on MAME and the game's timing loops read the counters
 * to within one count. MAME gives the CPU a whole number of attoseconds per
 * cycle, rounded down, so its CPU runs a hair behind the true ratio of the two
 * clocks. With these three numbers the counter formulas below reproduce all
 * 187,920 counter reads in the six race trace windows. */
typedef unsigned __int128 Atto;
#define ATTO_PER_SECOND 1000000000000000000ull
#define ATTO_PER_CYCLE (ATTO_PER_SECOND / RR_CPU_HZ)        /* 130,370,396,637 */
#define ATTO_PER_LINE 63695246183523ull                     /* 3420 master clocks */
#define ATTO_PHASE 1042963095403ull                         /* RR_CLOCK_PHASE master clocks */
#define ATTO_PER_FRAME ((Atto)ATTO_PER_LINE * RR_LINES_PER_FRAME)
/* Where in a line MAME raises its interrupts (line and frame alike): 205
 * master clocks. Bracketed from about 1,600 interrupt entries in the trace
 * windows: an instruction boundary at 204.996 never saw the interrupt, one at
 * 205.078 did. */
#define ATTO_INTERRUPT 3817989902814ull

static Atto atto_time(const RrGenesis *g) {
    return g->speed_base_time +
           (Atto)(g->m->cpu.cycles - g->speed_base_cycles) * ATTO_PER_CYCLE * 100 / g->cpu_percent;
}

uint64_t rr_genesis_master_time(const RrGenesis *g) {
    return (uint64_t)(atto_time(g) / ATTO_PER_LINE) * RR_MASTER_PER_LINE +
           (uint64_t)(atto_time(g) % ATTO_PER_LINE * RR_MASTER_PER_LINE / ATTO_PER_LINE);
}

/* The first cycle count at which atto_time has reached `atto`. */
static uint64_t cycles_at(const RrGenesis *g, Atto atto) {
    if (atto <= g->speed_base_time) return g->speed_base_cycles;
    Atto per_cycle = (Atto)ATTO_PER_CYCLE * 100; /* in units of 1/percent attoseconds */
    return g->speed_base_cycles +
           (uint64_t)(((atto - g->speed_base_time) * g->cpu_percent + per_cycle - 1) / per_cycle);
}

/* Time since the line began. */
static uint64_t line_time(const RrGenesis *g) { return (uint64_t)(atto_time(g) % ATTO_PER_LINE); }

int rr_genesis_line(const RrGenesis *g) {
    return (int)(atto_time(g) / ATTO_PER_LINE % RR_LINES_PER_FRAME);
}

/* Position across the line as MAME's 315-5313 counts it: 0-479. */
#define MAME_H_POSITIONS 480u
#define MAME_V_STEPS_AFTER 460 /* the V counter shows the next line beyond this position */
#define MAME_H_JUMP_ABOVE 0xF7
#define MAME_H_JUMP 0x49

/* In the last CPU cycle of a line MAME reports the end position, and counts
 * the line as over (see v_counter). */
static int line_ending(const RrGenesis *g) { return line_time(g) >= ATTO_PER_LINE - ATTO_PER_CYCLE; }

static unsigned h_position(const RrGenesis *g) {
    if (line_ending(g)) return MAME_H_POSITIONS;
    return (unsigned)((Atto)line_time(g) * MAME_H_POSITIONS / ATTO_PER_LINE);
}

/* Horizontal counter, as MAME computes it (and as the owner's reference
 * recordings therefore saw it): the position, reading 0x49 lower above 0xF7.
 * Real hardware counts 0x00-0xB6 then 0xE4-0xFF at less than half this rate.
 * The game times its line-interrupt waits with this value, so which one is
 * modelled changes when its main code gets to run. */
static unsigned h_counter(const RrGenesis *g) {
    unsigned position = h_position(g);
    return (position > MAME_H_JUMP_ABOVE ? position - MAME_H_JUMP : position) & 0xFF;
}

/* NTSC vertical counter: the line, stepping near the end of it; 0x00-0xEA then 0xE5-0xFF. */
static unsigned v_counter(const RrGenesis *g) {
    unsigned line = (unsigned)rr_genesis_line(g) + (h_position(g) > MAME_V_STEPS_AFTER ? 1 : 0) +
                    (line_ending(g) ? 1 : 0);
    return (line <= 0xEA ? line : line - 6) & 0xFF;
}

static int in_vblank(const RrGenesis *g) {
    return rr_genesis_line(g) >= RR_VBLANK_LINE || !(g->vdp.reg[1] & 0x40);
}

/* Horizontal blanking: roughly the last sixth of MAME's line. Approximate. */
static int in_hblank(const RrGenesis *g) { return h_position(g) >= 400; }

/* Three-button pad: what the data lines read for the current TH level. */
static uint8_t pad_lines(const RrGenesis *g, int port) {
    unsigned held = g->pad_buttons[port];
    int th = !(g->pad_ctrl[port] & 0x40) || (g->pad_latch[port] & 0x40);
    uint8_t lines;
    if (th) {
        lines = 0x3F;
        if (held & RR_PAD_UP) lines &= (uint8_t)~0x01;
        if (held & RR_PAD_DOWN) lines &= (uint8_t)~0x02;
        if (held & RR_PAD_LEFT) lines &= (uint8_t)~0x04;
        if (held & RR_PAD_RIGHT) lines &= (uint8_t)~0x08;
        if (held & RR_PAD_B) lines &= (uint8_t)~0x10;
        if (held & RR_PAD_C) lines &= (uint8_t)~0x20;
        lines |= 0x40;
    } else {
        lines = 0x33;
        if (held & RR_PAD_UP) lines &= (uint8_t)~0x01;
        if (held & RR_PAD_DOWN) lines &= (uint8_t)~0x02;
        if (held & RR_PAD_A) lines &= (uint8_t)~0x10;
        if (held & RR_PAD_START) lines &= (uint8_t)~0x20;
    }
    uint8_t outputs = g->pad_ctrl[port];
    return (uint8_t)((lines & ~outputs) | (g->pad_latch[port] & outputs));
}

static uint8_t io_read(RrGenesis *g, uint32_t addr) {
    switch ((addr >> 1) & 0x0F) {
    case 0: return VERSION_OVERSEAS_NTSC;
    case 1: return pad_lines(g, 0);
    case 2: return pad_lines(g, 1);
    case 4: return g->pad_ctrl[0];
    case 5: return g->pad_ctrl[1];
    default: return 0;
    }
}

static void io_write(RrGenesis *g, uint32_t addr, uint8_t value) {
    switch ((addr >> 1) & 0x0F) {
    case 1: g->pad_latch[0] = value; break;
    case 2: g->pad_latch[1] = value; break;
    case 4: g->pad_ctrl[0] = value; break;
    case 5: g->pad_ctrl[1] = value; break;
    default: break;
    }
}

uint64_t rr_genesis_sound_time(const RrGenesis *g) {
    uint64_t master = rr_genesis_master_time(g);
    return master > RR_CLOCK_PHASE ? master - RR_CLOCK_PHASE : 0;
}

static uint8_t z80_area_read(RrGenesis *g, uint32_t addr) {
    uint32_t offset = addr & 0x7FFF;
    if (g->sound != NULL) rr_sound_run_to(g->sound, rr_genesis_sound_time(g));
    return offset < 0x4000 ? g->z80_ram[offset & 0x1FFF] : 0; /* FM chip and the rest read 0 */
}

static void z80_area_write(RrGenesis *g, uint32_t addr, uint8_t value) {
    uint32_t offset = addr & 0x7FFF;
    if (g->sound != NULL) {
        rr_sound_run_to(g->sound, rr_genesis_sound_time(g));
        if (offset >= 0x4000 && offset < 0x4004) rr_sound_fm_write(g->sound, rr_genesis_sound_time(g), offset & 3, value);
    }
    if (offset < 0x4000) {
        g->z80_ram[offset & 0x1FFF] = value;
        g->z80_ram_writes++;
    }
}

/* The Z80's bus is granted to the 68000 while it is requested and the Z80 is not in reset. */
static uint8_t bus_request_read(const RrGenesis *g) { return g->z80_busreq ? 0 : 1; }

static uint16_t vdp_read(RrGenesis *g, uint32_t addr) {
    switch (addr & 0x1C) {
    case 0x00: return rr_vdp_read_data(&g->vdp);
    case 0x04: return rr_vdp_read_status(&g->vdp, in_vblank(g), in_hblank(g));
    case 0x08:
    case 0x0C: return (uint16_t)(v_counter(g) << 8 | h_counter(g));
    default: return 0xFFFF; /* PSG is write-only */
    }
}

static uint16_t genesis_read(RrHw *hw, uint32_t addr, int size) {
    RrGenesis *g = (RrGenesis *)hw;
    if (addr >= 0xA00000 && addr < 0xA10000) {
        uint8_t byte = z80_area_read(g, addr);
        return size == 1 ? byte : (uint16_t)(byte << 8 | byte);
    }
    if (addr >= IO_BASE && addr < IO_BASE + 0x20) return io_read(g, addr);
    if ((addr & 0xFFFFFE) == 0xA11100) {
        uint8_t byte = bus_request_read(g);
        return size == 1 ? ((addr & 1) ? 0 : byte) : (uint16_t)(byte << 8);
    }
    if ((addr & 0xFFFFFE) == 0xA11200) return 0;
    if (addr >= 0xC00000 && addr < 0xC00020) {
        uint16_t word = vdp_read(g, addr);
        if (size == 2) return word;
        return (addr & 1) ? (uint8_t)word : (uint8_t)(word >> 8);
    }
    rr_fatal(g->m, "read from unmodelled hardware address %06X", addr);
}

/* Tell the tool what a write to the video chip's data port (0x00) or control
 * port (0x04) is about to do. Works from the chip's state before the write
 * and leaves it untouched. */
static void report_video_write(RrGenesis *g, uint32_t port, uint16_t value) {
    const RrVdp *v = &g->vdp;
    RrVideoEvent e = {.frame = g->frame_index, .line = rr_genesis_line(g)};
    static const uint8_t memory_of_code[16] = {[1] = RR_VIDEO_VRAM, [3] = RR_VIDEO_CRAM, [5] = RR_VIDEO_VSRAM};
    if (port == 0x00) {
        e.kind = memory_of_code[v->code & 0x0F];
        if (e.kind != 0) {
            e.target = e.kind == RR_VIDEO_VRAM ? v->addr : (uint32_t)(v->addr & 0x7E);
            e.value = value;
            g->video_log(g->video_log_ctx, &e);
        }
        if (v->fill_pending) {
            uint32_t length = (uint32_t)v->reg[20] << 8 | v->reg[19];
            e.kind = RR_VIDEO_FILL;
            e.target = (uint16_t)(v->addr + v->reg[15]);
            e.value = length ? length : 0x10000;
            e.source = value >> 8;
            g->video_log(g->video_log_ctx, &e);
        }
        return;
    }
    if (!v->pending) {
        if ((value & 0xC000) != 0x8000) return; /* first half of an address: nothing happens yet */
        e.kind = RR_VIDEO_REGISTER;
        e.target = (value >> 8) & 0x1F;
        e.value = value & 0xFF;
        g->video_log(g->video_log_ctx, &e);
        return;
    }
    /* Second half of an address: it may start a DMA. */
    unsigned code = (v->code & 0x03) | ((value >> 2) & 0x3C);
    if (!(code & 0x20) || !(v->reg[1] & 0x10)) return;
    uint32_t length = (uint32_t)v->reg[20] << 8 | v->reg[19];
    uint32_t source = (uint32_t)v->reg[22] << 8 | v->reg[21];
    e.target = (uint16_t)((v->addr & 0x3FFF) | (value & 3) << 14);
    e.value = length ? length : 0x10000;
    switch (v->reg[23] >> 6) {
    case 2: return; /* a fill: reported when its value arrives at the data port */
    case 3:
        e.kind = RR_VIDEO_COPY;
        e.source = source;
        break;
    default:
        e.kind = RR_VIDEO_DMA;
        e.memory = memory_of_code[code & 0x0F];
        e.source = (uint32_t)(v->reg[23] & 0x7F) << 17 | source << 1;
        if (e.memory != RR_VIDEO_VRAM) e.target &= 0x7E;
        break;
    }
    g->video_log(g->video_log_ctx, &e);
}

static void genesis_write(RrHw *hw, uint32_t addr, int size, uint16_t value) {
    RrGenesis *g = (RrGenesis *)hw;
    if (addr >= 0xA00000 && addr < 0xA10000) {
        z80_area_write(g, addr, size == 1 ? (uint8_t)value : (uint8_t)(value >> 8));
        /* MAME charges one extra cycle for a byte write to the Z80's memory. */
        if (size == 1) g->m->cpu.cycles += 1;
        return;
    }
    if (addr >= IO_BASE && addr < IO_BASE + 0x20) {
        io_write(g, addr, (uint8_t)value);
        return;
    }
    if ((addr & 0xFFFFFE) == 0xA11100) {
        if (size == 2) g->z80_busreq = (value & 0x0100) != 0;
        else if (!(addr & 1)) g->z80_busreq = (value & 1) != 0;
        if (g->sound != NULL) rr_sound_set_bus_held(g->sound, rr_genesis_sound_time(g), g->z80_busreq);
        return;
    }
    if ((addr & 0xFFFFFE) == 0xA11200) {
        if (size == 2) g->z80_reset = (value & 0x0100) == 0;
        else if (!(addr & 1)) g->z80_reset = (value & 1) == 0;
        if (g->sound != NULL) rr_sound_set_reset_held(g->sound, rr_genesis_sound_time(g), g->z80_reset);
        return;
    }
    if ((addr & 0xFFFFFC) == 0xA14000) return; /* TMSS */
    if (addr >= 0xC00000 && addr < 0xC00020) {
        uint32_t port = addr & 0x1C;
        if (port >= 0x10) { /* PSG and unused: ignored */
            if (port == 0x10) g->psg_writes++;
            return;
        }
        if (size != 2) rr_fatal(g->m, "byte write to video port %06X is not modelled", addr);
        if (g->video_log != NULL) report_video_write(g, port, value);
        if (port == 0x00) rr_vdp_write_data(&g->vdp, value);
        else if (port == 0x04) {
            rr_vdp_write_control(&g->vdp, g->m, value);
            /* MAME holds the 68000 while a DMA from its memory runs. */
            g->m->cpu.cycles += dma_stall_cycles(rr_vdp_take_dma_words(&g->vdp));
        }
        return; /* HV counter is read-only */
    }
    rr_fatal(g->m, "write to unmodelled hardware address %06X", addr);
}

static void genesis_close(RrHw *hw) { free(hw); }

RrGenesis *rr_genesis_new(RrMachine *m) {
    RrGenesis *g = calloc(1, sizeof *g);
    if (g == NULL) rr_fatal(m, "out of memory");
    g->m = m;
    g->hw.read = genesis_read;
    g->hw.write = genesis_write;
    g->hw.close = genesis_close;
    rr_vdp_reset(&g->vdp);
    /* The line counter is reloaded from register 10 during vertical blanking.
     * Until the first blanking it must not expire, or an interrupt would be
     * pending before the game has programmed anything. */
    g->hint_counter = 0xFF;
    g->cpu_percent = 100;
    g->speed_base_time = ATTO_PHASE;
    g->resume_needed = 1; /* the first frame starts wherever the clock says */
    return g;
}

void rr_genesis_free(RrGenesis *g) { free(g); }

/* Cycles an instruction spends fetching its source operand, by addressing mode. */
static unsigned operand_fetch_cycles(unsigned opcode) {
    static const unsigned by_mode[8] = {0, 0, 4, 4, 6, 8, 10, 0};
    static const unsigned by_register[8] = {8, 12, 8, 10, 4, 0, 0, 0}; /* mode 7 */
    unsigned mode = (opcode >> 3) & 7;
    return mode == 7 ? by_register[opcode & 7] : by_mode[mode];
}

/* Instructions whose last bus access is followed by a single two-cycle
 * internal step. MAME notes the interrupt during that step, so one raised up
 * to two cycles after the instruction ends is still taken after it. Measured
 * for cmp.l, cmpa.l, exg, btst and bset on a data register (the trace
 * windows); the rest are the instructions of the same shape. */
static int ends_with_one_internal_step(const RrMachine *m, unsigned opcode, uint32_t pc) {
    unsigned top = opcode >> 12, mode = (opcode >> 3) & 7;
    int memory_source = mode >= 2 && !(mode == 7 && (opcode & 7) == 4);
    if ((opcode & 0xF1C0) == 0xB080) return 1;                  /* cmp.l to a data register */
    if ((opcode & 0xF0C0) == 0xB0C0) return 1;                  /* cmpa */
    if ((opcode & 0xF130) == 0xC100 && (opcode & 0x00C0) != 0 && (opcode & 0x00C0) != 0x00C0) return 1; /* exg */
    if ((opcode & 0xFFF8) == 0x0800 || (opcode & 0xF1F8) == 0x0100) return 1; /* btst on a data register */
    /* bchg and bset on a data register, when the bit is in the low word (a
     * higher bit takes two more internal cycles). */
    if ((opcode & 0xFFF8) == 0x0840 || (opcode & 0xFFF8) == 0x08C0) {
        return pc + 3 < m->rom_size && (m->rom[pc + 3] & 31) < 16;
    }
    if ((opcode & 0xF1F8) == 0x0140 || (opcode & 0xF1F8) == 0x01C0) {
        return (m->cpu.d[(opcode >> 9) & 7] & 31) < 16;
    }
    if ((opcode & 0xF9F8) == 0x4080) return 1;                  /* negx.l, clr.l, neg.l, not.l on a data register */
    if ((top == 0x8 || top == 0x9 || top == 0xC || top == 0xD) && (opcode & 0x01C0) == 0x0080) {
        return memory_source;                                   /* or.l, sub.l, and.l, add.l from memory */
    }
    if ((top == 0x9 || top == 0xD) && (opcode & 0x01C0) == 0x01C0) return memory_source; /* suba.l, adda.l */
    return 0;
}

/* MAME's 68000 notes a waiting interrupt at one step inside each instruction
 * (where its microcode moves the next opcode into the decoder). For most
 * instructions that step begins their last four cycles, so anything raised
 * before the instruction ends is taken after it. Shifts of a register,
 * multiplies, divides and short branches that fall through carry on with
 * internal work after that step: an interrupt raised later waits one more
 * instruction. Some take the step in a closing internal step
 * (ends_with_one_internal_step), an indexed lea two cycles sooner than most.
 * Returns the cycle count by which an interrupt must have been raised to be
 * taken after the instruction that ran from `start` to now.
 *
 * Fitted to the race-start and race trace windows (analysis/race-conformance.md);
 * the classes are the ones seen there, not MAME's whole microcode table. */
static uint64_t interrupt_noted_by(const RrGenesis *g, uint64_t start, uint32_t start_pc) {
    const RrMachine *m = g->m;
    if (start_pc + 1 >= m->rom_size) return m->cpu.cycles;
    unsigned opcode = (unsigned)(m->rom[start_pc] << 8 | m->rom[start_pc + 1]);
    unsigned top = opcode >> 12;
    if (top == 0xE && (opcode & 0x00C0) != 0x00C0) return start + 8; /* register shift or rotate */
    if ((top == 0x8 || top == 0xC) && (opcode & 0x00C0) == 0x00C0) {
        /* mulu, muls: 8 cycles after the operand is fetched; divu, divs: 4. */
        return start + operand_fetch_cycles(opcode) + (top == 0xC ? 8 : 4);
    }
    if (ends_with_one_internal_step(m, opcode, start_pc)) return m->cpu.cycles + 2;
    if ((opcode & 0xF1C0) == 0x41C0 && ((opcode & 0x38) == 0x30 || (opcode & 0x3F) == 0x3B)) {
        return m->cpu.cycles - 2;                                /* lea with an index */
    }
    if (top == 0x6 && (opcode & 0x0F00) > 0x0100 && (opcode & 0xFF) != 0) { /* a short conditional branch */
        if ((m->cpu.pc & 0xFFFFFF) == start_pc + 2) return start + 4;       /* that fell through */
    }
    return m->cpu.cycles;
}

/* Take a pending interrupt if the CPU's mask allows and it was raised early
 * enough in the last instruction. Level 6 wins over level 4. */
static void deliver_interrupts(RrGenesis *g) {
    int mask = (g->m->cpu.sr >> 8) & 7;
    if (g->vint_pending && (g->vdp.reg[1] & 0x20) && mask < 6 &&
        g->vint_raised_at <= g->interrupt_noted_by) {
        g->vint_pending = 0;
        rr_interrupt(g->m, 6);
        rr_vdp_ack_frame_interrupt(&g->vdp);
        g->vblank_entries++;
        if (g->vblank_hook != NULL) g->vblank_hook(g, g->vblank_hook_ctx);
    } else if (g->hint_pending && (g->vdp.reg[0] & 0x10) && mask < 4 &&
               g->hint_raised_at <= g->interrupt_noted_by) {
        g->hint_pending = 0;
        rr_interrupt(g->m, 4);
    }
}

/* Run until atto_time has reached `atto`. Returns 0 if the clock was replaced
 * on the way (rr_genesis_time_changed). */
static int run_until(RrGenesis *g, Atto atto) {
    uint64_t cycles = cycles_at(g, atto);
    /* An instruction that notes interrupts after its own end (see
     * interrupt_noted_by) reaches a little way past the clock. */
    while (g->m->cpu.cycles < cycles && g->interrupt_noted_by < cycles) {
        deliver_interrupts(g);
        if (g->step_hook != NULL) {
            g->step_hook(g, g->step_hook_ctx);
            if (g->resume_needed) return 0;
        }
        uint64_t start = g->m->cpu.cycles;
        uint32_t start_pc = g->m->cpu.pc & 0xFFFFFF;
        rr_step(g->m);
        g->interrupt_noted_by = interrupt_noted_by(g, start, start_pc);
    }
    return 1;
}

void rr_genesis_time_changed(RrGenesis *g) { g->resume_needed = 1; }

void rr_genesis_power_on(RrGenesis *g) {
    RrMachine *m = g->m;
    memset(&m->cpu, 0, sizeof m->cpu);
    m->cpu.sr = 0x2700;
    m->cpu.a[7] = rr_read32(m, 0);
    m->cpu.pc = rr_read32(m, 4);
    m->cpu.cycles = RR_RESET_CYCLES;
    g->speed_base_cycles = 0;
    g->speed_base_time = ATTO_PHASE;
    if (g->sound != NULL) {
        rr_sound_power_on(g->sound);
        rr_sound_set_bus_held(g->sound, 0, g->z80_busreq);
    }
    rr_genesis_time_changed(g);
}

void rr_genesis_set_cpu_speed(RrGenesis *g, unsigned percent) {
    if (percent < 100) percent = 100;
    if (percent > 6400) percent = 6400;
    g->speed_base_time = atto_time(g);
    g->speed_base_cycles = g->m->cpu.cycles;
    g->cpu_percent = percent;
}

unsigned rr_genesis_cpu_speed(const RrGenesis *g) { return g->cpu_percent; }

/* A frame is a schedule of events, two per line: the line is drawn at its
 * start, and its interrupts are raised a little way in. */
#define EVENTS_PER_FRAME (RR_LINES_PER_FRAME * 2)

static Atto event_time(int event) {
    return (Atto)(event / 2) * ATTO_PER_LINE + (event & 1 ? ATTO_INTERRUPT : 0);
}

/* Find the place in the schedule that matches the clock: at power-on, and
 * after the clock has been replaced. Events before it are taken as done. */
static void find_schedule(RrGenesis *g) {
    Atto now = atto_time(g);
    Atto within = now % ATTO_PER_FRAME;
    uint64_t position = (uint64_t)(within % ATTO_PER_LINE);
    g->frame_index = (uint64_t)(now / ATTO_PER_FRAME);
    g->next_event = (int)(within / ATTO_PER_LINE) * 2 +
                    (position == 0 ? 0 : position <= ATTO_INTERRUPT ? 1 : 2);
    g->interrupt_noted_by = 0; /* belongs to the old clock */
    g->resume_needed = 0;
}

uint64_t rr_genesis_run_frame(RrGenesis *g) {
    RrMachine *m = g->m;
    uint64_t start_insns = m->cpu.insns;
    if (g->resume_needed) find_schedule(g);
    /* Frames are fixed stretches of absolute time, so they follow each other
     * exactly however far the last instruction of one runs into the next. */
    Atto frame_start = (Atto)g->frame_index * ATTO_PER_FRAME;
    while (g->next_event < EVENTS_PER_FRAME) {
        int line = g->next_event / 2;
        if (!run_until(g, frame_start + event_time(g->next_event))) return m->cpu.insns - start_insns;
        if (!(g->next_event & 1)) {
            /* A display line is drawn from the state at its start, so what a
             * line interrupt handler changes shows from the next line on. */
            if (line < RR_SCREEN_H) {
                if (line == 0 && g->hires_begin != NULL) g->hires_begin(g, g->hires_ctx);
                if (g->hires_line != NULL) g->hires_line(g, line, g->hires_ctx);
                if (g->hires != NULL) {
                    int scale = g->hires_scale;
                    int shifted = g->hires_have != NULL && g->hires_have[line];
                    if (shifted) {
                        /* The shifts were chosen before line 0. Use them only if they
                         * still belong to this line as it is drawn now: their whole
                         * part must be the line's own scroll value. */
                        RrVdpLineState now;
                        rr_vdp_line_state(&g->vdp, line, &now);
                        const int32_t *a = g->hires_expect_a != NULL ? g->hires_expect_a : g->hires_shift_a;
                        const int32_t *b = g->hires_expect_b != NULL ? g->hires_expect_b : g->hires_shift_b;
                        shifted = ((a[line * scale] >> 16) & 0x3FF) == now.hscroll[0] &&
                                  ((b[line * scale] >> 16) & 0x3FF) == now.hscroll[1];
                    }
                    const RrFineSprites *fine = g->hires_sprites, *margin_fine = g->hires_margin_sprites;
                    if ((fine != NULL || margin_fine != NULL) && g->vdp.vram_writes != g->hires_sprites_stamp) {
                        fine = margin_fine = NULL;
                        g->hires_sprites_stale_lines++;
                    }
                    const int32_t *shift_a = shifted ? &g->hires_shift_a[line * scale] : NULL;
                    const int32_t *shift_b = shifted ? &g->hires_shift_b[line * scale] : NULL;
                    const RrWide *wide = g->hires_wide;
                    int margin = wide == NULL || wide->margin < 0 ? 0
                                 : wide->margin > RR_WIDE_MARGIN_MAX ? RR_WIDE_MARGIN_MAX : wide->margin;
                    size_t at = (size_t)line * (size_t)scale * (size_t)(RR_SCREEN_W + 2 * margin) * (size_t)scale;
                    rr_vdp_render_line_wide(
                        &g->vdp, line, scale, shift_a, shift_b,
                        shifted && g->hires_beyond_a != NULL ? &g->hires_beyond_a[line * scale] : NULL,
                        shifted && g->hires_beyond_b != NULL ? &g->hires_beyond_b[line * scale] : NULL, fine,
                        margin_fine, wide, &g->hires[at], g->hires_sources != NULL ? &g->hires_sources[at] : NULL);
                    if (g->hires_narrow != NULL) {
                        rr_vdp_render_line_fine(&g->vdp, line, scale, shift_a, shift_b, fine,
                                                &g->hires_narrow[(size_t)line * scale * RR_SCREEN_W * scale]);
                    }
                }
                rr_vdp_render_line(&g->vdp, line, &g->frame[line * RR_SCREEN_W]);
                if (g->sources != NULL) rr_vdp_render_line_sources(&g->vdp, line, &g->sources[line * RR_SCREEN_W]);
                if (g->line_state != NULL) rr_vdp_line_state(&g->vdp, line, &g->line_state[line]);
                if (g->hires_line_drawn != NULL) g->hires_line_drawn(g, line, g->hires_ctx);
            }
        } else {
            /* The line counter runs on display lines and reloads during blanking. */
            if (line <= RR_VBLANK_LINE) {
                if (--g->hint_counter < 0) {
                    g->hint_counter = g->vdp.reg[10];
                    /* One already waiting keeps the time it was first raised. */
                    if (!g->hint_pending) {
                        g->hint_raised_at = cycles_at(g, frame_start + event_time(g->next_event));
                    }
                    g->hint_pending = 1;
                }
            } else {
                g->hint_counter = g->vdp.reg[10];
            }
            if (line == RR_VBLANK_LINE) {
                if (!g->vint_pending) {
                    g->vint_raised_at = cycles_at(g, frame_start + event_time(g->next_event));
                }
                g->vint_pending = 1;
                rr_vdp_frame_interrupt(&g->vdp);
            }
        }
        g->next_event++;
    }
    if (!run_until(g, frame_start + ATTO_PER_FRAME)) return m->cpu.insns - start_insns;
    if (g->sound != NULL) rr_sound_run_to(g->sound, rr_genesis_sound_time(g));
    g->frame_index++;
    g->next_event = 0;
    return m->cpu.insns - start_insns;
}

int rr_genesis_write_ppm(const RrGenesis *g, const char *path) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) return 1;
    fprintf(f, "P6\n%d %d\n255\n", RR_SCREEN_W, RR_SCREEN_H);
    for (int i = 0; i < RR_SCREEN_W * RR_SCREEN_H; i++) {
        fputc((int)(g->frame[i] >> 16) & 0xFF, f);
        fputc((int)(g->frame[i] >> 8) & 0xFF, f);
        fputc((int)g->frame[i] & 0xFF, f);
    }
    return fclose(f) != 0;
}
