/* Machine state for the Road Rash II native port.
 *
 * Everything that determines future behaviour lives in RrMachine (plus the
 * hardware object it points to). Nothing is kept in static variables or on
 * the native call stack between instructions.
 */
#ifndef RR_MACHINE_H
#define RR_MACHINE_H

#include <stdint.h>

typedef struct RrHw RrHw;
struct RrHw {
    uint16_t (*read)(RrHw *hw, uint32_t addr, int size); /* size 1 or 2 */
    void (*write)(RrHw *hw, uint32_t addr, int size, uint16_t value);
    void (*close)(RrHw *hw);
};

typedef struct {
    uint32_t d[8], a[8], pc;
    uint32_t usp;     /* user stack pointer (a[7] is always the supervisor stack) */
    uint32_t last_pc; /* address of the instruction executed most recently */
    uint16_t sr;
    uint64_t cycles;  /* 68000 cycles charged so far */
    uint64_t insns;   /* instructions executed so far */
} RrCpu;

typedef struct RrMachine RrMachine;
struct RrMachine {
    RrCpu cpu;
    uint8_t ram[0x10000];
    const uint8_t *rom;
    uint32_t rom_size;
    uint32_t rom_crc32;
    RrHw *hw;
    /* For tools; off (NULL) by default and not part of the saved state. Called
     * after every byte or word write to work RAM that touches offsets
     * ram_watch_start up to, not including, ram_watch_end. A long write
     * arrives as two word writes. */
    void (*ram_watch)(RrMachine *m, uint32_t offset, int size, uint32_t value, void *ctx);
    void *ram_watch_ctx;
    uint32_t ram_watch_start, ram_watch_end;
};

#endif
