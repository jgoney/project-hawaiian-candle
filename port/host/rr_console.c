#include "rr_console.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rr_genesis.h"
#include "rr_mem.h"
#include "rr_between.h"
#include "rr_dashes.h"
#include "rr_objects.h"
#include "rr_road.h"
#include "rr_wide.h"
#include "rr_resample.h"
#include "rr_run.h"
#include "rr_sound.h"

/* One thing done to the console, for the session. */
enum { EV_PAD, EV_SPEED, EV_RESET, EV_SCALE, EV_CRISP, EV_FINE_ROAD, EV_FINE_OBJECTS, EV_BETWEEN, EV_SOFT, EV_WIDE,
       EV_KINDS };
static const char *const event_names[EV_KINDS] = {"pad", "speed", "reset", "scale", "crisp", "fine-road",
                                                  "fine-objects", "between", "soft-objects", "wide"};
typedef struct {
    uint64_t frame; /* frames run since opening when it was done */
    uint8_t kind, port;
    uint32_t value;
} SessionEvent;

struct RrConsole {
    RrMachine m;
    RrGenesis *hw;
    uint8_t *rom;
    unsigned buttons[RR_CONSOLE_PORTS];
    unsigned cpu_percent;
    int scale, crisp, fine_objects, soft_objects;
    uint32_t *scaled; /* the enlarged picture, or NULL at scale 1 */
    int wide;         /* the wide view is asked for */
    int margin;       /* game pixels each side of `scaled`: RR_WIDE_MARGIN while it is in effect, else 0 */
    RrWide wide_spec;
    RrFineSprites margin_fine; /* the frame's margin objects */
    uint64_t wide_frames;
    RrRoadTracker road;
    RrObjectTracker objects;
    RrBetween *between; /* in-between pictures: the updates kept and the scene for the frame */
    RrStrips *strips;   /* the centre-line dashes of a frame drawn from the update on screen */
    RrFineSprites frame_fine; /* and the sprites that go with them */
    uint64_t frames;
    int halted;
    char halt_message[512];
    int sound_on;
    RrSound *sound;        /* the sound side, attached to hw, or NULL */
    RrResample *resample;  /* chip rate to the caller's rate */
    jmp_buf escape;
    SessionEvent *events;
    size_t event_count, event_room;
    int events_lost;         /* there was no memory for one: the list is not whole */
    uint64_t session_frames; /* frames run since opening */
};

static void note_event(RrConsole *c, int kind, int port, uint32_t value) {
    if (c->event_count == c->event_room) {
        size_t room = c->event_room != 0 ? c->event_room * 2 : 256;
        SessionEvent *events = realloc(c->events, room * sizeof *events);
        if (events == NULL) {
            c->events_lost = 1;
            return;
        }
        c->events = events;
        c->event_room = room;
    }
    c->events[c->event_count++] = (SessionEvent){c->session_frames, (uint8_t)kind, (uint8_t)port, value};
}

static void fail(char *error, size_t size, const char *fmt, const char *path, uint32_t a,
                 uint32_t b) {
    if (error != NULL && size > 0) snprintf(error, size, fmt, path, a, b);
}

/* While the crisp road is on: note the road points each time the game starts
 * turning them into the picture, and the objects each time it is about to
 * draw them. */
static void note_road_at_its_routine(RrGenesis *g, void *ctx) {
    RrConsole *c = ctx;
    uint32_t pc = g->m->cpu.pc & 0xFFFFFF;
    if (pc == RR_ROAD_NOTE_PC) rr_road_tracker_note(&c->road, c->m.ram, c->m.rom, c->m.rom_size);
    else if (pc == RR_ROAD_INPUTS_PC) rr_road_tracker_note_inputs(&c->road, c->m.ram);
    else if (pc == RR_OBJECTS_NOTE_PC && c->fine_objects) rr_console_note_objects(c);
    else if (pc == RR_DASHES_PC) rr_console_note_dashes(c);
}

/* Before a frame's first line: which road, if any, the enlarged picture is drawn with. */
static void choose_road(RrGenesis *g, void *ctx) {
    RrConsole *c = ctx;
    const RrRoadShifts *shifts = NULL;
    uint64_t race_before = c->road.race_frames;
    if (c->crisp && c->scaled != NULL) {
        shifts = rr_road_tracker_choose(&c->road, c->m.ram, c->m.rom, c->m.rom_size, &g->vdp, c->scale);
    }
    g->hires_shift_a = shifts != NULL ? shifts->a : NULL;
    g->hires_shift_b = shifts != NULL ? shifts->b : NULL;
    g->hires_have = shifts != NULL ? shifts->have : NULL;
    /* What each line's entry in video memory must still be when the line is drawn. */
    g->hires_expect_a = shifts != NULL ? c->road.whole.a : NULL;
    g->hires_expect_b = shifts != NULL ? c->road.whole.b : NULL;
    /* And which objects, if any, are drawn from their full pictures. */
    const RrFineSprites *fine = NULL;
    if (c->crisp && c->scaled != NULL && c->fine_objects) fine = rr_object_tracker_choose(&c->objects, &g->vdp);
    /* The centre-line dashes as strips, where they are proven. */
    if (shifts != NULL && rr_dashes_frame_wide(&c->road, rr_object_tracker_chosen(&c->objects), fine, &g->vdp,
                                               c->scale, c->margin, c->strips, &c->frame_fine))
        fine = &c->frame_fine;
    if (fine != NULL && c->soft_objects) {
        if (fine != &c->frame_fine) c->frame_fine = *fine;
        c->frame_fine.soft = 1;
        fine = &c->frame_fine;
    }
    g->hires_sprites = fine;
    g->hires_sprites_stamp = g->vdp.vram_writes;
    /* The wide view: the lines of a race's view are drawn wide, everything else between margins. */
    g->hires_wide = NULL;
    g->hires_beyond_a = g->hires_beyond_b = NULL;
    g->hires_margin_sprites = NULL;
    RrBetween *b = c->between;
    b->margin = c->margin;
    if (c->margin > 0) {
        int race = c->road.race_frames != race_before;
        c->wide_spec.lines = race ? RR_WIDE_LINES : 0;
        g->hires_wide = &c->wide_spec;
        if (race) c->wide_frames++;
        if (shifts != NULL) {
            g->hires_beyond_a = shifts->beyond_a;
            g->hires_beyond_b = shifts->beyond_b;
        }
        const RrFineSprites *beside = fine != NULL ? rr_object_tracker_chosen_margin(&c->objects, c->margin) : NULL;
        if (race && beside != NULL) {
            c->margin_fine = *beside;
            c->margin_fine.soft = (uint8_t)c->soft_objects;
            g->hires_margin_sprites = &c->margin_fine;
        }
    }
    /* And, if asked for and possible, the scene between two updates in place of all that. */
    if (b->mode == RR_BETWEEN_OFF || !c->crisp || c->scaled == NULL || !c->fine_objects || c->road.mode != RR_ROAD_FINE)
        return;
    rr_between_note(b, c->frames, &c->road, &c->objects, &g->vdp); /* forgets the updates if this is no race frame */
    if (shifts == NULL || !rr_between_frame(b, c->frames, &c->road, &c->objects, &g->vdp, c->m.rom, c->m.rom_size, c->scale)) return;
    g->hires_shift_a = b->shifts.a;
    g->hires_shift_b = b->shifts.b;
    g->hires_have = b->shifts.have;
    g->hires_expect_a = b->expect.a;
    g->hires_expect_b = b->expect.b;
    b->fine.soft = (uint8_t)c->soft_objects;
    g->hires_sprites = &b->fine;
    if (c->margin > 0) {
        g->hires_beyond_a = b->shifts.beyond_a;
        g->hires_beyond_b = b->shifts.beyond_b;
        b->margin_fine.soft = (uint8_t)c->soft_objects;
        g->hires_margin_sprites = b->margin_ok ? &b->margin_fine : NULL;
    }
}

static void apply_picture_settings(RrConsole *c) {
    int crisp = c->crisp && c->scaled != NULL;
    c->hw->hires = c->scaled;
    c->hw->hires_scale = c->scale;
    c->hw->hires_begin = choose_road;
    c->hw->hires_ctx = c;
    c->hw->step_hook = crisp ? note_road_at_its_routine : NULL;
    c->hw->step_hook_ctx = c;
}

/* Give the machine a fresh sound side, or none. `from_power_on`: the machine
 * is about to be switched on, so the unit starts at time 0 with the Z80's
 * reset held; otherwise it joins a machine that is already running. */
static void attach_sound(RrConsole *c, int from_power_on) {
    rr_sound_free(c->sound);
    c->sound = NULL;
    c->hw->sound = NULL;
    if (c->resample != NULL) rr_resample_reset(c->resample);
    if (!c->sound_on) return;
    if (c->resample == NULL) c->resample = rr_resample_new(RR_SOUND_HZ);
    c->sound = rr_sound_new(c->rom, c->m.rom_size, c->hw->z80_ram);
    if (c->sound == NULL || c->resample == NULL) {
        rr_sound_free(c->sound);
        c->sound = NULL;
        return;
    }
    if (!from_power_on) rr_sound_set_time(c->sound, rr_genesis_sound_time(c->hw));
    c->hw->sound = c->sound;
}

static void power_on(RrConsole *c) {
    if (c->hw != NULL) rr_genesis_free(c->hw);
    memset(&c->m.cpu, 0, sizeof c->m.cpu);
    memset(c->m.ram, 0, sizeof c->m.ram);
    c->hw = rr_genesis_new(&c->m);
    c->m.hw = &c->hw->hw;
    attach_sound(c, 1);
    for (int port = 0; port < RR_CONSOLE_PORTS; port++) c->hw->pad_buttons[port] = (uint16_t)c->buttons[port];
    rr_genesis_power_on(c->hw);
    rr_genesis_set_cpu_speed(c->hw, c->cpu_percent);
    c->cpu_percent = rr_genesis_cpu_speed(c->hw);
    rr_road_tracker_reset(&c->road);
    rr_object_tracker_reset(&c->objects);
    rr_between_reset(c->between);
    apply_picture_settings(c);
    c->frames = 0;
    c->halted = 0;
    c->halt_message[0] = 0;
}

RrConsole *rr_console_open(const char *rom_path, char *error, size_t error_size) {
    FILE *f = fopen(rom_path, "rb");
    if (f == NULL) {
        fail(error, error_size, "cannot open ROM %s", rom_path, 0, 0);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    uint8_t *rom = size > 0 ? malloc((size_t)size) : NULL;
    if (rom == NULL || fread(rom, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(rom);
        fail(error, error_size, "cannot read ROM %s", rom_path, 0, 0);
        return NULL;
    }
    fclose(f);
    uint32_t crc = rr_crc32(rom, (size_t)size);
    if (crc != rr_code_rom_crc32) {
        free(rom);
        fail(error, error_size,
             "%s is not the ROM this build was translated from (CRC-32 %08X, expected %08X)",
             rom_path, crc, rr_code_rom_crc32);
        return NULL;
    }
    RrConsole *c = calloc(1, sizeof *c);
    if (c != NULL) c->between = calloc(1, sizeof *c->between);
    if (c != NULL) c->strips = calloc(1, sizeof *c->strips);
    if (c == NULL || c->between == NULL || c->strips == NULL) {
        if (c != NULL) free(c->between);
        if (c != NULL) free(c->strips);
        free(c);
        free(rom);
        fail(error, error_size, "out of memory opening %s", rom_path, 0, 0);
        return NULL;
    }
    c->rom = rom;
    c->m.rom = rom;
    c->m.rom_size = (uint32_t)size;
    c->m.rom_crc32 = crc;
    c->scale = 1;
    c->fine_objects = 1;
    rr_wide_blocks(c->wide_spec.block);
    rr_wide_dashboard(&c->wide_spec);
    c->wide_spec.lines = 0;
    c->wide_spec.panel = 0x000000;
    c->wide_spec.ground_backdrop = RR_WIDE_ROAD_BACKDROP;
    power_on(c);
    if (error != NULL && error_size > 0) error[0] = 0;
    return c;
}

void rr_console_close(RrConsole *c) {
    if (c == NULL) return;
    rr_sound_free(c->sound);
    rr_resample_free(c->resample);
    rr_genesis_free(c->hw);
    free(c->scaled);
    free(c->between);
    free(c->strips);
    free(c->rom);
    free(c->events);
    free(c);
}

void rr_console_reset(RrConsole *c) {
    note_event(c, EV_RESET, 0, 0);
    power_on(c);
}

void rr_console_set_sound(RrConsole *c, int on) {
    on = on != 0;
    if (on == c->sound_on) return;
    c->sound_on = on;
    attach_sound(c, 0);
}

int rr_console_sound(const RrConsole *c) { return c->sound_on && c->sound != NULL; }

size_t rr_console_take_sound(RrConsole *c, double out_hz, float *out, size_t max_frames) {
    if (c->sound == NULL) return 0;
    if (!(out_hz >= RR_RESAMPLE_MIN_HZ && out_hz <= RR_RESAMPLE_MAX_HZ)) return 0;
    int16_t chunk[2 * 1024];
    /* Sound nobody took in time is let go, oldest first. */
    size_t waiting = rr_sound_available(c->sound);
    while (waiting > RR_SOUND_HZ / 2) {
        size_t excess = waiting - RR_SOUND_HZ / 2;
        waiting -= rr_sound_take(c->sound, chunk, excess < 1024 ? excess : 1024);
    }
    /* First what the converter still holds from last time, then new sound for
     * as long as there is room for it in `out`. What is left stays in the
     * sound unit, where the half-second rule above applies to it; the
     * converter never holds more than one chunk. */
    size_t used;
    size_t made = rr_resample_run(c->resample, out_hz, chunk, 0, &used, out, max_frames);
    while (made < max_frames) {
        size_t got = rr_sound_take(c->sound, chunk, 1024);
        if (got == 0) break;
        made += rr_resample_run(c->resample, out_hz, chunk, got, &used, out + 2 * made, max_frames - made);
    }
    return made;
}

void rr_console_set_buttons(RrConsole *c, int port, unsigned held) {
    if (port < 0 || port >= RR_CONSOLE_PORTS) return;
    if (held & RR_PAD_RIGHT) held &= ~(unsigned)RR_PAD_LEFT;
    if (held & RR_PAD_DOWN) held &= ~(unsigned)RR_PAD_UP;
    if ((held & 0xFF) != c->buttons[port]) note_event(c, EV_PAD, port, held & 0xFF);
    c->buttons[port] = held & 0xFF;
    c->hw->pad_buttons[port] = (uint16_t)c->buttons[port];
}

unsigned rr_console_buttons(const RrConsole *c, int port) {
    return (port < 0 || port >= RR_CONSOLE_PORTS) ? 0 : c->buttons[port];
}

/* Fatal handler while a frame runs: keep the message and leave the frame. */
static void halt(const char *message, void *ctx) {
    RrConsole *c = ctx;
    snprintf(c->halt_message, sizeof c->halt_message, "%s", message);
    longjmp(c->escape, 1);
}

int rr_console_run_frame(RrConsole *c) {
    if (c->halted) return 0;
    if (setjmp(c->escape) != 0) {
        rr_set_fatal_handler(NULL, NULL);
        c->halted = 1;
        return 0;
    }
    rr_set_fatal_handler(halt, c);
    rr_genesis_run_frame(c->hw);
    rr_set_fatal_handler(NULL, NULL);
    c->frames++;
    c->session_frames++;
    return 1;
}

const uint32_t *rr_console_frame(const RrConsole *c) { return c->hw->frame; }
const uint8_t *rr_console_ram(const RrConsole *c) { return c->m.ram; }
uint64_t rr_console_frames(const RrConsole *c) { return c->frames; }

const char *rr_console_halt_message(const RrConsole *c) {
    return c->halted ? c->halt_message : NULL;
}

void rr_console_set_cpu_speed(RrConsole *c, unsigned percent) {
    rr_genesis_set_cpu_speed(c->hw, percent);
    c->cpu_percent = rr_genesis_cpu_speed(c->hw);
    note_event(c, EV_SPEED, 0, c->cpu_percent);
}

unsigned rr_console_cpu_speed(const RrConsole *c) { return c->cpu_percent; }

/* The margin the wide view has with these settings: it needs the crisp road,
 * the fine road, the fine objects and an enlarged picture. */
static int margin_with(int wide, int crisp, int fine_road, int fine_objects, int scale) {
    return wide && crisp && fine_road && fine_objects && scale > 1 ? RR_WIDE_MARGIN : 0;
}

/* Give the enlarged picture this scale and margin. Returns 0, leaving the
 * console as it was, if there is no memory for it. */
static int resize(RrConsole *c, int scale, int margin) {
    if (scale == c->scale && margin == c->margin) return 1;
    uint32_t *scaled = NULL;
    if (scale > 1) {
        scaled = calloc((size_t)(RR_CONSOLE_W + 2 * margin) * RR_CONSOLE_H * (size_t)scale * (size_t)scale,
                        sizeof *scaled);
        if (scaled == NULL) return 0;
    }
    free(c->scaled);
    c->scaled = scaled;
    c->scale = scale;
    c->margin = margin;
    c->wide_spec.margin = margin;
    c->objects.margin = margin;
    return 1;
}

void rr_console_set_scale(RrConsole *c, int scale) {
    if (scale < 1) scale = 1;
    if (scale > RR_CONSOLE_SCALE_MAX) scale = RR_CONSOLE_SCALE_MAX;
    if (scale == c->scale) return;
    if (!resize(c, scale, margin_with(c->wide, c->crisp, c->road.mode == RR_ROAD_FINE, c->fine_objects, scale)))
        return; /* no memory: stay as it is */
    apply_picture_settings(c);
    note_event(c, EV_SCALE, 0, (uint32_t)scale);
}

void rr_console_set_wide(RrConsole *c, int on) {
    on = on != 0;
    if (!resize(c, c->scale, margin_with(on, c->crisp, c->road.mode == RR_ROAD_FINE, c->fine_objects, c->scale)))
        return;
    c->wide = on;
    apply_picture_settings(c);
    note_event(c, EV_WIDE, 0, (uint32_t)on);
}

int rr_console_wide(const RrConsole *c) { return c->wide; }

int rr_console_scaled_width(const RrConsole *c) {
    return c->scaled != NULL ? (RR_CONSOLE_W + 2 * c->margin) * c->scale : RR_CONSOLE_W;
}

uint64_t rr_console_wide_frames(const RrConsole *c) { return c->wide_frames; }

int rr_console_scale(const RrConsole *c) { return c->scale; }

const uint32_t *rr_console_scaled_frame(const RrConsole *c) {
    return c->scaled != NULL ? c->scaled : c->hw->frame;
}

void rr_console_set_crisp_road(RrConsole *c, int on) {
    on = on != 0;
    if (!resize(c, c->scale, margin_with(c->wide, on, c->road.mode == RR_ROAD_FINE, c->fine_objects, c->scale)))
        return;
    if (on != c->crisp) rr_between_forget(c->between);
    c->crisp = on;
    apply_picture_settings(c);
    note_event(c, EV_CRISP, 0, (uint32_t)c->crisp);
}

int rr_console_crisp_road(const RrConsole *c) { return c->crisp; }
uint64_t rr_console_crisp_frames(const RrConsole *c) { return c->road.crisp_frames; }
void rr_console_note_road(RrConsole *c) { rr_road_tracker_note(&c->road, c->m.ram, c->m.rom, c->m.rom_size); }

void rr_console_set_fine_road(RrConsole *c, int on) {
    if (!resize(c, c->scale, margin_with(c->wide, c->crisp, on != 0, c->fine_objects, c->scale))) return;
    if ((on ? RR_ROAD_FINE : RR_ROAD_WHOLE) != c->road.mode) rr_between_forget(c->between);
    c->road.mode = on ? RR_ROAD_FINE : RR_ROAD_WHOLE;
    apply_picture_settings(c);
    note_event(c, EV_FINE_ROAD, 0, on != 0);
}
int rr_console_fine_road(const RrConsole *c) { return c->road.mode == RR_ROAD_FINE; }
uint64_t rr_console_fine_frames(const RrConsole *c) { return c->road.fine_frames; }
void rr_console_note_road_inputs(RrConsole *c) { rr_road_tracker_note_inputs(&c->road, c->m.ram); }

void rr_console_set_fine_objects(RrConsole *c, int on) {
    /* Sets noted before it was switched off may still be there when it comes
     * back on; that is safe, because a set is only used if it is the one in
     * video memory. */
    if (!resize(c, c->scale, margin_with(c->wide, c->crisp, c->road.mode == RR_ROAD_FINE, on != 0, c->scale))) return;
    if ((on != 0) != c->fine_objects) rr_between_forget(c->between);
    c->fine_objects = on != 0;
    apply_picture_settings(c);
    note_event(c, EV_FINE_OBJECTS, 0, (uint32_t)c->fine_objects);
}
int rr_console_fine_objects(const RrConsole *c) { return c->fine_objects; }
uint64_t rr_console_fine_object_frames(const RrConsole *c) { return c->objects.fine_frames; }
void rr_console_note_objects(RrConsole *c) {
    rr_object_tracker_note(&c->objects, c->m.ram, c->m.rom, c->m.rom_size);
}

void rr_console_set_between(RrConsole *c, int mode) {
    if (mode < RR_BETWEEN_OFF || mode > RR_BETWEEN_LATE) return; /* ignored, as rr_between_set_mode would */
    rr_between_set_mode(c->between, mode);
    note_event(c, EV_BETWEEN, 0, (uint32_t)mode);
}
int rr_console_between(const RrConsole *c) { return c->between->mode; }
uint64_t rr_console_between_frames(const RrConsole *c) { return c->between->between_frames; }

void rr_console_note_dashes(RrConsole *c) { rr_dashes_note(&c->road, c->m.ram, c->m.rom, c->m.rom_size); }

void rr_console_set_soft_objects(RrConsole *c, int on) {
    c->soft_objects = on != 0;
    note_event(c, EV_SOFT, 0, (uint32_t)c->soft_objects);
}
int rr_console_soft_objects(const RrConsole *c) { return c->soft_objects; }

/* ---- the session ---- */

uint64_t rr_console_session_frames(const RrConsole *c) { return c->session_frames; }

#define SESSION_HEADER "rr2-session 1"
enum { SESSION_LINE_MAX = 64 };

char *rr_console_session_text(const RrConsole *c) {
    if (c->events_lost) return NULL;
    char *text = malloc((c->event_count + 2) * SESSION_LINE_MAX);
    if (text == NULL) return NULL;
    char *at = text + sprintf(text, SESSION_HEADER "\n");
    for (size_t i = 0; i < c->event_count; i++) {
        const SessionEvent *e = &c->events[i];
        at += sprintf(at, "%llu %s", (unsigned long long)e->frame, event_names[e->kind]);
        if (e->kind == EV_PAD) at += sprintf(at, " %u %02X", (unsigned)e->port, (unsigned)e->value);
        else if (e->kind != EV_RESET) at += sprintf(at, " %u", (unsigned)e->value);
        *at++ = '\n';
    }
    sprintf(at, "end %llu\n", (unsigned long long)c->session_frames);
    return text;
}

static int replay_error(char *error, size_t size, const char *what, unsigned long line) {
    if (error != NULL && size > 0) {
        if (line != 0) snprintf(error, size, "line %lu: %s", line, what);
        else snprintf(error, size, "%s", what);
    }
    return 0;
}

/* Run frames until `frame` have run since opening. 0 if the game halts first. */
static int run_to(RrConsole *c, uint64_t frame, void (*each_frame)(void *ctx, RrConsole *c), void *ctx) {
    while (c->session_frames < frame) {
        if (!rr_console_run_frame(c)) return 0;
        if (each_frame != NULL) each_frame(ctx, c);
    }
    return 1;
}

static void apply_event(RrConsole *c, int kind, unsigned port, unsigned value) {
    switch (kind) {
    case EV_PAD: rr_console_set_buttons(c, (int)port, value); break;
    case EV_SPEED: rr_console_set_cpu_speed(c, value); break;
    case EV_RESET: rr_console_reset(c); break;
    case EV_SCALE: rr_console_set_scale(c, (int)value); break;
    case EV_CRISP: rr_console_set_crisp_road(c, (int)value); break;
    case EV_FINE_ROAD: rr_console_set_fine_road(c, (int)value); break;
    case EV_FINE_OBJECTS: rr_console_set_fine_objects(c, (int)value); break;
    case EV_BETWEEN: rr_console_set_between(c, (int)value); break;
    case EV_WIDE: rr_console_set_wide(c, (int)value); break;
    default: rr_console_set_soft_objects(c, (int)value); break;
    }
}

int rr_console_replay_session(RrConsole *c, const char *text, void (*each_frame)(void *ctx, RrConsole *c),
                              void *ctx, char *error, size_t error_size) {
    if (error != NULL && error_size > 0) error[0] = 0;
    if (c->session_frames != 0 || c->event_count != 0)
        return replay_error(error, error_size, "a session can only be replayed on a console just opened", 0);
    size_t header = strlen(SESSION_HEADER);
    if (strncmp(text, SESSION_HEADER, header) != 0 || text[header] != '\n')
        return replay_error(error, error_size, "not a session (it does not start with \"" SESSION_HEADER "\")", 0);
    const char *at = text + header + 1;
    int ended = 0;
    for (unsigned long line = 2; *at != 0; line++) {
        const char *stop = strchr(at, '\n');
        size_t length = stop != NULL ? (size_t)(stop - at) : strlen(at);
        char buffer[SESSION_LINE_MAX];
        if (length >= sizeof buffer) return replay_error(error, error_size, "too long", line);
        memcpy(buffer, at, length);
        buffer[length] = 0;
        at += length + (stop != NULL);
        if (buffer[0] == 0) continue;
        if (ended) return replay_error(error, error_size, "something follows the end line", line);
        unsigned long long frame;
        char what[24], extra;
        unsigned port = 0, value = 0;
        if (sscanf(buffer, "end %llu %c", &frame, &extra) == 1) {
            if (frame < c->session_frames) return replay_error(error, error_size, "ends before what came before it", line);
            if (!run_to(c, frame, each_frame, ctx)) return replay_error(error, error_size, "the game halted before this", line);
            ended = 1;
            continue;
        }
        int kind = -1, fields = sscanf(buffer, "%llu %23s", &frame, what);
        for (int k = 0; fields == 2 && k < EV_KINDS; k++) {
            if (strcmp(what, event_names[k]) == 0) kind = k;
        }
        int ok = kind >= 0;
        if (kind == EV_PAD) {
            ok = sscanf(buffer, "%*u %*s %u %x %c", &port, &value, &extra) == 2 && port < RR_CONSOLE_PORTS && value <= 0xFF;
        } else if (kind == EV_RESET) {
            ok = sscanf(buffer, "%*u %*s %c", &extra) != 1;
        } else if (kind >= 0) {
            ok = sscanf(buffer, "%*u %*s %u %c", &value, &extra) == 1;
        }
        if (!ok) return replay_error(error, error_size, "not \"<frame> <what> <value>\"", line);
        if (frame < c->session_frames) return replay_error(error, error_size, "is earlier than the line before it", line);
        if (!run_to(c, frame, each_frame, ctx)) return replay_error(error, error_size, "the game halted before this", line);
        apply_event(c, kind, port, value);
    }
    if (!ended) return replay_error(error, error_size, "no end line", 0);
    return 1;
}
