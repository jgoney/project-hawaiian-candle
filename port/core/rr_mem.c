#include "rr_mem.h"

#include "rr_run.h"

#define ADDR_MASK 0xFFFFFFu
#define RAM_BASE 0xE00000u

static inline int is_ram(uint32_t addr) { return addr >= RAM_BASE; }
static inline int is_rom(const RrMachine *m, uint32_t addr) { return addr < m->rom_size; }

static RrHw *hw_or_die(RrMachine *m, uint32_t addr) {
    if (m->hw == NULL) rr_fatal(m, "hardware access at %06X with no hardware attached", addr);
    return m->hw;
}

static void check_even(RrMachine *m, uint32_t addr) {
    if (addr & 1) rr_fatal(m, "word access at odd address %06X", addr);
}

uint8_t rr_read8(RrMachine *m, uint32_t addr) {
    addr &= ADDR_MASK;
    if (is_ram(addr)) return m->ram[addr & 0xFFFF];
    if (is_rom(m, addr)) return m->rom[addr];
    return (uint8_t)hw_or_die(m, addr)->read(m->hw, addr, 1);
}

uint16_t rr_read16(RrMachine *m, uint32_t addr) {
    addr &= ADDR_MASK;
    check_even(m, addr);
    if (is_ram(addr)) {
        uint32_t o = addr & 0xFFFF;
        return (uint16_t)(m->ram[o] << 8 | m->ram[o + 1]);
    }
    if (is_rom(m, addr)) return (uint16_t)(m->rom[addr] << 8 | m->rom[addr + 1]);
    return hw_or_die(m, addr)->read(m->hw, addr, 2);
}

uint32_t rr_read32(RrMachine *m, uint32_t addr) {
    uint32_t hi = rr_read16(m, addr);
    uint32_t lo = rr_read16(m, addr + 2);
    return hi << 16 | lo;
}

void rr_write8(RrMachine *m, uint32_t addr, uint8_t v) {
    addr &= ADDR_MASK;
    if (is_ram(addr)) {
        uint32_t o = addr & 0xFFFF;
        m->ram[o] = v;
        if (m->ram_watch != NULL && o >= m->ram_watch_start && o < m->ram_watch_end) {
            m->ram_watch(m, o, 1, v, m->ram_watch_ctx);
        }
        return;
    }
    if (is_rom(m, addr)) rr_fatal(m, "write to ROM address %06X", addr);
    hw_or_die(m, addr)->write(m->hw, addr, 1, v);
}

void rr_write16(RrMachine *m, uint32_t addr, uint16_t v) {
    addr &= ADDR_MASK;
    check_even(m, addr);
    if (is_ram(addr)) {
        uint32_t o = addr & 0xFFFF;
        m->ram[o] = (uint8_t)(v >> 8);
        m->ram[o + 1] = (uint8_t)v;
        if (m->ram_watch != NULL && o + 1 >= m->ram_watch_start && o < m->ram_watch_end) {
            m->ram_watch(m, o, 2, v, m->ram_watch_ctx);
        }
        return;
    }
    if (is_rom(m, addr)) rr_fatal(m, "write to ROM address %06X", addr);
    hw_or_die(m, addr)->write(m->hw, addr, 2, v);
}

void rr_write32(RrMachine *m, uint32_t addr, uint32_t v) {
    rr_write16(m, addr, (uint16_t)(v >> 16));
    rr_write16(m, addr + 2, (uint16_t)v);
}
