/* The console as a host sees it: load a ROM, feed input, run a frame, read the
 * picture. A runtime fatal error (untranslated address, unmodelled hardware,
 * unimplemented instruction) halts the console instead of ending the process.
 *
 * Only one console may be inside rr_console_run_frame at a time, because the
 * fatal handler it installs is process-wide.
 */
#ifndef RR_HOST_CONSOLE_H
#define RR_HOST_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

typedef struct RrConsole RrConsole;

enum { RR_CONSOLE_W = 320, RR_CONSOLE_H = 224, RR_CONSOLE_PORTS = 2, RR_CONSOLE_SCALE_MAX = 8 };

/* Open the ROM the linked code was translated from. Returns NULL and writes a
 * message to `error` (always terminated) if the file cannot be read or is a
 * different ROM. */
RrConsole *rr_console_open(const char *rom_path, char *error, size_t error_size);
void rr_console_close(RrConsole *c);

/* 68000 reset with cleared memory. Clears a halt. Held buttons are kept. */
void rr_console_reset(RrConsole *c);

/* Buttons held on a controller port (0 or 1), as RR_PAD_* bits from
 * rr_genesis.h. Left is dropped when right is also held, and up when down is,
 * as a real d-pad cannot press both. Other ports are ignored. */
void rr_console_set_buttons(RrConsole *c, int port, unsigned held);
unsigned rr_console_buttons(const RrConsole *c, int port);

/* CPU speed in percent of the original (100 to 6400, clamped; see
 * rr_genesis_set_cpu_speed). Kept across a reset. */
void rr_console_set_cpu_speed(RrConsole *c, unsigned percent);
unsigned rr_console_cpu_speed(const RrConsole *c);

/* Run one frame. Returns 1 if it ran, 0 if the console is halted. */
int rr_console_run_frame(RrConsole *c);

const uint32_t *rr_console_frame(const RrConsole *c); /* 320 x 224 pixels, 0x00RRGGBB */
const uint8_t *rr_console_ram(const RrConsole *c);    /* 65,536 bytes of work RAM */
uint64_t rr_console_frames(const RrConsole *c);       /* frames run since the last reset */

/* An enlarged picture. Scale 1 to RR_CONSOLE_SCALE_MAX (clamped; 1, the
 * default, is the plain picture). Kept across a reset. At a larger scale
 * rr_console_scaled_frame is 320 * scale by 224 * scale pixels, each original
 * pixel repeated, and takes effect from the next frame run. */
void rr_console_set_scale(RrConsole *c, int scale);
int rr_console_scale(const RrConsole *c);
const uint32_t *rr_console_scaled_frame(const RrConsole *c);

/* The wide view: the enlarged picture 39 game pixels wider on either side
 * (398 across in all) on the lines of a race's view; everything else (the
 * dashboard and mirrors, a menu, a two-player race) is the original picture
 * between black margins. Off as opened; the setting is kept across a reset.
 * It is in effect only while the crisp road, the fine road and the fine
 * objects are all on and the scale is above 1: rr_console_scaled_frame is
 * then rr_console_scaled_width pixels across on every frame, and as wide as
 * ever otherwise. The middle of a wide frame is the frame as it would be
 * without it, but for the last five columns of the screen where an object's
 * picture the game cut short there is carried on. */
void rr_console_set_wide(RrConsole *c, int on);
int rr_console_wide(const RrConsole *c);
/* Pixels across of rr_console_scaled_frame (its height is RR_CONSOLE_H * scale). */
int rr_console_scaled_width(const RrConsole *c);
/* Frames drawn wide (race frames with the wide view in effect). */
uint64_t rr_console_wide_frames(const RrConsole *c);

/* The crisp road (off by default, kept across a reset; needs a scale above
 * 1). During a one-player race the road's edges in the enlarged picture are
 * drawn as slopes at the enlarged picture's own resolution instead of the
 * original's staircase, from the game's own road points: the two grass planes
 * are shifted sideways by a fraction of a pixel on each row. Everything else
 * is unchanged, and the original picture is still there exactly: it is the
 * last of each original pixel's `scale` pixels on the first of its `scale`
 * rows. A frame whose road points cannot be matched to the road
 * on screen is drawn without it. rr_console_crisp_frames counts the frames
 * drawn with it since the last reset. */
void rr_console_set_crisp_road(RrConsole *c, int on);
int rr_console_crisp_road(const RrConsole *c);
uint64_t rr_console_crisp_frames(const RrConsole *c);
/* Fine road points (on by default, kept across a reset): the crisp road is
 * drawn from road points placed with fractions of a pixel, which the port
 * works out by repeating the game's own projection and keeping the remainders
 * the game drops. It is used only for a road whose points the port's
 * projection reproduces exactly; any other is drawn from the game's
 * whole-pixel points. With it on, the lines that show the road no longer hold
 * the original picture pixel for pixel (the edge is where the fraction puts
 * it); all other lines still do. rr_console_fine_frames counts the frames
 * drawn from fine points since the last reset (they are among the crisp
 * frames). */
void rr_console_set_fine_road(RrConsole *c, int on);
int rr_console_fine_road(const RrConsole *c);
uint64_t rr_console_fine_frames(const RrConsole *c);
/* Note what the game is about to project the road from. The console does this
 * itself whenever the game reaches its projection; it is public for tests. */
void rr_console_note_road_inputs(RrConsole *c);

/* Fine objects (on by default, kept across a reset; they act while the crisp
 * road is on and the scale is above 1): riders, cars and roadside objects are
 * drawn from their full pictures in the ROM at the enlarged picture's own
 * resolution, instead of from the sprites the game scaled to 320 x 224. Each
 * object keeps the game's place, size, order and colours. It is used only on
 * a frame whose object sprites and their tiles in video memory are byte for
 * byte what the port's repeat of the game's drawing says (port/render/
 * rr_objects.h); any other frame shows the game's sprites enlarged. Nothing
 * outside the object sprites' rectangles changes. rr_console_fine_object_frames
 * counts the frames drawn this way since the last reset. */
void rr_console_set_fine_objects(RrConsole *c, int on);
int rr_console_fine_objects(const RrConsole *c);
uint64_t rr_console_fine_object_frames(const RrConsole *c);
/* Note the objects the game is about to draw. The console does this itself
 * whenever the game reaches its object drawing; it is public for tests. */
void rr_console_note_objects(RrConsole *c);
/* Note the game's own description of the centre-line dashes, as it is about
 * to pack them into sprites (the console does this itself too). Where the
 * port's description of them from the road is proven equal, and the fine
 * road and the fine objects are being drawn, the dashes are drawn as strips
 * of the enlarged picture from the fine road, and move with it in the
 * in-between pictures; otherwise they are the game's sprites
 * (port/render/rr_dashes.h). */
void rr_console_note_dashes(RrConsole *c);

/* Note the road points now in work RAM as a candidate for the road on screen.
 * The console does this itself whenever the game reaches the routine that
 * turns road points into the picture; it is public for tests. */
void rr_console_note_road(RrConsole *c);

/* NULL while running; the fatal error message once halted. */
const char *rr_console_halt_message(const RrConsole *c);

/* In-between pictures: with the crisp road, the fine road points and the
 * fine objects all on, each frame is drawn from the race scene part-way
 * between two of the game's updates (or a little past the newest) instead of
 * from the update on screen, so the picture moves on every frame
 * (port/render/rr_between.h). The mode is 0 off (the default), 1 predict
 * (nothing late; guesses ahead), 2 blend (about one update late) or 3 late
 * blend (later still; never guesses in a steady race); other values are
 * ignored. A frame that cannot be made is drawn as it would be with the mode
 * off. The game itself runs exactly the same in every mode. A change of mode,
 * of the crisp road or of the fine objects, and a reset, forget the updates
 * kept: nothing is drawn in between until two new ones have shown.
 * rr_console_between_frames counts the frames drawn in between since power
 * on. */
void rr_console_set_between(RrConsole *c, int mode);

/* Softer far objects: with the fine objects showing, a picture drawn below
 * its natural size takes for each output pixel the mean colour of the
 * picture pixels it covers, instead of one of them, so small objects are
 * smoother and flicker less as they change size. The mean is not one of the
 * game's colours. Objects at natural size or larger are unchanged. Off by
 * default; the choice is kept across a reset. */
void rr_console_set_soft_objects(RrConsole *c, int on);
int rr_console_soft_objects(const RrConsole *c);
int rr_console_between(const RrConsole *c);

/* Sound: the game's own sound program playing a model of the FM chip
 * (port/hw/rr_sound.h, analysis/sound.md). Off as opened; it survives a
 * reset. It only listens: the game runs the same with it on. Turned on in
 * the middle of a run it is silent until the game next starts its sound
 * program. */
void rr_console_set_sound(RrConsole *c, int on);
int rr_console_sound(const RrConsole *c);
/* The sound made since the last call, at `out_hz` samples a second, as
 * left/right pairs of floats from -1 to 1. Returns the pairs written: 0
 * when sound is off, when there is nothing new, or for a rate that makes no
 * sense. Sound never taken is kept for half a second, then dropped, so a
 * caller that stopped asking does not get old sound later. */
size_t rr_console_take_sound(RrConsole *c, double out_hz, float *out, size_t max_frames);
uint64_t rr_console_between_frames(const RrConsole *c);

/* The session: what was done to the console since it was opened, and at
 * which frame, so that another console can be brought to exactly the same
 * moment (the console is deterministic). It lists the buttons each time they
 * change, the CPU speed, resets, and the picture settings (scale, crisp
 * road, fine road points, fine objects, in-between mode, soft objects); not
 * the sound, which changes neither the game nor the picture. The frames are
 * counted from opening, and are not set back by a reset.
 *
 * The text is "rr2-session 1", then one line per thing done,
 * "<frame> <what> <value>" (pad <port> <hex buttons>, speed, reset, scale,
 * crisp, fine-road, fine-objects, between, soft-objects), done before that
 * frame ran, then "end <frames run>". */
uint64_t rr_console_session_frames(const RrConsole *c);
/* The session as text, for the caller to free(). NULL if there is no memory
 * for it, or there was none to keep the list at some point. */
char *rr_console_session_text(const RrConsole *c);
/* Do a session again on a console just opened: everything listed, each at
 * its frame, and the frames up to the end line. `each_frame`, if not NULL, is
 * called after every frame run. Returns 1 when the end is reached; 0, with a
 * message in `error` (always terminated), if the text is not a session, the
 * console has already been used, or the game halts where a frame was still
 * to run. */
int rr_console_replay_session(RrConsole *c, const char *text, void (*each_frame)(void *ctx, RrConsole *c),
                              void *ctx, char *error, size_t error_size);

#endif
