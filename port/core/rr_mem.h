/* 68000 bus access: big-endian, 24-bit addresses, RAM mirrored over 0xE00000-0xFFFFFF. */
#ifndef RR_MEM_H
#define RR_MEM_H

#include "rr_machine.h"

uint8_t rr_read8(RrMachine *m, uint32_t addr);
uint16_t rr_read16(RrMachine *m, uint32_t addr);
uint32_t rr_read32(RrMachine *m, uint32_t addr);
void rr_write8(RrMachine *m, uint32_t addr, uint8_t v);
void rr_write16(RrMachine *m, uint32_t addr, uint16_t v);
void rr_write32(RrMachine *m, uint32_t addr, uint32_t v);

#endif
