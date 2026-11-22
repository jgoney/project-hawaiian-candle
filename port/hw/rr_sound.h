/* The Genesis's sound side: the Z80 that runs the game's sound program, the
 * YM2612 FM chip it plays, and the samples that come out.
 *
 * Time is in master clocks (53,693,175 a second), the same count
 * rr_genesis_master_time gives. The owner tells the unit how far time has
 * got (rr_sound_run_to) and the Z80 and the chip catch up; time never goes
 * back. The Z80 and the FM chip are other people's models
 * (third_party/README.md). The square-wave chip (PSG) is not modelled yet:
 * writes to it are counted and dropped. */
#ifndef RR_SOUND_H
#define RR_SOUND_H

#include <stddef.h>
#include <stdint.h>

#define RR_SOUND_HZ 53267               /* samples a second: master / 7 / 144 */
#define RR_SOUND_MASTER_PER_Z80 15      /* master clocks in one Z80 cycle */
#define RR_SOUND_MASTER_PER_FM_CLOCK 42 /* 24 of these make one sample */
#define RR_SOUND_MASTER_PER_SAMPLE (RR_SOUND_MASTER_PER_FM_CLOCK * 24)
#define RR_SOUND_MASTER_PER_FRAME (3420ull * 262)
#define RR_SOUND_INTERRUPT_AT (224ull * 3420 + 205) /* into each frame: the vertical blank */
#define RR_SOUND_INTERRUPT_FOR 3420ull              /* withdrawn a line later if not taken */
#define RR_SOUND_BUFFER_SECONDS 4                   /* older samples are dropped */

typedef struct RrSound RrSound;
/* Called for every write to the FM chip, the Z80's and the 68000's alike. */
typedef void (*RrSoundFmLog)(void *ctx, uint64_t master, unsigned port, uint8_t value, int from_68000);

/* `z80_ram` is the Z80's 8 KB of memory, shared with whoever else writes it.
 * The Z80 starts with its reset line held: it runs nothing until released. */
RrSound *rr_sound_new(const uint8_t *rom, uint32_t rom_size, uint8_t *z80_ram);
void rr_sound_free(RrSound *s);
/* As switched on: time starts again from 0, the Z80's reset is held, the FM
 * chip is reset and waiting samples are dropped. */
void rr_sound_power_on(RrSound *s);
/* Start from this time without running through what came before (for a
 * unit attached to a machine that is already running). */
void rr_sound_set_time(RrSound *s, uint64_t master);
void rr_sound_run_to(RrSound *s, uint64_t master);
/* The 68000 has the Z80's bus: the Z80 stands still. */
void rr_sound_set_bus_held(RrSound *s, uint64_t master, int held);
/* The reset line. Taking it also resets the FM chip; letting go starts the
 * Z80 at address 0. */
void rr_sound_set_reset_held(RrSound *s, uint64_t master, int held);
/* A 68000 write to the FM chip (ports 0 to 3 at A04000). */
void rr_sound_fm_write(RrSound *s, uint64_t master, unsigned port, uint8_t value);
void rr_sound_set_fm_log(RrSound *s, RrSoundFmLog log, void *ctx);
/* Stereo frames waiting, and taking them (interleaved left, right). */
size_t rr_sound_available(const RrSound *s);
size_t rr_sound_take(RrSound *s, int16_t *out, size_t frames);
uint64_t rr_sound_z80_steps(const RrSound *s); /* instructions run so far */
uint64_t rr_sound_fm_writes(const RrSound *s);       /* by either processor */
uint64_t rr_sound_fm_writes_68000(const RrSound *s); /* those the 68000 made itself */
uint64_t rr_sound_psg_writes(const RrSound *s);      /* the Z80's, to 0x7F10-0x7F17 */
uint64_t rr_sound_now(const RrSound *s);             /* the latest time given */

#endif
