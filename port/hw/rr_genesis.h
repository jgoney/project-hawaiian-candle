/* Sega Genesis hardware as seen by the 68000: everything outside ROM and work RAM.
 *
 * Implements RrHw, so a machine uses it by pointing m->hw at &genesis->hw.
 * The video chip is modelled; the Z80, FM chip and PSG are stubs that accept
 * writes. Timing comes from the 68000 cycle count since power-on.
 */
#ifndef RR_GENESIS_H
#define RR_GENESIS_H

#include "rr_machine.h"
#include "rr_vdp.h"

/* Video timing is a function of absolute time, measured in master clocks.
 *
 * The rates are MAME's, because MAME is what the reference recordings were
 * made on: a 53,693,175 Hz master clock, and a 68000 clock of one seventh of
 * that truncated to whole hertz. A line is 3420 master clocks (488.571 CPU
 * cycles) and a frame is 262 lines. Lines are numbered as the V counter shows
 * them at the start of the line, so line 0 is the first display line and
 * vertical blanking begins at line 224.
 *
 * Fitted to MAME 0.289 (analysis/race-conformance.md): with these rates and
 * RR_CLOCK_PHASE, kept in attoseconds as MAME keeps them (rr_genesis.c), the
 * model reproduces all 187,920 counter reads in the six race trace windows
 * from the cycle count alone. */
#define RR_MASTER_HZ 53693175ull
#define RR_CPU_HZ 7670453ull
#define RR_MASTER_PER_LINE 3420
#define RR_LINES_PER_FRAME 262
#define RR_MASTER_PER_FRAME ((uint64_t)RR_MASTER_PER_LINE * RR_LINES_PER_FRAME)
#define RR_CYCLES_PER_LINE 488 /* the whole part, for callers that only need a rough figure */
#define RR_VBLANK_LINE 224
/* Master clocks between an instruction's start and the moment MAME's video
 * chip sees the time of its bus access, plus MAME's line phase. */
#define RR_CLOCK_PHASE 56
/* MAME's cycle count at the first instruction after reset. The port counts
 * the same way (rr_genesis_power_on), because video time and the interrupt
 * entry delay both depend on the absolute count. */
#define RR_RESET_CYCLES 34

/* Bits of pad_buttons, set when the button is held. */
enum {
    RR_PAD_UP = 0x01, RR_PAD_DOWN = 0x02, RR_PAD_LEFT = 0x04, RR_PAD_RIGHT = 0x08,
    RR_PAD_B = 0x10, RR_PAD_C = 0x20, RR_PAD_A = 0x40, RR_PAD_START = 0x80
};

/* One write to the video chip, as reported to a tool (video_log below). */
enum { RR_VIDEO_REGISTER, RR_VIDEO_VRAM, RR_VIDEO_CRAM, RR_VIDEO_VSRAM, RR_VIDEO_DMA, RR_VIDEO_FILL,
       RR_VIDEO_COPY };
typedef struct {
    uint64_t frame; /* where the beam was */
    int line;
    uint8_t kind;   /* RR_VIDEO_* */
    uint8_t memory; /* DMA only: RR_VIDEO_VRAM, _CRAM or _VSRAM, the memory written */
    uint32_t target; /* register number, or address in that memory */
    uint32_t value; /* value written; DMA: length in words; fill and copy: length in bytes */
    uint32_t source; /* DMA: 68000 address of the data; fill: the byte; copy: VRAM address */
} RrVideoEvent;

typedef struct RrGenesis RrGenesis;
struct RrGenesis {
    RrHw hw; /* must be first */
    RrMachine *m;
    RrVdp vdp;
    uint8_t z80_ram[0x2000];
    uint8_t pad_ctrl[2], pad_latch[2];
    uint16_t pad_buttons[2];
    uint8_t z80_busreq, z80_reset;
    /* The sound side (rr_sound.h), or NULL: no sound, the default. When set,
     * it is brought up to the 68000's time whenever the 68000 touches the
     * Z80's memory, bus or reset line, and at the end of every frame. The
     * owner creates and frees it; it shares z80_ram. */
    struct RrSound *sound;
    /* For reports, never read by the model: 68000 writes to the Z80's memory
     * (below A04000) and to the square-wave chip. */
    uint64_t z80_ram_writes, psg_writes;

    /* Self-timed running (rr_genesis_run_frame). */
    uint8_t vint_pending, hint_pending;
    uint8_t resume_needed;                       /* the frame schedule must be found from the clock */
    uint64_t frame_index;                        /* frame being run */
    int next_event;                              /* next scheduled event within it */
    int hint_counter;
    uint64_t hint_raised_at, vint_raised_at; /* cycle count at which each waiting interrupt was raised */
    uint64_t interrupt_noted_by;  /* latest raise the last instruction would have noticed */
    unsigned cpu_percent;         /* CPU speed; 100 is the original */
    uint64_t speed_base_cycles;   /* cycle count when the speed was last set */
    unsigned __int128 speed_base_time; /* hardware time (attoseconds) at that moment */
    uint64_t vblank_entries;                     /* level 6 interrupts delivered so far */
    void (*vblank_hook)(RrGenesis *g, void *ctx); /* called as each VBlank handler is entered */
    void *vblank_hook_ctx;
    void (*step_hook)(RrGenesis *g, void *ctx);  /* called before every instruction; for tools */
    void *step_hook_ctx;
    uint32_t frame[RR_SCREEN_W * RR_SCREEN_H];   /* the picture, updated line by line */
    /* For tools, both off (NULL) by default and never read by the model: where
     * each pixel of `frame` came from (RR_SCREEN_W x RR_SCREEN_H entries) and
     * the video state each line was drawn from (RR_SCREEN_H entries), filled
     * at the moment the line is drawn. */
    RrPixelSource *sources;
    RrVdpLineState *line_state;
    /* An enlarged picture beside `frame`, off (NULL) by default and never read
     * by the model: RR_SCREEN_W * hires_scale by RR_SCREEN_H * hires_scale
     * pixels, each display line drawn as hires_scale rows at the moment the
     * line is drawn (rr_vdp_render_line_scaled). hires_begin, if set, is
     * called just before line 0 of each frame is drawn, so the owner can say
     * which lines get a sideways shift per row: on a line where hires_have is
     * 1, row r takes hires_shift_a/b[line * hires_scale + r], provided the
     * whole part of row 0's shifts is still that line's own scroll value when
     * the line is drawn (so shifts chosen for a table the game has since
     * rewritten are not used). If hires_expect_a/b are set, that check is made
     * on them instead (same indexing): they hold the whole-pixel shifts the
     * table must have, for shifts that are deliberately finer than the table.
     * All NULL means no shifts: every row repeats the original line. */
    uint32_t *hires;
    int hires_scale;
    const int32_t *hires_shift_a, *hires_shift_b;
    const int32_t *hires_expect_a, *hires_expect_b;
    const uint8_t *hires_have;
    void (*hires_begin)(RrGenesis *g, void *ctx);
    void *hires_ctx;
    /* If set, called just before each display line is drawn (after
     * hires_begin on line 0), with hires_ctx: for measuring what a line is
     * drawn from. It must not change the model. */
    void (*hires_line)(RrGenesis *g, int line, void *ctx);
    /* The same just after the line is drawn, the video chip still as the
     * line was drawn from it. */
    void (*hires_line_drawn)(RrGenesis *g, int line, void *ctx);
    /* Finer pictures for some sprites of the enlarged picture
     * (rr_vdp_render_line_fine), or NULL: set by hires_begin, with
     * hires_sprites_stamp = vdp.vram_writes at that moment. A line drawn after
     * video memory has changed since does not use them (the sprites and tiles
     * they describe may be gone) and is counted in hires_sprites_stale_lines. */
    const RrFineSprites *hires_sprites;
    uint64_t hires_sprites_stamp;
    uint64_t hires_sprites_stale_lines;
    /* A wider enlarged picture (rr_vdp_render_line_wide), set by hires_begin
     * or left NULL: with hires_wide set, `hires` must hold rows of
     * (RR_SCREEN_W + 2 * margin) * hires_scale pixels, row r of line l at
     * hires[(l * hires_scale + r) * that width]. hires_beyond_a/b (indexed
     * like the shifts) are used only on a line whose shifts are used.
     * hires_margin_sprites is under the same stamp rule as hires_sprites: a
     * line that drops either counts once in hires_sprites_stale_lines.
     * For tools: hires_narrow, if set, receives the same frame with no
     * margin (RR_SCREEN_W * hires_scale wide), and hires_sources the sources
     * of the rows of `hires`. */
    const RrWide *hires_wide;
    const int32_t *hires_beyond_a, *hires_beyond_b;
    const RrFineSprites *hires_margin_sprites;
    uint32_t *hires_narrow;
    RrPixelSource *hires_sources;
    /* Also for tools and off (NULL) by default: called for every write the
     * 68000 makes to the video chip, before the write takes effect. */
    void (*video_log)(void *ctx, const RrVideoEvent *e);
    void *video_log_ctx;
};

RrGenesis *rr_genesis_new(RrMachine *m);
void rr_genesis_free(RrGenesis *g);

/* Master clocks since power-on at the current instruction. */
uint64_t rr_genesis_master_time(const RrGenesis *g);
/* The time the sound side runs on: master clocks since the first frame began. */
uint64_t rr_genesis_sound_time(const RrGenesis *g);
/* 68000 reset: registers cleared, supervisor mode, interrupts masked, stack
 * pointer and start address from the ROM's first two vectors, and the cycle
 * count where MAME has it at the first instruction. */
void rr_genesis_power_on(RrGenesis *g);
/* CPU speed in percent of the original, 100 to 6400 (clamped). At 800 the
 * 68000 gets eight times as many cycles in every video line; the video chip's
 * own timing (lines per frame, interrupt positions, counters) is unchanged as
 * the game sees it. Hardware time advances cycles x 100 / percent from the
 * moment of the call, so no time is lost at a change. Call between frames. */
void rr_genesis_set_cpu_speed(RrGenesis *g, unsigned percent);
unsigned rr_genesis_cpu_speed(const RrGenesis *g);

/* Run to the start of the next frame. The frame interrupt (level 6) is raised
 * on line 224 when register 1 bit 5 is set, and line interrupts (level 4) by
 * the counter in register 10 when register 0 bit 4 is set. Interrupts are
 * taken between instructions when the CPU's mask allows. Each display line is
 * rendered into `frame` from the state at its start, before that line's 68000
 * time runs, so a line interrupt handler's changes show from the following
 * line. Returns the number of instructions executed. */
uint64_t rr_genesis_run_frame(RrGenesis *g);

/* Tell the model that the cycle count was replaced from outside. The frame in
 * progress stops at once; the next rr_genesis_run_frame call carries on from
 * the new position to the end of whatever frame that is. */
void rr_genesis_time_changed(RrGenesis *g);

/* Write `frame` as a binary PPM. Returns 0 on success. */
int rr_genesis_write_ppm(const RrGenesis *g, const char *path);

/* Position of the beam, from the cycle count. */
int rr_genesis_line(const RrGenesis *g);

#endif
