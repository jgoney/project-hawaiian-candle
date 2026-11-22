/* Instruction dispatch, interrupt entry, fatal errors, and save/load. */
#ifndef RR_RUN_H
#define RR_RUN_H

#include <stddef.h>
#include <stdio.h>

#include "rr_machine.h"

typedef void (*RrInsnFn)(RrMachine *m);

/* Defined by the generated code. rr_lookup returns NULL for untranslated addresses. */
RrInsnFn rr_lookup(uint32_t pc);
extern const uint32_t rr_code_rom_crc32;

/* Execute one instruction. Fatal if the PC has no translation. */
void rr_step(RrMachine *m);

/* 68000 autovectored interrupt entry for `level` (1-7). */
void rr_interrupt(RrMachine *m, int level);

/* A host can take over fatal errors. The handler receives the complete
 * "RR2FATAL ..." message. If it returns, the process still exits with status
 * 2; a host that wants to carry on must leave by longjmp. The handler is
 * process-wide (the one piece of global state in the core); NULL restores the
 * default, which prints the message to stderr. */
typedef void (*RrFatalHandler)(const char *message, void *ctx);
void rr_set_fatal_handler(RrFatalHandler handler, void *ctx);

/* Report "RR2FATAL pc=... insn=...: message" and exit with status 2, unless a
 * fatal handler takes over. */
_Noreturn void rr_fatal(const RrMachine *m, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

uint32_t rr_crc32(const uint8_t *data, size_t n);

/* Save and restore CPU state and RAM. Both return 0 on success. rr_load
 * refuses a file with the wrong magic or one saved against a different ROM. */
int rr_save(const RrMachine *m, FILE *f);
int rr_load(RrMachine *m, FILE *f);

/* Hardware backed by a recorded access log (hw.txt). Every access must match
 * the next record; any mismatch is fatal. */
RrHw *rr_hw_replay_open(const char *hw_log_path, const RrMachine *m);
size_t rr_hw_replay_remaining(RrHw *hw);
size_t rr_hw_replay_position(RrHw *hw);
void rr_hw_replay_seek(RrHw *hw, size_t position);

#endif
