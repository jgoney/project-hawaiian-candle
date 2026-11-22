/* 68000 arithmetic, flag, shift and timing helpers used by translated code. */
#ifndef RR_OPS_H
#define RR_OPS_H

#include "rr_machine.h"
#include "rr_mem.h"
#include "rr_run.h"

enum { RR_C = 0x01, RR_V = 0x02, RR_Z = 0x04, RR_N = 0x08, RR_X = 0x10 };
enum { RR_ASL, RR_ASR, RR_LSL, RR_LSR, RR_ROXL, RR_ROXR, RR_ROL, RR_ROR };

#define RR_SR_VALID 0xA71F
#define RR_SR_SUPERVISOR 0x2000

static inline uint32_t rr_mask(int size) {
    return size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
}
static inline uint32_t rr_msb(int size) {
    return size == 1 ? 0x80u : size == 2 ? 0x8000u : 0x80000000u;
}

/* Replace the low `size` bytes of data register n. */
static inline void rr_setd(RrCpu *c, int n, int size, uint32_t v) {
    uint32_t mask = rr_mask(size);
    c->d[n] = (c->d[n] & ~mask) | (v & mask);
}

/* N and Z from the result, V and C cleared, X kept (MOVE, logic, TST...). */
static inline void rr_nz(RrCpu *c, uint32_t r, int size) {
    uint16_t f = c->sr & (uint16_t)~(RR_N | RR_Z | RR_V | RR_C);
    r &= rr_mask(size);
    if (r == 0) f |= RR_Z;
    if (r & rr_msb(size)) f |= RR_N;
    c->sr = f;
}

static inline uint16_t rr_nz_bits(uint32_t r, int size) {
    return (uint16_t)((r == 0 ? RR_Z : 0) | (r & rr_msb(size) ? RR_N : 0));
}

/* d + s, setting X N Z V C. */
static inline uint32_t rr_add(RrCpu *c, uint32_t s, uint32_t d, int size) {
    uint32_t mask = rr_mask(size), msb = rr_msb(size);
    s &= mask;
    d &= mask;
    uint32_t r = (s + d) & mask;
    uint16_t f = (c->sr & (uint16_t)~0x1F) | rr_nz_bits(r, size);
    if (((s & d) | (~r & (s | d))) & msb) f |= RR_C | RR_X;
    if ((s ^ r) & (d ^ r) & msb) f |= RR_V;
    c->sr = f;
    return r;
}

static inline uint16_t rr_sub_flags(uint32_t s, uint32_t d, uint32_t r, int size) {
    uint32_t msb = rr_msb(size);
    uint16_t f = 0;
    if (((s & ~d) | (r & ~d) | (s & r)) & msb) f |= RR_C;
    if ((s ^ d) & (r ^ d) & msb) f |= RR_V;
    return f;
}

/* d - s, setting X N Z V C. */
static inline uint32_t rr_sub(RrCpu *c, uint32_t s, uint32_t d, int size) {
    uint32_t mask = rr_mask(size);
    s &= mask;
    d &= mask;
    uint32_t r = (d - s) & mask;
    uint16_t f = rr_sub_flags(s, d, r, size) | rr_nz_bits(r, size);
    if (f & RR_C) f |= RR_X;
    c->sr = (c->sr & (uint16_t)~0x1F) | f;
    return r;
}

/* d - s, setting N Z V C only. */
static inline void rr_cmp(RrCpu *c, uint32_t s, uint32_t d, int size) {
    uint32_t mask = rr_mask(size);
    s &= mask;
    d &= mask;
    uint32_t r = (d - s) & mask;
    c->sr = (c->sr & (uint16_t)~0x0F) | rr_sub_flags(s, d, r, size) | rr_nz_bits(r, size);
}

/* d + s + X. Z is only ever cleared. */
static inline uint32_t rr_addx(RrCpu *c, uint32_t s, uint32_t d, int size) {
    uint32_t mask = rr_mask(size), msb = rr_msb(size);
    s &= mask;
    d &= mask;
    uint32_t r = (s + d + ((c->sr & RR_X) ? 1 : 0)) & mask;
    uint16_t f = c->sr & (uint16_t)~(RR_X | RR_N | RR_V | RR_C);
    if (r != 0) f &= (uint16_t)~RR_Z;
    if (r & msb) f |= RR_N;
    if (((s & d) | (~r & (s | d))) & msb) f |= RR_C | RR_X;
    if ((s ^ r) & (d ^ r) & msb) f |= RR_V;
    c->sr = f;
    return r;
}

/* d - s - X. Z is only ever cleared. */
static inline uint32_t rr_subx(RrCpu *c, uint32_t s, uint32_t d, int size) {
    uint32_t mask = rr_mask(size);
    s &= mask;
    d &= mask;
    uint32_t r = (d - s - ((c->sr & RR_X) ? 1 : 0)) & mask;
    uint16_t f = c->sr & (uint16_t)~(RR_X | RR_N | RR_V | RR_C);
    uint16_t vc = rr_sub_flags(s, d, r, size);
    if (r != 0) f &= (uint16_t)~RR_Z;
    if (r & rr_msb(size)) f |= RR_N;
    f |= vc;
    if (vc & RR_C) f |= RR_X;
    c->sr = f;
    return r;
}

/* Packed decimal add: d + s + X. N and V are officially undefined; this
 * follows the commonly documented 68000 behaviour. Z is only ever cleared. */
static inline uint32_t rr_abcd(RrCpu *c, uint32_t s, uint32_t d) {
    uint32_t x = (c->sr & RR_X) ? 1 : 0;
    uint32_t r = (s & 0x0F) + (d & 0x0F) + x;
    uint32_t before = r;
    uint16_t f = c->sr & (uint16_t)~(RR_X | RR_N | RR_V | RR_C);
    if (r > 9) r += 6;
    r += (s & 0xF0) + (d & 0xF0);
    if (r > 0x99) {
        r -= 0xA0;
        f |= RR_C | RR_X;
    }
    if (~before & r & 0x80) f |= RR_V;
    r &= 0xFF;
    if (r & 0x80) f |= RR_N;
    if (r != 0) f &= (uint16_t)~RR_Z;
    c->sr = f;
    return r;
}

static inline int rr_cond(const RrCpu *c, int cc) {
    int n = (c->sr & RR_N) != 0, z = (c->sr & RR_Z) != 0;
    int v = (c->sr & RR_V) != 0, cy = (c->sr & RR_C) != 0;
    switch (cc) {
    case 0: return 1;
    case 1: return 0;
    case 2: return !cy && !z;
    case 3: return cy || z;
    case 4: return !cy;
    case 5: return cy;
    case 6: return !z;
    case 7: return z;
    case 8: return !v;
    case 9: return v;
    case 10: return !n;
    case 11: return n;
    case 12: return n == v;
    case 13: return n != v;
    case 14: return !z && n == v;
    default: return z || n != v;
    }
}

/* Shift or rotate `v` by `count` (0-63) bit positions, one bit at a time. */
static inline uint32_t rr_shift(RrCpu *c, int kind, uint32_t v, unsigned count, int size) {
    uint32_t mask = rr_mask(size), msb = rr_msb(size);
    uint16_t f = c->sr & (uint16_t)~(RR_N | RR_Z | RR_V | RR_C);
    unsigned x = (c->sr & RR_X) ? 1 : 0;
    v &= mask;
    if (count == 0 && (kind == RR_ROXL || kind == RR_ROXR) && x) f |= RR_C;
    for (unsigned i = 0; i < count; i++) {
        unsigned out;
        switch (kind) {
        case RR_ASL:
            out = (v & msb) != 0;
            v = (v << 1) & mask;
            if (((v & msb) != 0) != out) f |= RR_V;
            x = out;
            break;
        case RR_LSL:
            out = (v & msb) != 0;
            v = (v << 1) & mask;
            x = out;
            break;
        case RR_ASR:
            out = v & 1;
            v = (v >> 1) | (v & msb);
            x = out;
            break;
        case RR_LSR:
            out = v & 1;
            v >>= 1;
            x = out;
            break;
        case RR_ROL:
            out = (v & msb) != 0;
            v = ((v << 1) | out) & mask;
            break;
        case RR_ROR:
            out = v & 1;
            v = (v >> 1) | (out ? msb : 0);
            break;
        case RR_ROXL:
            out = (v & msb) != 0;
            v = ((v << 1) | x) & mask;
            x = out;
            break;
        default: /* RR_ROXR */
            out = v & 1;
            v = (v >> 1) | (x ? msb : 0);
            x = out;
            break;
        }
        f = (uint16_t)((f & ~RR_C) | (out ? RR_C : 0));
    }
    if (kind != RR_ROL && kind != RR_ROR) f = (uint16_t)((f & ~RR_X) | (x ? RR_X : 0));
    c->sr = f | rr_nz_bits(v, size);
    return v;
}

static inline void rr_set_ccr(RrCpu *c, uint32_t v) {
    c->sr = (uint16_t)((c->sr & 0xFF00) | (v & 0x1F));
}

static inline void rr_set_sr(RrMachine *m, uint32_t v) {
    if (!(v & RR_SR_SUPERVISOR)) rr_fatal(m, "switch to user mode is not supported");
    m->cpu.sr = (uint16_t)(v & RR_SR_VALID);
}

static inline void rr_push32(RrMachine *m, uint32_t v) {
    m->cpu.a[7] -= 4;
    rr_write32(m, m->cpu.a[7], v);
}
static inline uint32_t rr_pop32(RrMachine *m) {
    uint32_t v = rr_read32(m, m->cpu.a[7]);
    m->cpu.a[7] += 4;
    return v;
}
static inline uint16_t rr_pop16(RrMachine *m) {
    uint16_t v = rr_read16(m, m->cpu.a[7]);
    m->cpu.a[7] += 2;
    return v;
}

static inline unsigned rr_popcount16(uint32_t v) {
    unsigned n = 0;
    for (v &= 0xFFFF; v; v >>= 1) n += v & 1;
    return n;
}

/* MULU/MULS/DIVU/DIVS: result, flags, and the data-dependent part of the cost. */
static inline void rr_mulu(RrCpu *c, int n, uint32_t s) {
    uint32_t r = (c->d[n] & 0xFFFF) * (s & 0xFFFF);
    c->d[n] = r;
    rr_nz(c, r, 4);
    c->cycles += 38 + 2 * rr_popcount16(s);
}

static inline void rr_muls(RrCpu *c, int n, uint32_t s) {
    uint32_t r = (uint32_t)((int32_t)(int16_t)c->d[n] * (int32_t)(int16_t)s);
    c->d[n] = r;
    rr_nz(c, r, 4);
    c->cycles += 38 + 2 * rr_popcount16((s ^ (s << 1)) & 0xFFFF);
}

static inline void rr_divu(RrMachine *m, int n, uint32_t s) {
    RrCpu *c = &m->cpu;
    uint32_t dividend = c->d[n], divisor = s & 0xFFFF;
    if (divisor == 0) rr_fatal(m, "divide by zero trap is not supported");
    uint16_t f = c->sr & (uint16_t)~(RR_N | RR_Z | RR_V | RR_C);
    if ((dividend >> 16) >= divisor) {
        c->sr = f | RR_V | RR_N;
        c->cycles += 10;
        return;
    }
    uint32_t q = dividend / divisor, rem = dividend % divisor;
    c->d[n] = rem << 16 | q;
    c->sr = f | rr_nz_bits(q, 2);
    /* Timing after Jorge Cwik's analysis of the 68000 divide microcode. */
    unsigned mcycles = 38;
    uint32_t hdivisor = divisor << 16, work = dividend;
    for (int i = 0; i < 15; i++) {
        uint32_t before = work;
        work <<= 1;
        if (before & 0x80000000u) {
            work -= hdivisor;
        } else {
            mcycles += 2;
            if (work >= hdivisor) {
                work -= hdivisor;
                mcycles--;
            }
        }
    }
    c->cycles += mcycles * 2;
}

static inline void rr_divs(RrMachine *m, int n, uint32_t s) {
    RrCpu *c = &m->cpu;
    int32_t dividend = (int32_t)c->d[n];
    int32_t divisor = (int16_t)s;
    if (divisor == 0) rr_fatal(m, "divide by zero trap is not supported");
    uint16_t f = c->sr & (uint16_t)~(RR_N | RR_Z | RR_V | RR_C);
    unsigned mcycles = 6;
    if (dividend < 0) mcycles++;
    uint32_t adividend = dividend < 0 ? 0u - (uint32_t)dividend : (uint32_t)dividend;
    uint32_t adivisor = divisor < 0 ? (uint32_t)-divisor : (uint32_t)divisor;
    if ((adividend >> 16) >= adivisor) {
        c->sr = f | RR_V | RR_N;
        c->cycles += (mcycles + 2) * 2;
        return;
    }
    uint32_t aquot = adividend / adivisor;
    mcycles += 55;
    if (divisor >= 0) {
        if (dividend >= 0) mcycles--;
        else mcycles++;
    }
    for (uint32_t bits = aquot, i = 0; i < 15; i++, bits <<= 1) {
        if (!(bits & 0x8000)) mcycles++;
    }
    c->cycles += mcycles * 2;
    int32_t q = (int32_t)aquot, rem = (int32_t)(adividend % adivisor);
    if ((dividend < 0) != (divisor < 0)) q = -q;
    if (dividend < 0) rem = -rem;
    if (q > 32767 || q < -32768) {
        c->sr = f | RR_V | RR_N;
        return;
    }
    c->d[n] = ((uint32_t)rem & 0xFFFF) << 16 | ((uint32_t)q & 0xFFFF);
    c->sr = f | rr_nz_bits((uint32_t)q & 0xFFFF, 2);
}

#endif
