/* rr-race-replay: play a recorded race natively and compare it with the
 * original at every game-loop handoff.
 *
 * Usage: rr-race-replay --rom ROM --handoffs FILE --timesteps FILE --pads FILE
 *                       [--force-timestep] [--exclude START:END:REASON]...
 *                       [--max-frames N] [--report FILE] [--ram-at K:FILE]
 *                       [--regs FILE --resync] [--compare-regs]
 *                       [--exclude-stack-below FLOOR] [--offsets FILE]
 *                       [--profile A:B:FILE] [--follow K:TRACE] [--cpu-speed N]
 *
 *                       [--ram-at-pc PC:A:B:FILE]...
 *                       [--scaled-check S] [--crisp-points whole|fine|none] [--edge-log FILE [--edge-log-over N]]
 *                       [--scaled-at F:FILE] [--objects-check] [--fine-objects]
 *                       [--picture-at F:FILE] [--sources-at F:FILE]
 *                       [--line-state-at F:FILE] [--video-state-at F:FILE]
 *
 * The four -at options write what the video chip showed in frame F (frames
 * count from 0 at power-on; a handoff the report lists in frame F happens
 * during it). Each may be given several times. --picture-at: the picture, as
 * a binary PPM. --sources-at: which layer drew each pixel, 224 x 320 records
 * of 4 bytes (layer 0 backdrop, 1 plane B, 2 plane A, 3 window, 4 sprite;
 * colour index; two bytes of detail, big-endian: the sprite's index in the
 * sprite table, or the plane's name-table entry). --line-state-at: one text
 * line per display line with the state it was drawn from: line number, the
 * 24 registers, then plane A's and B's horizontal scroll and their vertical
 * scroll (all hex). --video-state-at: the registers and memories as they are
 * at the end of the frame, in the layout rr-vdp-render reads. A frame the run
 * never reaches is an input error. They cannot be combined with --resync, which
 * cuts frames short.
 *
 * --video-log A:B:FILE writes one line for every write the game makes to the
 * video chip from handoff A up to handoff B:
 * "<handoff> <frame> <line> <pc> <level> <kind> <target> <value> <source>".
 * <pc> is the instruction making the write, <level> the CPU's interrupt level
 * (4 inside the line interrupt handler, 6 inside the frame interrupt handler,
 * lower in the main loop). <kind> is reg, vram, cram, vsram (one value to a
 * register or to video, colour or vertical-scroll memory), dma-vram, dma-cram,
 * dma-vsram (a block copied from 68000 memory at <source>; <value> is its
 * length in words), fill (<value> bytes of <source>) or copy (<value> bytes
 * from video memory at <source>). <source> is "-" where there is none.
 *
 * --ram-writes A:B:START:END:FILE writes one line for every write to work
 * RAM offsets START to END (hex, END included) from handoff A up to handoff
 * B: "<handoff> <frame> <pc> <address> <size> <value>", <pc> being the
 * instruction that wrote. A long write shows as two word writes.
 *
 * --ram-at-pc PC:A:B:FILE (PC hex) appends an image of work RAM to FILE each
 * time the instruction at PC is about to run, from handoff A up to handoff B.
 * FILE has the layout of the handoffs file (images numbered from 0), so the
 * same readers take it; FILE.index lists, for each image, "<image> <pass>
 * <frame>", the pass being the handoff that began it. Unlike a handoff image,
 * which the frame interrupt takes wherever the main loop happens to be, such
 * an image shows the main loop's work finished up to that instruction. May be
 * given several times; not with --resync.
 *
 * --scaled-check S (2 to 8) also draws every frame enlarged S times with the
 * crisp road (port/render/rr_road.h): during a one-player race the road edges
 * are drawn from the game's road points at the enlarged resolution.
 * --crisp-points fine (the default) places those points with fractions of a
 * pixel, by repeating the game's projection and keeping the remainders it
 * drops; --crisp-points whole uses the game's whole-pixel points. It prints
 * "SCALED scale=S points=fine|whole frames=N off_road_identical=O
 * sampled_back_identical=M crisp_frames=K fine_frames=F race_frames=R
 * edge_lines=L within_1px=W within_2px_and_a_line=B over_2px=.. over_4px=..
 * over_8px=.. max_edge_difference=D
 * run_ms_per_frame=T":
 *   O  frames in which every line without a road shift is the original line,
 *      each pixel repeated;
 *   M  frames whose enlarged picture still holds the original exactly (the
 *      last of each original pixel's S pixels, on the first of its S rows):
 *      all of them with whole points, not with fine ones, whose edges are
 *      deliberately not on the game's whole pixels;
 *   R  frames that began as a one-player race; K those drawn with the crisp
 *      road (the rest fell back to the plain enlargement because no noted
 *      road matched the one in video memory); F those of K drawn from fine
 *      points (the rest from whole points, because no noted inputs projected
 *      to exactly that road's points);
 *   L  (line, plane) pairs drawn with a fine shift; W those whose fine shift
 *      on row 0 is less than a pixel from the game's whole-pixel one; B those
 *      less than two pixels plus the edge's sideways movement over one line
 *      away (the game drops the remainder of a point's line as well as of its
 *      edges); over_Npx those N pixels or more away; D the largest
 *      difference, in 1/65536 pixel;
 *   T  the processor time of the whole run per frame.
 * --edge-log FILE lists the (line, plane) pairs whose fine shift on row 0 is
 * far from the game's whole-pixel one: with --edge-log-over N, those N pixels
 * or more away; without it, those counted in L but not in B. Each line is
 * "<frame> <line> <plane> <whole> <fine> <exact> <jump> <beyond> <above>
 * <below>", shifts in pixels: the game's whole-pixel shift, the fine one, the
 * shift on a straight run between the game's whole-pixel points worked
 * without the game's table of reciprocals, the largest change of the
 * whole-pixel shift between this line and its two neighbours (a hill crest
 * shows as a jump), how many lines past the nearer point of its run the line
 * is (the game draws the bottom of the road by extending the run between its
 * second and third points, which magnifies those points' dropped
 * remainders), and the whole-pixel shifts of the line above and the line
 * below ("-" where that line has no road).
 * --scaled-at F:FILE writes frame F's enlarged picture as a PPM. None of these
 * can be used with --resync.
 *
 * --dashes-check compares the port's per-line description of the centre-line
 * dashes (port/render/rr_dashes.h) with the game's own, each time the game is
 * about to pack them into sprites. Prints "DASHES passes=N covered=C
 * identical=M first_difference=K|none sprites_after_objects=A dash_sprites=D":
 * N times the game reached its dash routine, C of them covered by the model
 * (not near the finish line), M of those where every line's start and width
 * are equal, the first pass that differed, the sprites the game added after
 * its objects in all, how many of those are dashes by their attribute
 * (colour set 3, low priority), and which colour numbers their tiles hold
 * (0, see-through, aside).
 *
 * With --fine-objects the centre-line dashes are drawn as strips from the
 * fine road wherever the port's description of them is proven for the road
 * on screen (port/render/rr_dashes.h), and with --inbetween they move with
 * the in-between road. Prints "FINE_DASHES frames=N strip_frames=M
 * sprite_frames=K": frames drawn with the crisp road, those whose dashes
 * were strips, and those that kept the game's dash sprites.
 *
 * --soft-objects (with --fine-objects) softens the objects drawn below their
 * natural size (RrFineSprites.soft in port/hw/rr_vdp.h).
 *
 * --sound-wav FILE and --fm-log FILE attach the sound side (port/hw/rr_sound.h):
 * the game's Z80 sound program runs beside the game and plays the FM chip.
 * The first writes the whole run's sound as a WAV (16-bit stereo, 53,267
 * samples a second). The second writes one line for every write to the FM
 * chip: "<master clocks since power-on> <port 0-3> <value, two hex digits>
 * <z or m: written by the Z80 or by the 68000>", and a last line "END <master
 * clocks>" for when the run ended. Either prints a line "SOUND z80_steps=N
 * fm_writes=N fm_writes_68000=N psg_writes_z80=N psg_writes_68000=N
 * z80_ram_writes_68000=N samples=N" before the result. Neither can be used with --resync. Without them there is
 * no sound side and the run is as before.
 *
 * --ground-check (with --scaled-check S) looks at what the two scrolling
 * planes hold on every line of the view (the first 152) of every frame that
 * has a road chosen, just before the line is drawn, for the wide picture
 * (render/rr_wide.h). It prints "GROUND ground_lines=N block_rows=B
 * full_rows=F empty_rows=E other_ground_rows=O sky_lines=S
 * sky_rows_passing=P mark_max=W period_max=Q ground_lines_sky_backdrop=G
 * sky_rows_passing_road_backdrop=R". Ground lines are the ones the road reaches (the tracker's
 * whole-pixel shifts); each has two rows, one a plane. block_rows are the
 * ground block with its marks (rr_wide_is_ground_row), full_rows opaque
 * right across the plane, empty_rows blank, other_ground_rows anything else
 * (--ground-log FILE lists every 499th of them, and every 499th of the sky
 * rows that pass, or every Nth with --ground-log-every N, as "<frame> <line> <A|B> ground|sky reg7=XX road_top=L runs=N
 * <first>..<end> ...", reg7 being the backdrop register as the line is
 * drawn and road_top the first line the road reaches). sky_rows_passing counts rows of the other lines that pass the
 * ground row test. mark_max is the furthest the opaque pixels of a
 * ground row with the whole block reach beyond the block's ends; period_max
 * the largest sideways step such a block repeats with (999: none up to 64).
 * The last two say how well the backdrop register tells ground from sky as
 * a line is drawn: ground lines drawn with another backdrop than the
 * road's grey, and passing sky rows drawn with the road's grey.
 *
 * --wide M (1 to 64; with --scaled-check S and --fine-objects) draws the
 * enlarged picture M game pixels wider on either side on the lines of a
 * race's view (rr_vdp_render_line_wide; anything else lies between black
 * panels), and the same frame without margins beside it, on which every
 * other check is made as before. --scaled-at then writes the wide picture.
 * It prints three lines (the checks are render/rr_wide.h's):
 *   "WIDE_CENTRE frames=N identical=K pixels=T differing_pixels=P
 *    road_line=A object=B dash=C other=D first_frame=F|none": the middle of
 *    the wide picture against the one without margins;
 *   "WIDE_GROUND examined=E grass=G mark=M road=R wrong=W short_lines=S
 *    short_updates=U beyond_lines=B no_edge_lines=X": the margin pixels of
 *    the ground lines drawn with the road's own shifts, against what the
 *    road says belongs there; S the ground lines on which the game's block
 *    of ground ends inside a margin (U the frames with any), B those with an
 *    edge beyond one of the game's limits, X those drawn without the road's
 *    shifts while held at the screen's edge (grass runs on there);
 *   "WIDE_SKY examined=E plane=P backdrop=B panel=Q
 *    untracked_race_frames=F": the margin pixels of the view's other lines
 *    by kind; F the race frames for which no road was chosen (all their
 *    lines are counted here).
 *   "WIDE_OBJECTS frames=N margin_pictures=P uncut=U off_view=V copies=C
 *    left_out_room=R seam_pixels=S differing_outside_seam=D gaps=G": the objects drawn in the margins
 *    (rr_objects_margin) on the N frames that had an object set on screen:
 *    pictures in all, those that are a picture of the screen's cut at its
 *    side put back whole, those the screen does not show at all, copies the
 *    game did not make, records left out for want of sprites; and S, the
 *    pixels of the screen's last five columns filled from them. They are
 *    the only difference WIDE_CENTRE may show (as object; fewer than S,
 *    because a filled pixel can have the colour that was there): D counts
 *    the differing pixels that are not among them, and must be 0. gaps=G:
 *    columns of a margin picture, beyond the screen's sides or in its last
 *    five, that no sprite entry shows (rr_objects_margin_gaps, of the sets
 *    drawn, in-between scenes' included); must be 0.
 *
 * --inbetween predict|blend|late (with --scaled-check S and --fine-objects, and
 * the fine road points) draws every frame it can from the scene part-way
 * between two of the game's updates (port/render/rr_between.h) instead of
 * from the update on screen. Prints "INBETWEEN mode=M frames=N between=B
 * guessed=G no_pair=.. road=.. objects=.. table=.. returned=R stale_lines=L":
 * N frames asked for, B drawn in between, G of those past the newest update,
 * the frames that fell back by reason, R frames on which an earlier update
 * was on screen again, and the lines drawn from the game's own sprites
 * because video memory changed during the frame (a line whose sideways
 * shift changed is drawn plainly too, but is not counted). --inbetween-check adds four
 * measurements, all in screen pixels of 320:
 *   INBETWEEN_ENDS: for each pair of neighbouring updates, whether the scene
 *     at the older (t = 0) and at the newer (t = 1) is the scene the tracker
 *     gives for that update: every row of every road line, and every object
 *     sprite entry (the link aside) and picture; and whether the sprite table
 *     at the newer one is video memory's own chain.
 *   INBETWEEN_SKIP_ONE: for updates k-1, k, k+1, the scene made from k-1 and
 *     k+1 at the moment of k against the real k: the left road edge on lines
 *     100, 115, 130 and 145, and the x of every object followed through all
 *     three.
 *   INBETWEEN_MOTION: while the camera is moving, how many frames left the
 *     left edge on line 130 (and the largest followed roadside object) where
 *     the frame before had it, to within 1/16 pixel; "today" is the same
 *     count for the picture without in-between frames.
 *   INBETWEEN_GUESS: when an update arrives, how far the last frame drawn
 *     past the newest update was from it (same samples as skip-one).
 *
 * --fine-objects (with --scaled-check) also draws the objects of the enlarged
 * picture from their pictures in the ROM instead of from the game's scaled
 * sprites, on every frame where the object set in video memory is one the
 * repeat of the game's drawing reproduced (port/render/rr_objects.h). The two
 * SCALED checks then leave out the screen pixels inside those sprites, so
 * they still say that nothing else changed. --crisp-points none draws no
 * road shifts, so that the objects are the only difference from the plain
 * enlargement. Prints "FINE_OBJECTS frames=N set_frames=K fine_frames=F
 * stale_lines=L entries=E fine_entries=G covered_pixels=P same_at_centre=Q":
 * K frames that began with an object set noted, F of them drawn with one, L
 * lines that fell back because video memory changed during the frame, E
 * object sprites on those F frames and G of them drawn from a picture, P
 * screen pixels inside those G sprites and Q of them whose centre output
 * pixel has the original pixel's colour (a description, not a pass mark: the
 * game scales sideways in pixel pairs and from a different starting phase).
 *
 * --objects-check repeats the game's drawing of the objects (riders, cars,
 * roadside things) outside the game each time the game is about to do it
 * (port/render/rr_objects.h) and compares the sprite list, the sprite-picture
 * buffer and the two counters the game leaves with what the repeat says. It
 * prints "OBJECTS passes=N identical=M records=R dropped=D pictures=P
 * first_difference=K": N times the game drew, M of them identical, R draw
 * records in them, D of those the game dropped, P pictures described, and the
 * pass of the first drawing that differed ("none" if none did). Not with
 * --resync.
 *
 * --track ADDR:SIZE:NAME (ADDR a hex work-RAM address, SIZE 1, 2 or 4 bytes)
 * adds " NAME=<hex value>" to every report line: the value at that handoff.
 *
 * --pad-log FILE writes "<frame> <port> <hex value>" for every controller
 * read of the native run. --inputs-by-frame FILE takes such a file instead of
 * following the reference pass by pass: each read gets the value logged for
 * its port at the latest frame at or before the current one. In that mode the
 * run is not compared with the reference and does not stop at its last
 * handoff; it runs for --max-frames frames, and prints
 * "FREE_RUN handoffs=N frames=M" instead of the comparison lines. It is for
 * runs that do not line up with the reference, such as other CPU speeds.
 *
 * --cpu-speed N runs the 68000 at N percent of the original speed (100 to
 * 6400; see rr_genesis_set_cpu_speed) and prints "CPU_SPEED percent=N".
 * With --speed-from-handoff K the speed changes at the first frame boundary
 * after handoff K instead of at power-on, so everything before (the menus,
 * which also depend on CPU speed) runs as the original did.
 *
 * --follow K:TRACE compares every instruction from handoff K on with a trace
 * recorded from MAME (scripts/run-mame-port-trace-rr206.sh): program counter,
 * D0-D7, A0-A7 and the cycle count. It stops at the first difference and
 * prints it with the instructions leading up to it (the result is then
 * "RESULT differ", exit status 1), or reports that the whole trace matched. K is "boot" for a trace that starts at power-on.
 *
 * --resync restarts every pass from the original: after handoff k has been
 * compared, work RAM, the CPU registers and the cycle count are replaced by
 * the original's at handoff k (the regs file has one line per handoff: MAME's
 * cycle count, SR, D0-D7, A0-A7). A difference then shows what one pass got
 * wrong and cannot grow, so the count of identical handoffs is a count of
 * passes the port computed exactly. The video chip's own state is not
 * replaced.
 *
 * --compare-regs (with --regs) also compares the cycle count, SR, D0-D7 and A0-A7 with the
 * original's at each handoff and reports how many handoffs had a difference;
 * the report file gains a column with the number of differing registers.
 *
 * --exclude-stack-below FLOOR (hex RAM offset) leaves out the dead part of
 * the stack: bytes from FLOOR up to, but not including, the stack pointer at
 * the handoff. They hold leftovers from interrupts and calls that have
 * returned; where an interrupt landed decides what is left there. Bytes at
 * and above the stack pointer are live and are compared.
 *
 * --offsets FILE writes "<offset> <handoffs at which it differed>" for every
 * RAM offset that differed anywhere, to show what kind of state is involved.
 *
 * --ram-at K:FILE writes the native work RAM as it is at handoff K, for
 * looking into a difference. --profile A:B:FILE writes, for every instruction
 * address executed from handoff A up to handoff B, "<pc> <times> <cycles>".
 *
 * The game runs from reset on the port's own hardware model. All controller
 * access in RR206 goes through two routines (0x10F4 for port 1, 0x1156 for
 * port 2). The pads file lists what each call returned in the original, in
 * order ("<port> <hex value>" per line), with a line "H" at each handoff. As
 * the native game enters one of the routines, the controller is set to the
 * next value the original read on that port in the same pass (the stretch
 * between two handoffs; everything before the first handoff is one stretch).
 * The game reads its controllers once a frame, so a native pass that spans a
 * different number of frames makes a different number of reads: extra reads
 * repeat the port's last value, and unused recorded reads are dropped at the
 * next handoff, so the difference never shifts later input.
 *
 * Each time the game reaches the handoff (0xDF48, where the timestep in D1 is about to be
 * stored), handoff k counted from 0:
 *   - the native timestep is compared with the recorded one, and with
 *     --force-timestep replaced by it;
 *   - all of work RAM is compared with the reference's handoff k, outside the
 *     excluded ranges (hex offsets into RAM, inclusive).
 * The reference file format is described in src/rr2port/handoffs.py.
 *
 * Output: HANDOFFS, TIMESTEP, PADS, RAM and EXCLUDED lines, then
 *   RESULT match        every handoff identical and every timestep the same
 *   RESULT logic-match  identical, with some timesteps forced
 *   RESULT differ
 * Exit status: 0 match or logic-match; 1 differ, or the native run and the
 * reference are out of step; 2 usage or input error, or a fatal runtime error.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rr_genesis.h"
#include "rr_mem.h"
#include "rr_between.h"
#include "rr_dashes.h"
#include "rr_wide.h"
#include "rr_objects.h"
#include "rr_road.h"
#include "rr_run.h"
#include "rr_sound.h"

#define RR206_CRC32 0x0876E992u
#ifndef HANDOFF_PC
#define HANDOFF_PC 0x00DF48u /* overridable for tests */
#endif
#define RAM_SIZE 0x10000
#ifndef PAD1_READ_PC
#define PAD1_READ_PC 0x0010F4u /* entry of the port 1 pad read routine */
#endif
#ifndef PAD2_READ_PC
#define PAD2_READ_PC 0x001156u /* entry of the port 2 pad read routine */
#endif
#ifndef ROAD_NOTE_PC
#define ROAD_NOTE_PC RR_ROAD_NOTE_PC /* overridable for tests */
#endif
#ifndef OBJECTS_NOTE_PC
#define OBJECTS_NOTE_PC RR_OBJECTS_NOTE_PC
#endif
#ifndef DASHES_PC
#define DASHES_PC RR_DASHES_PC
#endif
#ifndef DASHES_DONE_PC
#define DASHES_DONE_PC 0x13F70u /* the dash routine's return */
#endif
#ifndef OBJECTS_DONE_PC
#define OBJECTS_DONE_PC RR_OBJECTS_DONE_PC
#endif
#ifndef ROAD_INPUTS_PC
#define ROAD_INPUTS_PC RR_ROAD_INPUTS_PC
#endif
#define MAX_EXCLUDES 32
#define FIRST_DIFFS 10

typedef struct {
    unsigned start, end;
    const char *reason;
    unsigned long long hidden;
} Exclude;

#define MAX_PC_DUMPS 8
typedef struct {
    uint32_t pc;
    long from, to;     /* passes: from handoff `from` up to handoff `to` */
    FILE *f, *index;
    uint8_t *previous; /* the last image written */
    unsigned long count;
} PcDump;

#define MAX_TRACKS 8
#define MAX_DUMPS 256
enum { DUMP_PICTURE, DUMP_SOURCES, DUMP_LINE_STATE, DUMP_VIDEO_STATE, DUMP_SCALED };
typedef struct {
    int kind;
    unsigned long frame;
    const char *path;
    int done;
} FrameDump;

typedef struct {
    RrMachine *m;
    RrGenesis *g;
    FILE *handoffs, *report;
    uint8_t current[RAM_SIZE]; /* reference RAM at handoff k */
    uint8_t *pad_port, *pad_value;     /* recorded pad reads, in order */
    unsigned long *segment_start;      /* index of the first read of each stretch */
    unsigned long pad_count, segment_count;
    unsigned long segment;             /* stretch the native run is in */
    unsigned long pad_next[2];         /* next unread index for each port in this stretch */
    uint8_t pad_last[2];               /* last value each port was given */
    unsigned long pad_reads, pad_repeated, pad_dropped;
    uint16_t *timesteps;
    uint64_t *reg_cycles;      /* per handoff, from the regs file */
    uint32_t (*reg_values)[17]; /* sr, d0-d7, a0-a7 */
    int resync;
    int compare_regs;
    int resumed_at_handoff;    /* the hook will be called once more for the same instruction */
    unsigned long regs_identical, regs_differing;
    uint32_t *offset_tally;    /* per RAM offset: handoffs at which it differed */
    long stack_floor;          /* dead stack starts here, or -1 */
    unsigned long long stack_hidden;
    unsigned long total;       /* handoffs in the reference */
    unsigned long k;           /* next handoff to compare */
    unsigned long frames;
    unsigned long timestep_same, timestep_different;
    unsigned long ram_identical, ram_differing;
    long first_differing;
    unsigned first_offsets[FIRST_DIFFS];
    int first_count;
    int force_timestep;
    int finished;
    long profile_from, profile_to; /* handoffs bounding the profile, or -1 */
    const char *profile_path;
    uint32_t *profile_count;       /* per instruction address / 2 */
    uint32_t *profile_cycles;
    uint64_t last_cycles;
    uint32_t last_pc;
    int profiling;
    long follow_at;            /* handoff at which a recorded trace starts, or -1 */
    FILE *follow;              /* that trace; NULL once done or unused */
    int follow_boot;           /* the trace starts at power-on, not at a handoff */
    int follow_differed;       /* the run was stopped at a difference from the trace */
    struct { uint32_t offset; int size; const char *name; } tracks[MAX_TRACKS];
    int track_count;
    FILE *pad_log;
    int by_frame;              /* --inputs-by-frame: free run, inputs by frame number */
    long speed_from;           /* handoff after which the CPU speed changes, or -1 */
    int speed_due;             /* that handoff has been reached */
    unsigned long *frame_pad_frame[2]; /* per port: frames with a logged value, ascending */
    uint8_t *frame_pad_value[2];
    unsigned long frame_pad_count[2], frame_pad_at[2];
    unsigned long long follow_line;
    char follow_recent[8][256];
    long ram_writes_from, ram_writes_to; /* handoffs bounding the RAM write log, or -1 */
    FILE *ram_writes;
    long video_log_from, video_log_to; /* handoffs bounding the video log, or -1 */
    FILE *video_log;
    uint32_t current_pc;       /* the instruction being executed */
    FrameDump dumps[MAX_DUMPS];
    int dump_count;
    int scale;                 /* --scaled-check: the enlargement, or 0 */
    uint32_t *scaled;          /* the enlarged picture */
    RrRoadTracker road;
    unsigned long scaled_frames, sampled_back, off_road;
    int wide_margin;           /* --wide M: the margin, or 0 */
    RrWide wide;
    uint32_t *wide_px;         /* the wide picture; `scaled` is then the same frame without margins */
    RrPixelSource *wide_src;
    uint8_t *wide_cover;       /* where the narrow picture shows an object */
    RrObjectSet *margin_set;   /* --wide: the objects of the margins, for this frame */
    uint8_t *margin_table, *margin_work;
    RrFineSprites margin_fine;
    unsigned long margin_frames;
    unsigned long long margin_pictures, margin_uncut, margin_off_view, margin_copies, margin_left_out, seam_pixels, outside_seam, margin_gaps;
    int wide_tracked;          /* this frame: a road was chosen and its shifts are the tracker's */
    int wide_between;          /* this frame is an in-between scene */
    int wide_short_frame;
    unsigned long wide_frames, wide_identical, wide_short_updates, wide_untracked;
    long wide_first;           /* the first frame whose middle differs, or -1 */
    unsigned long long wide_no_edge;
    RrWideCentre wide_centre;
    RrWideGroundCount wide_ground;
    RrWideSky wide_sky;
    int ground_check;          /* --ground-check */
    int ground_frame;          /* this frame has a road chosen: its lines are counted */
    unsigned long long ground_lines, ground_block, ground_full, ground_empty, ground_other;
    unsigned long long sky_lines, sky_passing, sky_passing_road, ground_sky_backdrop;
    int ground_mark_max, ground_period_max;
    FILE *ground_log;          /* --ground-log FILE: a sample of the rows that are not as expected */
    unsigned long long ground_every; /* --ground-log-every N: one row in N */
    unsigned long long edge_lines, edge_within_1px, edge_within_bound, edge_over[3]; /* 2, 4, 8 pixels */
    long edge_max;
    int points_given;          /* --crisp-points was given */
    int points_none;           /* --crisp-points none: no road shifts at all */
    int fine_objects;          /* --fine-objects */
    RrObjectTracker *objects;
    const RrFineSprites *pending_fine; /* the object set's sprites chosen for this frame, or NULL */
    RrStrips *strips;          /* --fine-objects: the dashes of a frame drawn from the update on screen */
    RrFineSprites frame_fine;
    int frame_strips;          /* this frame's dashes are strips */
    int soft_objects;          /* --soft-objects */
    FILE *sound_wav;           /* --sound-wav: the run's sound, 16-bit stereo at RR_SOUND_HZ */
    FILE *fm_log;              /* --fm-log: every write to the FM chip */
    unsigned long sound_samples;
    unsigned long dash_frames, dash_strip_frames; /* frames with the fine road drawn; of those, dashes as strips */
    RrBetween *between;        /* --inbetween */
    int between_check;         /* --inbetween-check */
    struct BetweenCheck *check;
    uint8_t *covered;          /* per screen pixel: inside a sprite that is drawn from its picture */
    unsigned long long fine_entries_all, fine_entries, covered_pixels, same_at_centre;
    FILE *edge_log;
    const char *edge_log_path;
    long edge_log_over;        /* 16.16 pixels, or -1 for "outside the bound" */
    int objects_check;         /* --objects-check */
    int dashes_check;          /* --dashes-check */
    unsigned long dash_passes, dash_covered, dash_identical, dash_after, dash_sprites;
    long dash_first_difference;
    int dash_first_sprite;     /* the sprite count when the dash routine began, or -1 */
    unsigned dash_colours;     /* bit n: colour number n occurs in a dash sprite's tiles */
    RrObjectSet *object_set;   /* the model's drawing of the pass in progress */
    uint8_t *object_work;      /* RAM as the model says the game will leave it */
    int object_pending;
    unsigned long object_passes, object_identical, object_records, object_dropped, object_pictures;
    long object_first_difference; /* pass, or -1 */
    PcDump pc_dumps[MAX_PC_DUMPS];
    int pc_dump_count;
    long ram_at;               /* handoff whose native RAM to write, or -1 */
    const char *ram_at_path;
    uint8_t excluded[RAM_SIZE]; /* 1 + index of the range covering each byte, or 0 */
    Exclude excludes[MAX_EXCLUDES];
    int exclude_count;
} Replay;

static _Noreturn void input_error(const char *fmt, ...);

/* "F:FILE" of one of the -at options. */
static void add_dump(Replay *r, int kind, const char *option, char *text) {
    char *end;
    unsigned long frame = strtoul(text, &end, 10);
    if (end == text || *end != ':' || end[1] == 0) input_error("%s needs FRAME:FILE, not %s", option, text);
    if (r->dump_count == MAX_DUMPS) input_error("too many -at options (at most %d)", MAX_DUMPS);
    r->dumps[r->dump_count++] = (FrameDump){kind, frame, end + 1, 0};
}

static void put16(FILE *f, unsigned value) {
    fputc((int)(value >> 8) & 0xFF, f);
    fputc((int)value & 0xFF, f);
}

static void put32(FILE *f, uint32_t value) {
    put16(f, value >> 16);
    put16(f, value & 0xFFFF);
}

/* Write what was asked for the frame that has just been run. */
static void write_dumps(Replay *r, const RrGenesis *g) {
    for (int i = 0; i < r->dump_count; i++) {
        FrameDump *d = &r->dumps[i];
        if (d->done || d->frame != r->frames) continue;
        d->done = 1;
        if (d->kind == DUMP_PICTURE) {
            if (rr_genesis_write_ppm(g, d->path) != 0) input_error("cannot write %s", d->path);
            continue;
        }
        FILE *f = fopen(d->path, "wb");
        if (f == NULL) input_error("cannot write %s", d->path);
        if (d->kind == DUMP_SCALED) {
            /* With --wide, the wide picture. */
            int wide = (RR_SCREEN_W + 2 * r->wide_margin) * r->scale, high = RR_SCREEN_H * r->scale;
            const uint32_t *px = r->wide_margin != 0 ? r->wide_px : r->scaled;
            fprintf(f, "P6\n%d %d\n255\n", wide, high);
            for (int p = 0; p < wide * high; p++) {
                fputc((int)(px[p] >> 16) & 0xFF, f);
                fputc((int)(px[p] >> 8) & 0xFF, f);
                fputc((int)px[p] & 0xFF, f);
            }
            if (fclose(f) != 0) input_error("cannot write %s", d->path);
            continue;
        }
        if (d->kind == DUMP_SOURCES) {
            for (int p = 0; p < RR_SCREEN_W * RR_SCREEN_H; p++) {
                fputc(g->sources[p].layer, f);
                fputc(g->sources[p].colour, f);
                put16(f, g->sources[p].detail);
            }
        } else if (d->kind == DUMP_LINE_STATE) {
            for (int line = 0; line < RR_SCREEN_H; line++) {
                const RrVdpLineState *state = &g->line_state[line];
                fprintf(f, "%d", line);
                for (int reg = 0; reg < RR_VDP_REGISTERS; reg++) fprintf(f, " %02X", state->reg[reg]);
                fprintf(f, " %04X %04X %04X %04X\n", state->hscroll[0], state->hscroll[1],
                        state->vscroll[0], state->vscroll[1]);
            }
        } else {
            fputs("RR2VDPSTATE1\n", f);
            fwrite(g->vdp.reg, 1, sizeof g->vdp.reg, f);
            fwrite(g->vdp.vram, 1, sizeof g->vdp.vram, f);
            for (int c = 0; c < 64; c++) put16(f, g->vdp.cram[c]);
            for (int c = 0; c < RR_VDP_VSRAM_ENTRIES; c++) put16(f, g->vdp.vsram[c]);
        }
        if (fclose(f) != 0) input_error("cannot write %s", d->path);
    }
}

static _Noreturn void input_error(const char *fmt, ...) {
    va_list args;
    fprintf(stderr, "rr-race-replay: ");
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    exit(2);
}

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Apply the next record of the handoff file to `ram`. Returns 0 at a clean end of file. */
static int read_handoff(FILE *f, uint8_t *ram, unsigned long expected) {
    uint8_t header[8], change[3];
    size_t got = fread(header, 1, sizeof header, f);
    if (got == 0) return 0;
    if (got != sizeof header) input_error("handoff %lu: truncated record header", expected);
    if (be32(header) != expected) {
        input_error("handoff %lu: found index %u instead", expected, be32(header));
    }
    for (uint32_t n = be32(header + 4); n > 0; n--) {
        if (fread(change, 1, sizeof change, f) != sizeof change) {
            input_error("handoff %lu: truncated record", expected);
        }
        ram[change[0] << 8 | change[1]] = change[2];
    }
    return 1;
}

/* Count the handoffs in the file, validating all of it. */
static unsigned long count_handoffs(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) input_error("cannot open %s", path);
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "RR2HAND1", 8) != 0) {
        input_error("%s is not an RR2HAND1 file", path);
    }
    static uint8_t scratch[RAM_SIZE];
    unsigned long n = 0;
    while (read_handoff(f, scratch, n)) n++;
    fclose(f);
    return n;
}

static uint16_t *read_timesteps(const char *path, unsigned long total) {
    FILE *f = fopen(path, "r");
    if (f == NULL) input_error("cannot open %s", path);
    uint16_t *values = calloc(total ? total : 1, sizeof *values);
    if (values == NULL) input_error("out of memory");
    for (unsigned long k = 0; k < total; k++) {
        unsigned long index;
        unsigned value;
        if (fscanf(f, "%lu %u", &index, &value) != 2 || index != k) {
            input_error("%s: expected timestep %lu of %lu", path, k, total);
        }
        values[k] = (uint16_t)value;
    }
    fclose(f);
    return values;
}

static void add_exclude(Replay *r, char *text) {
    char *end;
    unsigned long start = strtoul(text, &end, 16);
    if (end == text || *end != ':') input_error("expected START:END:REASON, got '%s'", text);
    char *second = end + 1;
    unsigned long stop = strtoul(second, &end, 16);
    if (end == second || *end != ':' || end[1] == 0 || start > stop || stop >= RAM_SIZE) {
        input_error("expected START:END:REASON with START <= END < 10000, got '%s'", text);
    }
    if (r->exclude_count == MAX_EXCLUDES) input_error("too many --exclude ranges");
    r->excludes[r->exclude_count] = (Exclude){(unsigned)start, (unsigned)stop, end + 1, 0};
    r->exclude_count++;
    for (unsigned long i = start; i <= stop; i++) r->excluded[i] = (uint8_t)r->exclude_count;
}

static void read_pads(Replay *r, const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL) input_error("cannot open %s", path);
    unsigned long capacity = 0, segments = 0;
    char line[64];
    unsigned long line_number = 0;
    r->segment_start = malloc(sizeof *r->segment_start);
    if (r->segment_start == NULL) input_error("out of memory");
    r->segment_start[0] = 0;
    r->segment_count = 1;
    segments = 1;
    while (fgets(line, sizeof line, f) != NULL) {
        unsigned port, value;
        line_number++;
        if (line[0] == 'H') {
            if (r->segment_count == segments) {
                segments *= 2;
                r->segment_start = realloc(r->segment_start, segments * sizeof *r->segment_start);
                if (r->segment_start == NULL) input_error("out of memory");
            }
            r->segment_start[r->segment_count++] = r->pad_count;
            continue;
        }
        if (sscanf(line, "%u %x", &port, &value) != 2 || (port != 1 && port != 2) || value > 0xFF) {
            input_error("%s line %lu: expected 'H' or '<port> <hex value>'", path, line_number);
        }
        if (r->pad_count == capacity) {
            capacity = capacity ? capacity * 2 : 4096;
            r->pad_port = realloc(r->pad_port, capacity);
            r->pad_value = realloc(r->pad_value, capacity);
            if (r->pad_port == NULL || r->pad_value == NULL) input_error("out of memory");
        }
        r->pad_port[r->pad_count] = (uint8_t)port;
        r->pad_value[r->pad_count] = (uint8_t)value;
        r->pad_count++;
    }
    fclose(f);
}

static unsigned long segment_end(const Replay *r, unsigned long segment) {
    return segment + 1 < r->segment_count ? r->segment_start[segment + 1] : r->pad_count;
}

/* Move to the stretch after a handoff, counting recorded reads left unused. */
static void next_segment(Replay *r) {
    if (r->segment < r->segment_count) {
        unsigned long end = segment_end(r, r->segment);
        for (int port = 0; port < 2; port++) {
            for (unsigned long i = r->pad_next[port]; i < end; i++) {
                if (r->pad_port[i] == port + 1) r->pad_dropped++;
            }
        }
    }
    r->segment++;
    unsigned long start = r->segment < r->segment_count ? r->segment_start[r->segment] : r->pad_count;
    r->pad_next[0] = r->pad_next[1] = start;
}

/* The game is about to read a controller: give it what the original read on
 * that port at the same point of the same pass. The raw byte the routine
 * returns is, bit for bit, the RR_PAD_* layout: Start A C B Right Left Down Up
 * from bit 7 down. */
static void on_pad_read(Replay *r, int port) {
    if (r->by_frame) {
        /* The value logged at the latest frame at or before this one. */
        unsigned long *at = &r->frame_pad_at[port - 1], count = r->frame_pad_count[port - 1];
        const unsigned long *frames = r->frame_pad_frame[port - 1];
        while (*at + 1 < count && frames[*at + 1] <= r->frames) (*at)++;
        if (count > 0 && frames[*at] <= r->frames) r->pad_last[port - 1] = r->frame_pad_value[port - 1][*at];
        r->pad_reads++;
        r->g->pad_buttons[port - 1] = r->pad_last[port - 1];
        return;
    }
    unsigned long end = r->segment < r->segment_count ? segment_end(r, r->segment) : 0;
    unsigned long i = r->pad_next[port - 1];
    r->pad_reads++;
    while (i < end && r->pad_port[i] != port) i++;
    if (i < end) {
        r->pad_last[port - 1] = r->pad_value[i];
        r->pad_next[port - 1] = i + 1;
    } else {
        r->pad_next[port - 1] = end;
        r->pad_repeated++;
    }
    r->g->pad_buttons[port - 1] = r->pad_last[port - 1];
    if (r->pad_log != NULL) fprintf(r->pad_log, "%lu %d %02X\n", r->frames, port, r->pad_last[port - 1]);
}

/* " NAME=value" for each tracked RAM location, to end a report line. */
static void report_tracks(const Replay *r) {
    for (int i = 0; i < r->track_count; i++) {
        uint32_t value = 0;
        for (int b = 0; b < r->tracks[i].size; b++) {
            value = value << 8 | r->m->ram[(r->tracks[i].offset + (uint32_t)b) & 0xFFFF];
        }
        fprintf(r->report, " %s=%0*X", r->tracks[i].name, r->tracks[i].size * 2, value);
    }
    fputc('\n', r->report);
}

static void add_track(Replay *r, char *text) {
    char *end;
    unsigned long addr = strtoul(text, &end, 16);
    long size = *end == ':' ? strtol(end + 1, &end, 10) : 0;
    if (end == text || (size != 1 && size != 2 && size != 4) || *end != ':' || end[1] == 0) {
        input_error("--track needs ADDR:SIZE:NAME with SIZE 1, 2 or 4, got '%s'", text);
    }
    if (r->track_count == MAX_TRACKS) input_error("too many --track options");
    r->tracks[r->track_count].offset = (uint32_t)(addr & 0xFFFF);
    r->tracks[r->track_count].size = (int)size;
    r->tracks[r->track_count].name = end + 1;
    r->track_count++;
}

static void read_frame_pads(Replay *r, const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL) input_error("cannot read %s", path);
    unsigned long frame, lines = 0;
    unsigned port, value;
    while (fscanf(f, "%lu %u %x", &frame, &port, &value) == 3) lines++;
    if (!feof(f)) input_error("%s: line %lu is not '<frame> <port> <hex value>'", path, lines + 1);
    rewind(f);
    for (int p = 0; p < 2; p++) {
        r->frame_pad_frame[p] = calloc(lines + 1, sizeof *r->frame_pad_frame[p]);
        r->frame_pad_value[p] = calloc(lines + 1, 1);
        if (r->frame_pad_frame[p] == NULL || r->frame_pad_value[p] == NULL) input_error("out of memory");
    }
    while (fscanf(f, "%lu %u %x", &frame, &port, &value) == 3) {
        if (port < 1 || port > 2) input_error("%s: port %u is not 1 or 2", path, port);
        unsigned long n = r->frame_pad_count[port - 1];
        if (n > 0 && frame < r->frame_pad_frame[port - 1][n - 1]) {
            input_error("%s: frames must not go backwards", path);
        }
        r->frame_pad_frame[port - 1][n] = frame;
        r->frame_pad_value[port - 1][n] = (uint8_t)value;
        r->frame_pad_count[port - 1] = n + 1;
    }
    fclose(f);
}

static void read_regs(Replay *r, const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL) input_error("cannot open regs file %s", path);
    r->reg_cycles = calloc(r->total, sizeof *r->reg_cycles);
    r->reg_values = calloc(r->total, sizeof *r->reg_values);
    if (r->reg_cycles == NULL || r->reg_values == NULL) input_error("out of memory");
    for (unsigned long k = 0; k < r->total; k++) {
        unsigned long long cycles;
        if (fscanf(f, "%llu", &cycles) != 1) {
            input_error("regs file %s: expected %lu lines, found %lu", path, r->total, k);
        }
        r->reg_cycles[k] = cycles;
        for (int i = 0; i < 17; i++) {
            unsigned value;
            if (fscanf(f, "%x", &value) != 1) input_error("regs file %s: line %lu is short", path, k + 1);
            r->reg_values[k][i] = value;
        }
    }
    fclose(f);
}

/* Replace the native state with the original's at handoff k. */
static void resync(Replay *r, unsigned long k) {
    RrCpu *cpu = &r->m->cpu;
    memcpy(r->m->ram, r->current, RAM_SIZE);
    cpu->sr = (uint16_t)r->reg_values[k][0];
    for (int i = 0; i < 8; i++) cpu->d[i] = r->reg_values[k][1 + i];
    for (int i = 0; i < 8; i++) cpu->a[i] = r->reg_values[k][9 + i];
    /* MAME's own cycle count, so the video position is MAME's too. */
    cpu->cycles = r->reg_cycles[k];
    /* The frame in progress stops here and resumes from the new clock; the
     * hook then runs again before the handoff instruction executes. */
    rr_genesis_time_changed(r->g);
    r->resumed_at_handoff = 1;
}

static void on_handoff(Replay *r) {
    RrCpu *cpu = &r->m->cpu;
    unsigned long k = r->k;
    if ((long)k == r->speed_from) r->speed_due = 1;
    if (r->by_frame) {
        /* Not compared: the reference does not line up with this run. */
        if (r->report != NULL) {
            fprintf(r->report, "%lu %lu %u - - -", k, r->frames, (unsigned)(uint16_t)cpu->d[1]);
            report_tracks(r);
        }
        r->k++;
        return;
    }
    uint16_t native = (uint16_t)cpu->d[1], recorded = r->timesteps[k];
    if (native == recorded) {
        r->timestep_same++;
    } else {
        r->timestep_different++;
        if (r->force_timestep) cpu->d[1] = (cpu->d[1] & 0xFFFF0000u) | recorded;
    }

    if ((long)k == r->ram_at) {
        FILE *f = fopen(r->ram_at_path, "wb");
        if (f == NULL || fwrite(r->m->ram, 1, RAM_SIZE, f) != RAM_SIZE) {
            input_error("cannot write %s", r->ram_at_path);
        }
        fclose(f);
        /* And the registers, in the regs file's layout, for comparing by eye. */
        printf("REGS_AT %lu %llu %04X", k, (unsigned long long)cpu->cycles,
               cpu->sr);
        for (int i = 0; i < 8; i++) printf(" %08X", cpu->d[i]);
        for (int i = 0; i < 8; i++) printf(" %08X", cpu->a[i]);
        putchar('\n');
    }

    unsigned long differing = 0;
    unsigned offsets[FIRST_DIFFS];
    int noted = 0;
    for (unsigned i = 0; i < RAM_SIZE; i++) {
        if (r->m->ram[i] == r->current[i]) continue;
        if (r->stack_floor >= 0 && (long)i >= r->stack_floor && i < (cpu->a[7] & 0xFFFF)) {
            r->stack_hidden++;
            continue;
        }
        if (r->excluded[i]) {
            r->excludes[r->excluded[i] - 1].hidden++;
            continue;
        }
        if (noted < FIRST_DIFFS) offsets[noted++] = i;
        if (r->offset_tally != NULL) r->offset_tally[i]++;
        differing++;
    }
    if (differing == 0) {
        r->ram_identical++;
    } else {
        r->ram_differing++;
        if (r->first_differing < 0) {
            r->first_differing = (long)k;
            r->first_count = noted;
            memcpy(r->first_offsets, offsets, sizeof offsets);
        }
    }
    unsigned regs_wrong = 0;
    if (r->compare_regs) {
        if (cpu->cycles != r->reg_cycles[k]) regs_wrong++;
        if (cpu->sr != (uint16_t)r->reg_values[k][0]) regs_wrong++;
        for (int i = 0; i < 8; i++) {
            if (cpu->d[i] != r->reg_values[k][1 + i]) regs_wrong++;
            if (cpu->a[i] != r->reg_values[k][9 + i]) regs_wrong++;
        }
        if (regs_wrong) r->regs_differing++;
        else r->regs_identical++;
    }
    if (r->report != NULL) {
        fprintf(r->report, "%lu %lu %u %u %lu %u", k, r->frames, native, recorded, differing,
                regs_wrong);
        report_tracks(r);
    }

    if (r->resync) resync(r, k);
    next_segment(r);
    r->k++;
    if (r->k == r->total) {
        r->finished = 1;
        return;
    }
    read_handoff(r->handoffs, r->current, r->k);
}

/* --follow: compare this instruction with the next line of a recorded trace
 * and stop at the first that differs, showing the lines leading up to it. */
static void follow_step(Replay *r) {
    char line[256];
    do {
        if (fgets(line, sizeof line, r->follow) == NULL) {
            printf("FOLLOW matched all %llu recorded instructions\n", r->follow_line);
            fclose(r->follow);
            r->follow = NULL;
            return;
        }
    } while (line[0] != 'T');
    const RrCpu *c = &r->m->cpu;
    unsigned pc, sr, reg[16];
    unsigned long long cycles;
    int n = 0, used = 0;
    const char *at = line + 1;
    int ok = sscanf(at, "%x %x%n", &pc, &sr, &used) == 2;
    at += used;
    for (int i = 0; ok && i < 16; i++) {
        ok = sscanf(at, "%x%n", &reg[i], &n) == 1;
        at += n;
    }
    ok = ok && sscanf(at, "%llu", &cycles) == 1;
    if (!ok) input_error("--follow: cannot read trace line %llu", r->follow_line + 1);
    if (r->follow_line == 0 && r->follow_boot && pc != (c->pc & 0xFFFFFF)) {
        rewind(r->follow); /* the recording starts a few instructions after power-on */
        return;
    }
    int same = pc == (c->pc & 0xFFFFFF) && cycles == c->cycles;
    for (int i = 0; i < 8; i++) same = same && reg[i] == c->d[i] && reg[8 + i] == c->a[i];
    if (same) {
        snprintf(r->follow_recent[r->follow_line % 8], sizeof r->follow_recent[0], "%s", line);
        r->follow_line++;
        return;
    }
    printf("FOLLOW differs at recorded instruction %llu; the last matching ones:\n", r->follow_line);
    for (unsigned long long i = r->follow_line > 8 ? r->follow_line - 8 : 0; i < r->follow_line; i++) {
        printf("  %s", r->follow_recent[i % 8]);
    }
    printf("  recorded: %s", line);
    printf("  native:   T %06X %04X", c->pc & 0xFFFFFF, c->sr);
    for (int i = 0; i < 8; i++) printf(" %08X", c->d[i]);
    for (int i = 0; i < 8; i++) printf(" %08X", c->a[i]);
    printf(" %llu\n", (unsigned long long)c->cycles);
    fclose(r->follow);
    r->follow = NULL;
    r->follow_differed = 1;
    r->finished = 1;
}

static void on_ram_write(RrMachine *m, uint32_t offset, int size, uint32_t value, void *ctx) {
    Replay *r = ctx;
    long pass = (long)r->k - 1;
    (void)m;
    if (pass < r->ram_writes_from || pass >= r->ram_writes_to) return;
    fprintf(r->ram_writes, "%ld %lu %06X FF%04X %d %0*X\n", pass, r->frames, r->current_pc, offset,
            size, size * 2, value);
}

static void on_video_write(void *ctx, const RrVideoEvent *e) {
    Replay *r = ctx;
    static const char *const kinds[] = {"reg", "vram", "cram", "vsram", "dma", "fill", "copy"};
    static const char *const memories[] = {"", "-vram", "-cram", "-vsram"};
    /* r->k is the next handoff, so the pass in progress began at handoff k - 1. */
    long pass = (long)r->k - 1;
    if (pass < r->video_log_from || pass >= r->video_log_to) return;
    fprintf(r->video_log, "%ld %llu %d %06X %u %s%s %04X %04X ", pass, (unsigned long long)e->frame,
            e->line, r->current_pc, (unsigned)(r->m->cpu.sr >> 8) & 7, kinds[e->kind],
            e->kind == RR_VIDEO_DMA ? memories[e->memory] : "", e->target, e->value);
    if (e->kind >= RR_VIDEO_DMA) fprintf(r->video_log, "%06X\n", e->source);
    else fputs("-\n", r->video_log);
}

static long magnitude(long v) { return v < 0 ? -v : v; }

/* One line of the edge log (see the header comment). */
static void log_edge(Replay *r, int line, int plane) {
    const RrRoadShifts *fine = &r->road.shifts, *whole = &r->road.whole;
    const RrRoad *road = &r->road.recent[r->road.chosen];
    const int16_t *xs = plane == 0 ? road->left : road->right;
    const int32_t *f = plane == 0 ? fine->a : fine->b, *w = plane == 0 ? whole->a : whole->b;
    int scale = fine->scale;
    /* The run this line is on, by the game's rule: the last pair of points that covers it. */
    double exact = 0;
    int beyond = 0; /* lines past the nearer point of the run: the run is extended that far */
    for (int i = 0; i + 1 < road->count; i++) {
        int top = road->line[i + 1], run = road->line[i] - top;
        if (line < top || line >= road->clipped_line[i]) continue;
        exact = xs[i + 1] + (run > 0 ? (double)(xs[i] - xs[i + 1]) * (line - top) / run : 0.0);
        beyond = line - road->line[i];
    }
    exact += road->centre_x;
    if (plane == 0) exact = (exact > 0x140 ? 0x140 : exact < -0x160 ? -0x160 : exact) - 0x2A0;
    else exact = (exact > 0x180 ? 0x180 : exact < 0 ? 0 : exact) + 0x2A0;
    long jump = 0;
    for (int other = line - 1; other <= line + 1; other += 2) {
        if (other < 0 || other >= RR_SCREEN_H || !whole->have[other]) continue;
        long change = magnitude((long)w[other * scale] - w[line * scale]);
        if (change > jump) jump = change;
    }
    fprintf(r->edge_log, "%lu %d %d %.3f %.3f %.3f %.3f %d", r->frames, line, plane, w[line * scale] / 65536.0,
            f[line * scale] / 65536.0, exact, jump / 65536.0, beyond > 0 ? beyond : 0);
    for (int other = line - 1; other <= line + 1; other += 2) {
        if (other < 0 || other >= RR_SCREEN_H || !whole->have[other]) fputs(" -", r->edge_log);
        else fprintf(r->edge_log, " %.3f", w[other * scale] / 65536.0);
    }
    fputc('\n', r->edge_log);
}

/* How far the fine shifts chosen for this frame are from the game's whole-pixel ones. */
static void measure_edges(Replay *r) {
    const RrRoadShifts *fine = &r->road.shifts, *whole = &r->road.whole;
    int scale = fine->scale;
    for (int line = 0; line < RR_SCREEN_H; line++) {
        if (!fine->have[line]) continue;
        for (int plane = 0; plane < 2; plane++) {
            const int32_t *f = plane == 0 ? fine->a : fine->b, *w = plane == 0 ? whole->a : whole->b;
            long difference = magnitude((long)f[line * scale] - w[line * scale]);
            /* The edge's movement over one line, taken from whichever neighbour has a shift. */
            int other = line + 1 < RR_SCREEN_H && fine->have[line + 1] ? line + 1 : line > 0 && fine->have[line - 1] ? line - 1 : line;
            long moved = magnitude((long)f[other * scale] - f[line * scale]);
            long moved_whole = magnitude((long)w[other * scale] - w[line * scale]);
            if (moved_whole > moved) moved = moved_whole;
            r->edge_lines++;
            r->edge_within_1px += difference < 0x10000;
            r->edge_within_bound += difference < 0x20000 + moved;
            for (int k = 0; k < 3; k++) r->edge_over[k] += difference >= (0x20000L << k);
            if (difference > r->edge_max) r->edge_max = difference;
            int far_off = r->edge_log_over >= 0 ? difference >= r->edge_log_over : !(difference < 0x20000 + moved);
            if (far_off && r->edge_log != NULL) log_edge(r, line, plane);
        }
    }
}

/* --fine-objects, before a frame's first line: which object set is on screen,
 * and which screen pixels lie inside the sprites drawn from its pictures. */
static void choose_objects(Replay *r, RrGenesis *g) {
    if (!r->fine_objects) return;
    const RrFineSprites *fine = rr_object_tracker_choose(r->objects, &g->vdp);
    r->pending_fine = NULL;
    g->hires_sprites = fine;
    g->hires_sprites_stamp = g->vdp.vram_writes;
    memset(r->covered, 0, RR_SCREEN_W * RR_SCREEN_H);
    const RrObjectSet *set = rr_object_tracker_chosen(r->objects);
    if (fine == NULL || set == NULL) return;
    r->pending_fine = fine;
    for (int e = set->first_sprite; e < set->sprites; e++) {
        const uint8_t *entry = set->list + 8 * e;
        r->fine_entries_all++;
        if (fine->picture[e] < 0) continue;
        r->fine_entries++;
        int y = ((entry[0] << 8 | entry[1]) & 0x1FF) - 128, x = ((entry[6] << 8 | entry[7]) & 0x1FF) - 128;
        int wide = (((entry[2] >> 2) & 3) + 1) * 8, high = ((entry[2] & 3) + 1) * 8;
        for (int line = y < 0 ? 0 : y; line < y + high && line < RR_SCREEN_H; line++)
            for (int column = x < 0 ? 0 : x; column < x + wide && column < RR_SCREEN_W; column++)
                r->covered[line * RR_SCREEN_W + column] = 1;
    }
}

/* --fine-objects, after a frame: how many covered pixels still show the
 * original's colour at their centre. */
static void measure_objects(Replay *r, const RrGenesis *g) {
    int scale = r->scale, wide = RR_SCREEN_W * scale, middle = scale / 2;
    for (int y = 0; y < RR_SCREEN_H; y++)
        for (int x = 0; x < RR_SCREEN_W; x++) {
            if (!r->covered[y * RR_SCREEN_W + x]) continue;
            r->covered_pixels++;
            r->same_at_centre += r->scaled[((size_t)y * scale + middle) * wide + x * scale + middle] ==
                                 g->frame[y * RR_SCREEN_W + x];
        }
}

/* ---- --inbetween ---- */

enum { SAMPLE_LINES = 4, MOTION_LINE = 2 };
static const int sample_line[SAMPLE_LINES] = {100, 115, 130, 145};

typedef struct {
    double *value;
    size_t count, room;
} Samples;

static void add_sample(Samples *s, double value) {
    if (s->count == s->room) {
        s->room = s->room ? s->room * 2 : 4096;
        s->value = realloc(s->value, s->room * sizeof *s->value);
        if (s->value == NULL) input_error("out of memory");
    }
    s->value[s->count++] = value;
}

static int by_value(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* The value below which `share` of the samples lie (0 if there are none). */
static double percentile(Samples *s, double share) {
    if (s->count == 0) return 0;
    qsort(s->value, s->count, sizeof *s->value, by_value);
    size_t at = (size_t)(share * (double)s->count);
    return s->value[at < s->count ? at : s->count - 1];
}

/* What one drawn scene shows, kept to compare with the next frame and the next update. */
typedef struct {
    int valid, guessed, moving;
    uint64_t newer_id;                 /* the newer update of the pair it was made from */
    uint8_t have[SAMPLE_LINES];
    int32_t edge[SAMPLE_LINES];        /* plane A's shift on row 0: the left edge, 16.16 */
    int objects;
    uint64_t key[RR_BETWEEN_RECORDS];
    int64_t x[RR_BETWEEN_RECORDS];     /* 16.16 */
    int dash_top;                      /* the row where the nearest dash begins, or -1 */
    uint64_t largest_key;              /* the largest followed roadside object, or 0 */
    int64_t largest_x;
} SceneSample;

typedef struct BetweenCheck {
    /* The update that arrived last, as the trackers gave it. */
    int previous_valid;
    uint64_t previous_id;
    RrRoadShifts previous_shifts;
    RrObjectSet previous_set;
    SceneSample last;                  /* the last scene drawn */
    int today_valid;
    int32_t today_edge;
    unsigned long ends_made, ends_unmade[RR_BETWEEN_REASONS];
    unsigned long ends, road_at_0, road_at_1, objects_at_0, objects_at_1, table_at_1;
    long first_difference;             /* the frame of the first pair that differed, or -1 */
    unsigned long triples, guesses;
    Samples skip_edge, skip_object, guess_edge, guess_object;
    unsigned long edge_still, edge_moving, object_still, object_moving, today_still, today_moving;
    unsigned long dash_still, dash_moving;
    double largest_step;
    RrBetweenRecords records[3];
} BetweenCheck;

/* What the scene just made shows. */
static void sample_scene(const RrBetween *b, SceneSample *out) {
    static RrBetweenRecords base;
    int scale = b->shifts.scale;
    const RrBetweenUpdate *a = &b->update[b->older], *z = &b->update[b->newer];
    out->valid = 1;
    out->guessed = b->t > 0x10000;
    out->moving = a->inputs.place != z->inputs.place;
    out->newer_id = z->id;
    for (int i = 0; i < SAMPLE_LINES; i++) {
        out->have[i] = b->road_have[sample_line[i]];
        out->edge[i] = b->shifts.a[sample_line[i] * scale];
    }
    /* The nearest dash: the unbroken run of strips nearest the bottom of the road. */
    out->dash_top = -1;
    if (b->fine.strips != NULL) {
        int row = RR_SCREEN_H * scale - 1;
        while (row >= 0 && b->strips.to[row] <= b->strips.from[row]) row--;
        while (row > 0 && b->strips.to[row - 1] > b->strips.from[row - 1]) row--;
        out->dash_top = row;
    }
    /* The records as drawn: their whole parts are in the scratch image, the fractions on the pictures. */
    rr_between_read(b->scratch, &base);
    out->objects = 0;
    out->largest_key = 0;
    unsigned largest = 0;
    for (int slot = 0; base.ok && slot < base.count; slot++) {
        const RrBetweenRecord *r = &base.record[slot];
        if (r->key == 0) continue;
        int64_t x = (int64_t)r->x * 65536;
        int drawn = 0;
        for (int i = 0; i < b->set.pictures && !drawn; i++) {
            if (b->set.picture_record[i] != slot) continue;
            x += b->set.picture[i].fx;
            drawn = 1;
        }
        if (!drawn) continue;
        out->key[out->objects] = r->key;
        out->x[out->objects++] = x;
        if ((r->key >> 60) == 1 && r->size > largest) {
            largest = r->size;
            out->largest_key = r->key;
            out->largest_x = x;
        }
    }
}

/* How far a sample is from an update as the game drew it. */
static void compare_sample(const SceneSample *s, const RrRoadShifts *road, const RrBetweenRecords *real,
                           Samples *edges, Samples *objects) {
    for (int i = 0; i < SAMPLE_LINES; i++) {
        if (!s->have[i] || !road->have[sample_line[i]]) continue;
        add_sample(edges, fabs((double)s->edge[i] - (double)road->a[sample_line[i] * road->scale]) / 65536.0);
    }
    for (int i = 0; i < s->objects; i++)
        for (int slot = 0; slot < real->count; slot++) {
            if (real->record[slot].key != s->key[i]) continue;
            add_sample(objects, fabs((double)s->x[i] / 65536.0 - (double)real->record[slot].x));
        }
}

/* A new update has just been kept: compare what can be compared with it. */
static void measure_arrival(Replay *r, const RrGenesis *g) {
    RrBetween *b = r->between;
    BetweenCheck *c = r->check;
    const uint8_t *rom = r->m->rom;
    size_t rom_size = r->m->rom_size;
    int n = b->count - 1;
    const RrObjectSet *set = rr_object_tracker_chosen(r->objects);
    const RrBetweenUpdate *newest = &b->update[n];
    rr_between_read(newest->object_ram, &c->records[2]);
    /* The guess: the last scene drawn, if it was past the update before this one. */
    if (newest->good && c->last.valid && c->last.guessed && n >= 1 && c->last.newer_id == b->update[n - 1].id) {
        compare_sample(&c->last, &r->road.shifts, &c->records[2], &c->guess_edge, &c->guess_object);
        c->guesses++;
    }
    int pair = n >= 1 && newest->good && b->update[n - 1].good && c->previous_valid &&
               c->previous_id == b->update[n - 1].id;
    if (pair) {
        int why_0 = rr_between_scene(b, n - 1, n, 0, set, &g->vdp, NULL, rom, rom_size, r->scale), at_0 = why_0 < 0;
        int road_0 = at_0 && rr_between_same_road(b, &c->previous_shifts), objects_0 = at_0 && rr_between_same_objects(&b->set, &c->previous_set);
        int why_1 = rr_between_scene(b, n - 1, n, 0x10000, set, &g->vdp, NULL, rom, rom_size, r->scale), at_1 = why_1 < 0;
        int road_1 = at_1 && rr_between_same_road(b, &r->road.shifts), objects_1 = at_1 && rr_between_same_objects(&b->set, set);
        int table_1 = at_1 && rr_between_same_table(b, &g->vdp);
        c->ends++;
        c->road_at_0 += (unsigned long)road_0;
        c->objects_at_0 += (unsigned long)objects_0;
        c->road_at_1 += (unsigned long)road_1;
        c->objects_at_1 += (unsigned long)objects_1;
        c->table_at_1 += (unsigned long)table_1;
        if (!(road_0 && objects_0 && road_1 && objects_1 && table_1) && c->first_difference < 0)
            c->first_difference = (long)r->frames;
        if (at_0 && at_1) c->ends_made++;
        else c->ends_unmade[at_1 ? why_0 : why_1]++;
        if (getenv("RR_INBETWEEN_DEBUG") != NULL && !(road_0 && objects_0 && road_1 && objects_1 && table_1))
            fprintf(stderr, "ENDS frame=%lu why0=%d why1=%d road0=%d obj0=%d road1=%d obj1=%d table1=%d place %08X -> %08X sprites %d -> %d\n",
                    r->frames, why_0, why_1, road_0, objects_0, road_1, objects_1, table_1,
                    (unsigned)b->update[n - 1].inputs.place, (unsigned)newest->inputs.place, c->previous_set.sprites,
                    set->sprites);
    }
    /* Skip one: the update before this, guessed from its two neighbours. */
    if (pair && n >= 2 && b->update[n - 2].good) {
        uint64_t whole = newest->stamp - b->update[n - 2].stamp, part = b->update[n - 1].stamp - b->update[n - 2].stamp;
        int32_t t = whole > 0 ? (int32_t)((part << 16) / whole) : 0;
        if (rr_between_scene(b, n - 2, n, t, set, &g->vdp, NULL, rom, rom_size, r->scale) < 0) {
            static SceneSample skipped;
            sample_scene(b, &skipped);
            rr_between_read(b->update[n - 1].object_ram, &c->records[1]);
            compare_sample(&skipped, &c->previous_shifts, &c->records[1], &c->skip_edge, &c->skip_object);
            c->triples++;
        }
    }
    c->previous_valid = newest->good && set != NULL && r->road.has_fine[r->road.chosen];
    c->previous_id = newest->id;
    if (c->previous_valid) {
        c->previous_shifts = r->road.shifts;
        c->previous_set = *set;
    }
}

/* A scene has just been made for this frame: how far did things move since the last? */
static void measure_motion(Replay *r) {
    static SceneSample now;
    BetweenCheck *c = r->check;
    sample_scene(r->between, &now);
    if (c->last.valid && now.moving) {
        if (now.have[MOTION_LINE] && c->last.have[MOTION_LINE]) {
            double step = fabs((double)now.edge[MOTION_LINE] - (double)c->last.edge[MOTION_LINE]) / 65536.0;
            if (step < 1.0 / 16) c->edge_still++;
            else c->edge_moving++;
            if (step > c->largest_step) c->largest_step = step;
        }
        if (now.dash_top >= 0 && c->last.dash_top >= 0) {
            if (now.dash_top == c->last.dash_top) c->dash_still++;
            else c->dash_moving++;
        }
        if (now.largest_key != 0 && now.largest_key == c->last.largest_key) {
            double step = fabs((double)(now.largest_x - c->last.largest_x)) / 65536.0;
            if (step < 1.0 / 16) c->object_still++;
            else c->object_moving++;
        }
    }
    c->last = now;
}

/* --inbetween, before a frame's first line and after the trackers have chosen. */
static void choose_between(Replay *r, RrGenesis *g) {
    RrBetween *b = r->between;
    if (b == NULL) return;
    b->margin = r->wide_margin;
    uint64_t newest_before = b->count > 0 ? b->update[b->count - 1].id : 0;
    rr_between_note(b, r->frames, &r->road, r->objects, &g->vdp);
    int arrived = b->count > 0 && b->update[b->count - 1].id != newest_before;
    if (arrived && r->check != NULL) measure_arrival(r, g);
    /* Today's picture, for comparison: does the road edge stand still from frame to frame? */
    if (r->check != NULL) {
        BetweenCheck *c = r->check;
        int line = sample_line[MOTION_LINE];
        int have = g->hires_have != NULL && g->hires_have[line];
        int moving = b->count >= 2 && b->update[b->count - 1].inputs.place != b->update[b->count - 2].inputs.place;
        if (have && c->today_valid && moving) {
            if (g->hires_shift_a[line * r->scale] == c->today_edge) c->today_still++;
            else c->today_moving++;
        }
        c->today_valid = have;
        if (have) c->today_edge = g->hires_shift_a[line * r->scale];
    }
    if (!rr_between_frame(b, r->frames, &r->road, r->objects, &g->vdp, r->m->rom, r->m->rom_size, r->scale)) {
        if (r->check != NULL) r->check->last.valid = 0;
        return;
    }
    g->hires_shift_a = b->shifts.a;
    g->hires_shift_b = b->shifts.b;
    g->hires_have = b->shifts.have;
    g->hires_expect_a = b->expect.a;
    g->hires_expect_b = b->expect.b;
    if (r->wide_margin != 0) {
        /* The scene's own edges beyond the limits, and its own margin objects. */
        g->hires_beyond_a = b->shifts.beyond_a;
        g->hires_beyond_b = b->shifts.beyond_b;
        b->margin_fine.soft = (uint8_t)r->soft_objects;
        g->hires_margin_sprites = b->margin_ok ? &b->margin_fine : NULL;
        if (b->margin_ok) {
            unsigned gaps = rr_objects_margin_gaps(&b->margin_set, b->margin_table, &b->margin_fine, r->wide_margin);
            r->margin_gaps += gaps;
        }
        r->wide_between = 1;
    }
    b->fine.soft = (uint8_t)r->soft_objects;
    g->hires_sprites = &b->fine;
    g->hires_sprites_stamp = g->vdp.vram_writes;
    if (r->frame_strips && b->fine.strips == NULL) r->dash_strip_frames--; /* this frame keeps the game's dash sprites */
    else if (!r->frame_strips && b->fine.strips != NULL) r->dash_strip_frames++;
    if (r->check != NULL) measure_motion(r);
}

static void log_ground_row(Replay *r, int line, int which, const char *kind, int runs, const int *first,
                           const int *end) {
    if (r->ground_log == NULL) return;
    int top = 0;
    while (top < RR_SCREEN_H && !r->road.whole.have[top]) top++;
    fprintf(r->ground_log, "%lu %d %c %s reg7=%02X road_top=%d runs=%d", r->scaled_frames, line,
            which == 0 ? 'A' : 'B', kind, r->g->vdp.reg[7], top, runs);
    for (int i = 0; i < runs && i < 8; i++) fprintf(r->ground_log, " %d..%d", first[i], end[i]);
    fputc('\n', r->ground_log);
}

/* --ground-check: what each plane holds on this line, just before it is drawn.
 * On a line the road reaches (the tracker's whole-pixel shifts say which) each
 * plane's row should be the ground block with its marks, or full, or empty;
 * on any other line it should not look like the ground block. */
static void measure_ground_line(RrGenesis *g, int line, void *ctx) {
    Replay *r = ctx;
    if (line == 0) r->ground_frame = g->hires_have != NULL;
    if (!r->ground_frame) return;
    static uint8_t row[1024];
    RrWideBlock block[2];
    rr_wide_blocks(block);
    if (line >= RR_WIDE_LINES) return; /* under the dashboard: not widened */
    int ground = r->road.whole.have[line];
    int road_backdrop = (g->vdp.reg[7] & 0x3F) == RR_WIDE_ROAD_BACKDROP;
    if (ground) {
        r->ground_lines++;
        if (!road_backdrop) r->ground_sky_backdrop++;
    } else {
        r->sky_lines++;
    }
    for (int which = 0; which < 2; which++) {
        const RrWideBlock *b = &block[which];
        int passes = rr_wide_is_ground_row(&g->vdp, which, line, b);
        int first[8], end[8];
        int runs = rr_vdp_plane_row_runs(&g->vdp, which, line, first, end, 8);
        if (!ground) {
            if (passes && road_backdrop) r->sky_passing_road++;
            if (passes && r->sky_passing++ % r->ground_every == 0) log_ground_row(r, line, which, "sky", runs, first, end);
            continue;
        }
        int width = rr_vdp_plane_row_pixels(&g->vdp, which, line, row);
        /* How far the opaque pixels reach beyond a block that is all there. */
        int covered = 0, mark = 0;
        for (int i = 0; i < runs && i < 8; i++) {
            if (first[i] <= b->first && end[i] >= b->end) covered = 1;
        }
        int full = runs == 1 && first[0] == 0 && end[0] == width;
        if (covered && !full) {
            for (int i = 0; i < runs && i < 8; i++) {
                if (b->first - first[i] > mark) mark = b->first - first[i];
                if (end[i] - b->end > mark) mark = end[i] - b->end;
            }
            if (mark > r->ground_mark_max) r->ground_mark_max = mark;
            /* The smallest sideways step the block repeats with (999: none up to 64). */
            int period = 999;
            for (int p = 1; p <= 64 && period == 999; p++) {
                int same = 1;
                for (int x = b->first; same && x + p < b->end; x++) same = row[x] == row[x + p];
                if (same) period = p;
            }
            if (period > r->ground_period_max) r->ground_period_max = period;
        }
        if (passes) r->ground_block++;
        else if (full) r->ground_full++;
        else if (runs == 0) r->ground_empty++;
        else {
            if (r->ground_other++ % r->ground_every == 0) log_ground_row(r, line, which, "ground", runs, first, end);
        }
    }
}

/* --wide: a line of the wide picture has just been drawn. Its margins are
 * judged from the road (a ground line) or counted by kind (any other). */
static void measure_wide_line(RrGenesis *g, int line, void *ctx) {
    Replay *r = ctx;
    if (line >= r->wide.lines) return;
    int scale = r->scale, margin = r->wide_margin;
    const RrPixelSource *rows = &r->wide_src[(size_t)line * scale * (size_t)(RR_SCREEN_W + 2 * margin) * scale];
    /* Ground is where the road on screen reaches (an in-between scene gives every line a shift). */
    int ground = g->hires_have != NULL && g->hires_have[line] && r->road.whole.have[line];
    if (!ground) {
        rr_wide_sky(rows, scale, margin, &r->wide_sky);
        return;
    }
    /* Were the road's shifts used on this line (the frame loop's own rule)? */
    RrVdpLineState now;
    rr_vdp_line_state(&g->vdp, line, &now);
    const int32_t *a = g->hires_expect_a != NULL ? g->hires_expect_a : g->hires_shift_a;
    const int32_t *b = g->hires_expect_b != NULL ? g->hires_expect_b : g->hires_shift_b;
    int used = ((a[line * scale] >> 16) & 0x3FF) == now.hscroll[0] && ((b[line * scale] >> 16) & 0x3FF) == now.hscroll[1];
    /* The block falls short of the view where its far end is inside a margin:
     * plane A's when the left edge is within `margin` of the screen's right
     * edge, plane B's when the right edge is within `margin` of its left. */
    int left = (int)((now.hscroll[0] + 0x2A0) & 0x3FF), right = (int)((now.hscroll[1] - 0x2A0) & 0x3FF);
    if (left > 0x140) left -= 0x400; /* the game's limits: -0x160..0x140 and 0..0x180 */
    if (left > RR_SCREEN_W - margin || right < margin) {
        r->wide_ground.short_lines++;
        r->wide_short_frame = 1;
    }
    if (used && r->wide_tracked) {
        rr_wide_ground(rows, r->wide_between ? &r->between->shifts : &r->road.shifts, &g->vdp, line, scale, margin,
                       &r->wide_ground);
    } else if (!used && (left == RR_SCREEN_W || right == 0)) {
        r->wide_no_edge++; /* held at the screen's edge with nothing known beyond: grass runs on */
    }
}

/* --wide: a frame has been drawn. Its middle against the picture without margins. */
static void measure_wide_frame(Replay *r, RrGenesis *g) {
    int scale = r->scale, margin = r->wide_margin;
    size_t across = (size_t)RR_SCREEN_W * scale, stride = across + 2 * (size_t)margin * scale;
    for (size_t row = 0; row < (size_t)RR_SCREEN_H * scale; row++)
        for (size_t c = 0; c < across; c++) {
            const RrPixelSource *p = &r->wide_src[row * stride + (size_t)margin * scale + c];
            r->wide_cover[row * across + c] = p->layer == RR_SRC_SPRITE;
            /* The seam: a pixel of the margin's objects on the screen itself. */
            int seam = p->layer == RR_SRC_SPRITE && p->detail != 0xFFFF && (p->detail & RR_MARGIN_SPRITE);
            if (seam) r->seam_pixels++;
            else if (r->wide_px[row * stride + (size_t)margin * scale + c] != r->scaled[row * across + c])
                r->outside_seam++; /* a difference from the narrow picture that the seam does not explain */
        }
    uint64_t before = r->wide_centre.differing;
    static const uint8_t no_road[RR_SCREEN_H];
    rr_wide_centre(r->wide_px, r->scaled, scale, margin, r->wide.lines, g->hires_have != NULL ? g->hires_have : no_road,
                   r->wide_cover, g->hires_sprites != NULL ? g->hires_sprites->strips : NULL, &r->wide_centre);
    r->wide_frames++;
    if (r->wide_centre.differing == before) r->wide_identical++;
    else if (r->wide_first < 0) r->wide_first = (long)r->frames;
    if (r->wide_short_frame) r->wide_short_updates++;
    r->wide_short_frame = 0;
}

/* Before a frame's first line: which road, if any, the enlarged picture is drawn with. */
static void choose_road(RrGenesis *g, void *ctx) {
    Replay *r = ctx;
    unsigned long long fine_before = r->road.fine_frames, race_before = r->road.race_frames;
    choose_objects(r, g);
    const RrRoadShifts *shifts = r->points_none ? NULL :
        rr_road_tracker_choose(&r->road, r->m->ram, r->m->rom, r->m->rom_size, &g->vdp, r->scale);
    g->hires_shift_a = shifts != NULL ? shifts->a : NULL;
    g->hires_shift_b = shifts != NULL ? shifts->b : NULL;
    g->hires_have = shifts != NULL ? shifts->have : NULL;
    g->hires_expect_a = shifts != NULL ? r->road.whole.a : NULL;
    g->hires_expect_b = shifts != NULL ? r->road.whole.b : NULL;
    if (r->road.fine_frames != fine_before) measure_edges(r);
    /* The centre-line dashes as strips, where they are proven. */
    if (r->fine_objects && shifts != NULL) {
        r->dash_frames++;
        if (rr_dashes_frame_wide(&r->road, rr_object_tracker_chosen(r->objects), r->pending_fine, &g->vdp, r->scale,
                                 r->wide_margin, r->strips, &r->frame_fine)) {
            g->hires_sprites = &r->frame_fine;
            r->dash_strip_frames++;
            r->frame_strips = 1;
        } else {
            r->frame_strips = 0;
        }
    }
    if (r->soft_objects && g->hires_sprites != NULL) {
        if (g->hires_sprites != &r->frame_fine) r->frame_fine = *g->hires_sprites;
        r->frame_fine.soft = 1;
        g->hires_sprites = &r->frame_fine;
    }
    if (r->wide_margin != 0) {
        /* Wide on the lines of the view of a race picture; anything else between panels. */
        int race = r->road.race_frames != race_before;
        r->wide.lines = race ? RR_WIDE_LINES : 0;
        r->wide_tracked = shifts != NULL;
        if (race && shifts == NULL) r->wide_untracked++;
        g->hires_wide = &r->wide;
        g->hires_beyond_a = shifts != NULL ? shifts->beyond_a : NULL;
        g->hires_beyond_b = shifts != NULL ? shifts->beyond_b : NULL;
        r->wide_between = 0;
        /* The objects of the margins, from the image the set on screen was drawn from. */
        g->hires_margin_sprites = NULL;
        const RrObjectSet *set = rr_object_tracker_chosen(r->objects);
        const uint8_t *image = rr_object_tracker_chosen_ram(r->objects);
        if (race && r->pending_fine != NULL && set != NULL && image != NULL
            && rr_objects_margin(set, image, image, r->m->rom, r->m->rom_size, r->wide_margin,
                                 (int16_t)(image[0xF2B2] << 8 | image[0xF2B3]), r->margin_set, r->margin_table,
                                 &r->margin_fine, r->margin_work)) {
            r->margin_fine.soft = (uint8_t)r->soft_objects;
            g->hires_margin_sprites = &r->margin_fine;
            r->margin_frames++;
            r->margin_pictures += r->margin_set->margin_pictures;
            r->margin_uncut += r->margin_set->margin_uncut;
            r->margin_off_view += r->margin_set->margin_off_view;
            r->margin_copies += r->margin_set->margin_copies;
            r->margin_left_out += r->margin_set->margin_left_out;
            unsigned gaps = rr_objects_margin_gaps(r->margin_set, r->margin_table, &r->margin_fine, r->wide_margin);
            r->margin_gaps += gaps;
        }
    }
    choose_between(r, g);
}

/* Whether every line drawn without a road shift is the original line, each
 * pixel repeated. Pixels inside a sprite drawn from its picture are left out
 * (--fine-objects), so this still says that nothing else changed. */
static int off_road_is_plain(const Replay *r, const RrGenesis *g) {
    int scale = r->scale, wide = RR_SCREEN_W * scale;
    for (int y = 0; y < RR_SCREEN_H; y++) {
        if (g->hires_have != NULL && g->hires_have[y]) continue;
        for (int row = 0; row < scale; row++)
            for (int x = 0; x < wide; x++) {
                if (r->covered != NULL && r->covered[y * RR_SCREEN_W + x / scale]) continue;
                if (r->scaled[((size_t)y * scale + row) * wide + x] != g->frame[y * RR_SCREEN_W + x / scale]) return 0;
            }
    }
    return 1;
}

/* Whether the enlarged picture still holds the original: the last of each
 * original pixel's pixels, on the first of its rows. */
static int samples_back(const Replay *r, const RrGenesis *g) {
    int scale = r->scale, wide = RR_SCREEN_W * scale;
    for (int y = 0; y < RR_SCREEN_H; y++)
        for (int x = 0; x < RR_SCREEN_W; x++) {
            if (r->covered != NULL && r->covered[y * RR_SCREEN_W + x]) continue;
            if (r->scaled[(size_t)y * scale * wide + x * scale + scale - 1] != g->frame[y * RR_SCREEN_W + x]) return 0;
        }
    return 1;
}

/* Append work RAM to an image file as changes from the image before. */
static void write_pc_dump(Replay *r, PcDump *d) {
    const uint8_t *ram = r->m->ram;
    uint32_t changed = 0;
    for (unsigned i = 0; i < RAM_SIZE; i++) changed += ram[i] != d->previous[i];
    put32(d->f, (uint32_t)d->count);
    put32(d->f, changed);
    for (unsigned i = 0; i < RAM_SIZE; i++) {
        if (ram[i] == d->previous[i]) continue;
        put16(d->f, i);
        fputc(ram[i], d->f);
    }
    memcpy(d->previous, ram, RAM_SIZE);
    fprintf(d->index, "%lu %ld %lu\n", d->count, (long)r->k - 1, r->frames);
    d->count++;
}

/* Whether `count` bytes of work RAM from `address` are the same in both. */
static int same_ram(const uint8_t *a, const uint8_t *b, uint32_t address, unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        unsigned at = (address + i) & 0xFFFF;
        if (a[at] != b[at]) return 0;
    }
    return 1;
}

/* --dashes-check, as the game is about to pack the dashes: is its per-line
 * description what the model makes of its road points? */
static void check_dashes(Replay *r) {
    static RrRoad road;
    static RrDashLines made, held;
    const uint8_t *ram = r->m->ram;
    r->dash_passes++;
    r->dash_first_sprite = ram[0xDC34] << 8 | ram[0xDC35];
    rr_dashes_read(ram, &held);
    if (!held.ok || !rr_road_read(ram, &road)) return;
    uint32_t place = (uint32_t)(ram[0x0A0A] << 8 | ram[0x0A0B]) << 16 | (uint32_t)(ram[0x0A0C] << 8 | ram[0x0A0D]);
    rr_dashes_lines(&road, place, rr_dashes_stop_line(ram), r->m->rom, r->m->rom_size, &made);
    r->dash_covered++;
    int same = made.ok;
    /* The lines of the 3D view: above and below it the arrays are not the dashes'. */
    int top = (int16_t)(ram[0xF2B0] << 8 | ram[0xF2B1]), bottom = road.view_bottom;
    for (int line = top < 0 ? 0 : top; same && line < bottom && line < RR_SCREEN_H; line++)
        same = made.start[line] == held.start[line] && made.width[line] == held.width[line];
    r->dash_identical += (unsigned long)same;
    if (!same && r->dash_first_difference < 0) r->dash_first_difference = (long)r->k - 1;
}

/* --dashes-check, as the dash routine returns: what did it add to the sprite list? */
static void count_dash_sprites(Replay *r) {
    const uint8_t *ram = r->m->ram;
    int end = ram[0xDC34] << 8 | ram[0xDC35];
    uint32_t list = (uint32_t)(ram[0x27A8] << 8 | ram[0x27A9]);
    for (int e = r->dash_first_sprite; e < end && e < RR_SPRITES_MAX; e++) {
        unsigned attr = (unsigned)(ram[(list + 8 * (uint32_t)e + 4) & 0xFFFF] << 8);
        r->dash_after++;
        r->dash_sprites += (attr & 0xE000) == 0x6000;
        /* Which colour numbers the sprite's tiles hold, in the picture buffer the game is building. */
        uint32_t buffer = (uint32_t)(ram[0x27D0] << 8 | ram[0x27D1]);
        unsigned word = (unsigned)(ram[(list + 8 * (uint32_t)e + 4) & 0xFFFF] << 8 | ram[(list + 8 * (uint32_t)e + 5) & 0xFFFF]);
        unsigned base = (unsigned)(ram[0xCE86] << 8 | ram[0xCE87]);
        unsigned size = ram[(list + 8 * (uint32_t)e + 2) & 0xFFFF];
        unsigned tiles = (((size >> 2) & 3) + 1) * ((size & 3) + 1), first = ((word & 0x7FF) - base) & 0x7FF;
        for (unsigned i = 0; i < tiles * 32 && first + tiles <= RR_OBJECT_TILES; i++) {
            unsigned byte = ram[(buffer + first * 32 + i) & 0xFFFF];
            r->dash_colours |= (1u << (byte >> 4)) | (1u << (byte & 15));
        }
    }
    r->dash_first_sprite = -1;
}

/* --objects-check: the game has drawn the objects; is that what the model said? */
static void check_objects(Replay *r) {
    const uint8_t *ram = r->m->ram, *work = r->object_work;
    uint32_t list = (uint32_t)(ram[0x27A8] << 8 | ram[0x27A9]), buffer = (uint32_t)(ram[0x27D0] << 8 | ram[0x27D1]);
    int same = r->object_set->ok && same_ram(ram, work, list, RR_SPRITES_MAX * 8) &&
               same_ram(ram, work, buffer, RR_OBJECT_TILES * 32) && same_ram(ram, work, 0xDC30, 2) &&
               same_ram(ram, work, 0xDC34, 2);
    r->object_passes++;
    r->object_identical += (unsigned long)same;
    if (!same && r->object_first_difference < 0) r->object_first_difference = (long)r->k - 1;
    if (r->object_set->ok) {
        r->object_records += r->object_set->records;
        r->object_dropped += r->object_set->dropped;
        r->object_pictures += (unsigned long)r->object_set->pictures;
    }
}

static void step_hook(RrGenesis *g, void *ctx) {
    Replay *r = ctx;
    uint32_t pc = g->m->cpu.pc & 0xFFFFFF;
    r->current_pc = pc;
    if (r->finished) return;
    if (r->resumed_at_handoff) {
        r->resumed_at_handoff = 0;
        return;
    }
    if (r->profiling) {
        /* Charge the cycles since the previous instruction started to it. */
        r->profile_cycles[r->last_pc >> 1] += (uint32_t)(g->m->cpu.cycles - r->last_cycles);
    }
    if (pc == HANDOFF_PC && r->profile_path != NULL) {
        if ((long)r->k == r->profile_from) r->profiling = 1;
        else if ((long)r->k == r->profile_to) r->profiling = 0;
    }
    if (r->profiling && pc < g->m->rom_size) {
        r->profile_count[pc >> 1]++;
        r->last_pc = pc;
        r->last_cycles = g->m->cpu.cycles;
    }
    if (r->objects_check) {
        if (pc == OBJECTS_NOTE_PC) {
            rr_objects_draw(r->m->ram, r->m->rom, r->m->rom_size, r->object_set, r->object_work);
            r->object_pending = 1;
        } else if (pc == OBJECTS_DONE_PC && r->object_pending) {
            r->object_pending = 0;
            check_objects(r);
        }
    }
    if (r->dashes_check && pc == DASHES_PC) check_dashes(r);
    if (r->dashes_check && pc == DASHES_DONE_PC && r->dash_first_sprite >= 0) count_dash_sprites(r);
    if (r->fine_objects && pc == OBJECTS_NOTE_PC) {
        rr_object_tracker_note(r->objects, r->m->ram, r->m->rom, r->m->rom_size);
    }
    if (pc == ROAD_NOTE_PC && r->scale != 0) rr_road_tracker_note(&r->road, r->m->ram, r->m->rom, r->m->rom_size);
    if (pc == DASHES_PC && r->scale != 0) rr_dashes_note(&r->road, r->m->ram, r->m->rom, r->m->rom_size);
    if (pc == ROAD_INPUTS_PC && r->scale != 0) rr_road_tracker_note_inputs(&r->road, r->m->ram);
    for (int i = 0; i < r->pc_dump_count; i++) {
        PcDump *d = &r->pc_dumps[i];
        long pass = (long)r->k - 1;
        if (pc == d->pc && pass >= d->from && pass < d->to) write_pc_dump(r, d);
    }
    if (pc == HANDOFF_PC && (long)r->k == r->follow_at) r->follow_at = -2; /* following from here */
    if (pc == HANDOFF_PC) on_handoff(r);
    else if (pc == PAD1_READ_PC) on_pad_read(r, 1);
    else if (pc == PAD2_READ_PC) on_pad_read(r, 2);
    if (r->follow_at == -2 && r->follow != NULL && !r->finished) follow_step(r);
}

static void put_le(FILE *f, unsigned long value, int bytes) {
    for (int i = 0; i < bytes; i++) fputc((int)((value >> (8 * i)) & 0xFF), f);
}

/* Write (or rewrite, once the length is known) the WAV header. */
static void wav_header(FILE *f, unsigned long frames) {
    rewind(f);
    fwrite("RIFF", 1, 4, f);
    put_le(f, 36 + frames * 4, 4);
    fwrite("WAVEfmt ", 1, 8, f);
    put_le(f, 16, 4);
    put_le(f, 1, 2); /* plain samples */
    put_le(f, 2, 2); /* stereo */
    put_le(f, RR_SOUND_HZ, 4);
    put_le(f, RR_SOUND_HZ * 4, 4);
    put_le(f, 4, 2);
    put_le(f, 16, 2);
    fwrite("data", 1, 4, f);
    put_le(f, frames * 4, 4);
}

static void log_fm_write(void *ctx, uint64_t master, unsigned port, uint8_t value, int from_68000) {
    fprintf((FILE *)ctx, "%llu %u %02X %c\n", (unsigned long long)master, port, value, from_68000 ? 'm' : 'z');
}

/* Move the samples made so far out of the sound side (and into the WAV). */
static void take_sound(Replay *r, RrGenesis *g) {
    int16_t samples[2 * 1024];
    size_t got;
    while ((got = rr_sound_take(g->sound, samples, 1024)) > 0) {
        r->sound_samples += (unsigned long)got;
        if (r->sound_wav == NULL) continue;
        for (size_t i = 0; i < 2 * got; i++) put_le(r->sound_wav, (uint16_t)samples[i], 2);
    }
}

int main(int argc, char **argv) {
    const char *rom_path = NULL, *handoff_path = NULL, *timestep_path = NULL, *report_path = NULL;
    const char *pads_path = NULL, *regs_path = NULL, *offsets_path = NULL;
    const char *sound_wav_path = NULL, *fm_log_path = NULL;
    unsigned long max_frames = 200000;
    unsigned long cpu_speed = 0; /* percent; 0 leaves the original speed */
    int between_mode = RR_BETWEEN_OFF;
    const char *pad_log_path = NULL;
    static Replay r;
    r.first_differing = -1;
    r.ram_at = -1;
    r.edge_log_over = -1;
    r.object_first_difference = -1;
    r.dash_first_difference = -1;
    r.dash_first_sprite = -1;
    r.stack_floor = -1;
    r.profile_from = r.profile_to = -1;
    r.follow_at = -1;
    r.speed_from = -1;
    r.video_log_from = r.video_log_to = -1;
    r.ram_writes_from = r.ram_writes_to = -1;
    uint32_t ram_watch_start = 0, ram_watch_end = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc) rom_path = argv[++i];
        else if (strcmp(argv[i], "--handoffs") == 0 && i + 1 < argc) handoff_path = argv[++i];
        else if (strcmp(argv[i], "--timesteps") == 0 && i + 1 < argc) timestep_path = argv[++i];
        else if (strcmp(argv[i], "--pads") == 0 && i + 1 < argc) pads_path = argv[++i];
        else if (strcmp(argv[i], "--report") == 0 && i + 1 < argc) report_path = argv[++i];
        else if (strcmp(argv[i], "--video-log") == 0 && i + 1 < argc) {
            char *end, *text = argv[++i];
            r.video_log_from = strtol(text, &end, 10);
            char *second = end + 1;
            if (end == text || *end != ':') input_error("--video-log needs A:B:FILE, not %s", text);
            r.video_log_to = strtol(second, &end, 10);
            if (end == second || *end != ':' || end[1] == 0 || r.video_log_to <= r.video_log_from) {
                input_error("--video-log needs A:B:FILE with A below B, not %s", text);
            }
            r.video_log = fopen(end + 1, "w");
            if (r.video_log == NULL) input_error("cannot write %s", end + 1);
        }
        else if (strcmp(argv[i], "--ram-writes") == 0 && i + 1 < argc) {
            char *text = argv[++i], *end;
            unsigned long start, last;
            r.ram_writes_from = strtol(text, &end, 10);
            if (end == text || *end != ':') input_error("--ram-writes needs A:B:START:END:FILE, not %s", text);
            char *field = end + 1;
            r.ram_writes_to = strtol(field, &end, 10);
            if (end == field || *end != ':' || r.ram_writes_to <= r.ram_writes_from) {
                input_error("--ram-writes needs A:B:START:END:FILE with A below B, not %s", text);
            }
            field = end + 1;
            start = strtoul(field, &end, 16);
            if (end == field || *end != ':') input_error("--ram-writes: bad START in %s", text);
            field = end + 1;
            last = strtoul(field, &end, 16);
            if (end == field || *end != ':' || end[1] == 0 || last < start || last > 0xFFFF) {
                input_error("--ram-writes: START and END are hex work-RAM offsets 0000 to FFFF, in %s", text);
            }
            ram_watch_start = (uint32_t)start;
            ram_watch_end = (uint32_t)last + 1;
            r.ram_writes = fopen(end + 1, "w");
            if (r.ram_writes == NULL) input_error("cannot write %s", end + 1);
        }
        else if (strcmp(argv[i], "--picture-at") == 0 && i + 1 < argc) add_dump(&r, DUMP_PICTURE, argv[i], argv[i + 1]), i++;
        else if (strcmp(argv[i], "--sources-at") == 0 && i + 1 < argc) add_dump(&r, DUMP_SOURCES, argv[i], argv[i + 1]), i++;
        else if (strcmp(argv[i], "--line-state-at") == 0 && i + 1 < argc) add_dump(&r, DUMP_LINE_STATE, argv[i], argv[i + 1]), i++;
        else if (strcmp(argv[i], "--video-state-at") == 0 && i + 1 < argc) add_dump(&r, DUMP_VIDEO_STATE, argv[i], argv[i + 1]), i++;
        else if (strcmp(argv[i], "--force-timestep") == 0) r.force_timestep = 1;
        else if (strcmp(argv[i], "--regs") == 0 && i + 1 < argc) regs_path = argv[++i];
        else if (strcmp(argv[i], "--resync") == 0) r.resync = 1;
        else if (strcmp(argv[i], "--compare-regs") == 0) r.compare_regs = 1;
        else if (strcmp(argv[i], "--offsets") == 0 && i + 1 < argc) offsets_path = argv[++i];
        else if (strcmp(argv[i], "--exclude-stack-below") == 0 && i + 1 < argc) {
            r.stack_floor = strtol(argv[++i], NULL, 16);
            if (r.stack_floor < 0 || r.stack_floor >= RAM_SIZE) {
                input_error("--exclude-stack-below needs a hex RAM offset below 10000");
            }
        }
        else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            char *end;
            r.profile_from = strtol(argv[++i], &end, 10);
            if (*end != ':') input_error("expected A:B:FILE, got '%s'", argv[i]);
            char *second = end + 1;
            r.profile_to = strtol(second, &end, 10);
            if (end == second || *end != ':' || end[1] == 0 || r.profile_to <= r.profile_from) {
                input_error("expected A:B:FILE with A < B, got '%s'", argv[i]);
            }
            r.profile_path = end + 1;
        } else if (strcmp(argv[i], "--follow") == 0 && i + 1 < argc) {
            char *colon;
            /* K is a handoff number, or "boot" for a trace that starts at power-on. */
            if (strncmp(argv[i + 1], "boot:", 5) == 0) {
                r.follow_at = -2;
                r.follow_boot = 1;
                colon = argv[++i] + 4;
            } else {
                r.follow_at = strtol(argv[++i], &colon, 10);
                if (r.follow_at < 0) input_error("expected K:TRACE, got '%s'", argv[i]);
            }
            if (*colon != ':' || colon[1] == 0) {
                input_error("expected K:TRACE, got '%s'", argv[i]);
            }
            r.follow = fopen(colon + 1, "r");
            if (r.follow == NULL) input_error("cannot read %s", colon + 1);
        } else if (strcmp(argv[i], "--ram-at-pc") == 0 && i + 1 < argc) {
            char *text = argv[++i], *end, *field;
            if (r.pc_dump_count == MAX_PC_DUMPS) input_error("too many --ram-at-pc (at most %d)", MAX_PC_DUMPS);
            PcDump *d = &r.pc_dumps[r.pc_dump_count++];
            d->pc = (uint32_t)strtoul(text, &end, 16);
            if (end == text || *end != ':') input_error("--ram-at-pc needs PC:A:B:FILE, not %s", text);
            field = end + 1;
            d->from = strtol(field, &end, 10);
            if (end == field || *end != ':') input_error("--ram-at-pc needs PC:A:B:FILE, not %s", text);
            field = end + 1;
            d->to = strtol(field, &end, 10);
            if (end == field || *end != ':' || end[1] == 0 || d->to <= d->from) {
                input_error("--ram-at-pc needs PC:A:B:FILE with A below B, not %s", text);
            }
            char index_path[4096];
            snprintf(index_path, sizeof index_path, "%s.index", end + 1);
            d->f = fopen(end + 1, "wb");
            d->index = fopen(index_path, "w");
            d->previous = calloc(RAM_SIZE, 1);
            if (d->f == NULL || d->index == NULL || d->previous == NULL) input_error("cannot write %s", end + 1);
            fputs("RR2HAND1", d->f);
        } else if (strcmp(argv[i], "--scaled-check") == 0 && i + 1 < argc) {
            char *end;
            long scale = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != 0 || scale < 2 || scale > RR_SCALE_MAX) {
                input_error("--scaled-check needs a scale from 2 to %d, got '%s'", RR_SCALE_MAX, argv[i]);
            }
            r.scale = (int)scale;
        } else if (strcmp(argv[i], "--wide") == 0 && i + 1 < argc) {
            char *end;
            long margin = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != 0 || margin < 1 || margin > RR_WIDE_MARGIN_MAX) {
                input_error("--wide needs a margin from 1 to %d, got '%s'", RR_WIDE_MARGIN_MAX, argv[i]);
            }
            r.wide_margin = (int)margin;
        } else if (strcmp(argv[i], "--ground-check") == 0) {
            r.ground_check = 1;
        } else if (strcmp(argv[i], "--ground-log") == 0 && i + 1 < argc) {
            r.ground_log = fopen(argv[++i], "w");
            if (r.ground_log == NULL) input_error("cannot write %s", argv[i]);
        } else if (strcmp(argv[i], "--ground-log-every") == 0 && i + 1 < argc) {
            r.ground_every = strtoull(argv[++i], NULL, 10);
            if (r.ground_every == 0) input_error("--ground-log-every needs a count of 1 or more, got '%s'", argv[i]);
        } else if (strcmp(argv[i], "--crisp-points") == 0 && i + 1 < argc) {
            const char *value = argv[++i];
            if (strcmp(value, "fine") == 0) r.road.mode = RR_ROAD_FINE;
            else if (strcmp(value, "whole") == 0) r.road.mode = RR_ROAD_WHOLE;
            else if (strcmp(value, "none") == 0) r.points_none = 1;
            else input_error("--crisp-points needs whole, fine or none, got '%s'", value);
            r.points_given = 1;
        } else if (strcmp(argv[i], "--edge-log") == 0 && i + 1 < argc) {
            r.edge_log_path = argv[++i]; /* opened once the options are known to be usable */
        } else if (strcmp(argv[i], "--edge-log-over") == 0 && i + 1 < argc) {
            char *end;
            double pixels = strtod(argv[++i], &end);
            if (end == argv[i] || *end != 0 || pixels < 0 || pixels > 1000) {
                input_error("--edge-log-over needs a number of pixels, got '%s'", argv[i]);
            }
            r.edge_log_over = (long)(pixels * 65536.0);
        } else if (strcmp(argv[i], "--fine-objects") == 0) {
            r.fine_objects = 1;
        } else if (strcmp(argv[i], "--inbetween") == 0 && i + 1 < argc) {
            const char *value = argv[++i];
            between_mode = strcmp(value, "predict") == 0 ? RR_BETWEEN_PREDICT : strcmp(value, "blend") == 0 ? RR_BETWEEN_BLEND
                           : strcmp(value, "late") == 0 ? RR_BETWEEN_LATE : -1;
            if (between_mode < 0) input_error("--inbetween needs predict, blend or late, got '%s'", value);
        } else if (strcmp(argv[i], "--inbetween-check") == 0) {
            r.between_check = 1;
        } else if (strcmp(argv[i], "--sound-wav") == 0 && i + 1 < argc) {
            sound_wav_path = argv[++i];
        } else if (strcmp(argv[i], "--fm-log") == 0 && i + 1 < argc) {
            fm_log_path = argv[++i];
        } else if (strcmp(argv[i], "--soft-objects") == 0) {
            r.soft_objects = 1;
        } else if (strcmp(argv[i], "--dashes-check") == 0) {
            r.dashes_check = 1;
        } else if (strcmp(argv[i], "--objects-check") == 0) {
            r.objects_check = 1;
        } else if (strcmp(argv[i], "--scaled-at") == 0 && i + 1 < argc) {
            add_dump(&r, DUMP_SCALED, "--scaled-at", argv[++i]);
        } else if (strcmp(argv[i], "--ram-at") == 0 && i + 1 < argc) {
            char *colon;
            r.ram_at = strtol(argv[++i], &colon, 10);
            if (*colon != ':' || colon[1] == 0 || r.ram_at < 0) {
                input_error("expected K:FILE, got '%s'", argv[i]);
            }
            r.ram_at_path = colon + 1;
        }
        else if (strcmp(argv[i], "--exclude") == 0 && i + 1 < argc) add_exclude(&r, argv[++i]);
        else if (strcmp(argv[i], "--max-frames") == 0 && i + 1 < argc) {
            max_frames = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--speed-from-handoff") == 0 && i + 1 < argc) {
            char *end;
            r.speed_from = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != 0 || r.speed_from < 0) {
                input_error("--speed-from-handoff needs a handoff number, got '%s'", argv[i]);
            }
        } else if (strcmp(argv[i], "--track") == 0 && i + 1 < argc) {
            add_track(&r, argv[++i]);
        } else if (strcmp(argv[i], "--pad-log") == 0 && i + 1 < argc) {
            pad_log_path = argv[++i];
        } else if (strcmp(argv[i], "--inputs-by-frame") == 0 && i + 1 < argc) {
            read_frame_pads(&r, argv[++i]);
            r.by_frame = 1;
        } else if (strcmp(argv[i], "--cpu-speed") == 0 && i + 1 < argc) {
            char *end;
            cpu_speed = strtoul(argv[++i], &end, 10);
            if (end == argv[i] || *end != 0) input_error("--cpu-speed needs a number, got '%s'", argv[i]);
        } else input_error("unknown or incomplete option %s", argv[i]);
    }
    if (r.speed_from >= 0 && cpu_speed == 0) input_error("--speed-from-handoff needs --cpu-speed");
    if (rom_path == NULL || handoff_path == NULL || timestep_path == NULL || pads_path == NULL) {
        input_error("usage: rr-race-replay --rom ROM --handoffs FILE --timesteps FILE --pads FILE "
                    "[--force-timestep] [--exclude START:END:REASON]... [--max-frames N] "
                    "[--report FILE]");
    }

    FILE *f = fopen(rom_path, "rb");
    if (f == NULL) input_error("cannot open %s", rom_path);
    fseek(f, 0, SEEK_END);
    long rom_size = ftell(f);
    rewind(f);
    uint8_t *rom = malloc(rom_size > 0 ? (size_t)rom_size : 1);
    if (rom == NULL || fread(rom, 1, (size_t)rom_size, f) != (size_t)rom_size) {
        input_error("cannot read %s", rom_path);
    }
    fclose(f);
    uint32_t crc = rr_crc32(rom, (size_t)rom_size);
    if (crc != rr_code_rom_crc32) {
        input_error("ROM CRC-32 %08X does not match the translated code (%08X)", crc,
                    rr_code_rom_crc32);
    }
#ifndef RR_ANY_ROM
    if (crc != RR206_CRC32) input_error("ROM CRC-32 %08X is not RR206 (%08X)", crc, RR206_CRC32);
#endif

    r.total = count_handoffs(handoff_path);
    if (r.total == 0) input_error("%s holds no handoffs", handoff_path);
    r.timesteps = read_timesteps(timestep_path, r.total);
    r.handoffs = fopen(handoff_path, "rb");
    if (r.handoffs == NULL) input_error("cannot open %s", handoff_path);
    fseek(r.handoffs, 8, SEEK_SET);
    read_handoff(r.handoffs, r.current, 0);
    read_pads(&r, pads_path);
    if (r.resync || r.compare_regs) {
        if (regs_path == NULL) input_error("--resync and --compare-regs need --regs FILE");
        read_regs(&r, regs_path);
    }
    if (report_path != NULL) {
        r.report = fopen(report_path, "w");
        if (r.report == NULL) input_error("cannot write %s", report_path);
    }

    RrMachine *m = calloc(1, sizeof *m);
    if (m == NULL) input_error("out of memory");
    m->rom = rom;
    m->rom_size = (uint32_t)rom_size;
    m->rom_crc32 = crc;
    RrGenesis *g = rr_genesis_new(m);
    m->hw = &g->hw;
    if (sound_wav_path != NULL || fm_log_path != NULL) {
        if (r.resync) {
            input_error("--sound-wav and --fm-log cannot be used with --resync: a resynced run has no whole frames");
        }
        g->sound = rr_sound_new(m->rom, m->rom_size, g->z80_ram);
        if (g->sound == NULL) input_error("out of memory");
        if (sound_wav_path != NULL) {
            r.sound_wav = fopen(sound_wav_path, "wb");
            if (r.sound_wav == NULL) input_error("cannot write %s", sound_wav_path);
            wav_header(r.sound_wav, 0);
        }
        if (fm_log_path != NULL) {
            r.fm_log = fopen(fm_log_path, "w");
            if (r.fm_log == NULL) input_error("cannot write %s", fm_log_path);
            rr_sound_set_fm_log(g->sound, log_fm_write, r.fm_log);
        }
    }
    g->step_hook = step_hook;
    g->step_hook_ctx = &r;
    if (r.ram_writes != NULL) {
        m->ram_watch = on_ram_write;
        m->ram_watch_ctx = &r;
        m->ram_watch_start = ram_watch_start;
        m->ram_watch_end = ram_watch_end;
    }
    if (r.video_log != NULL) {
        g->video_log = on_video_write;
        g->video_log_ctx = &r;
    }
    r.m = m;
    r.g = g;
    rr_genesis_power_on(g);
    if (cpu_speed > 100000) cpu_speed = 100000;
    if (cpu_speed != 0 && r.speed_from < 0) {
        rr_genesis_set_cpu_speed(g, (unsigned)cpu_speed);
        printf("CPU_SPEED percent=%u\n", rr_genesis_cpu_speed(g));
    }

    if (offsets_path != NULL) {
        r.offset_tally = calloc(RAM_SIZE, sizeof *r.offset_tally);
        if (r.offset_tally == NULL) input_error("out of memory");
    }
    if (r.profile_path != NULL) {
        r.profile_count = calloc((size_t)rom_size / 2 + 1, sizeof *r.profile_count);
        r.profile_cycles = calloc((size_t)rom_size / 2 + 1, sizeof *r.profile_cycles);
        if (r.profile_count == NULL || r.profile_cycles == NULL) input_error("out of memory");
    }

    if (pad_log_path != NULL) {
        r.pad_log = fopen(pad_log_path, "w");
        if (r.pad_log == NULL) input_error("cannot write %s", pad_log_path);
    }
    if (r.resync && r.pc_dump_count > 0) {
        input_error("--ram-at-pc cannot be used with --resync: a resynced run is not the main loop's own work");
    }
    if (r.resync && r.dump_count > 0) {
        /* A resync moves the clock, which ends the frame in progress early: the
         * picture would be half drawn and the frame numbers would not be the
         * free run's. */
        static const char *const names[] = {"--picture-at", "--sources-at", "--line-state-at",
                                            "--video-state-at", "--scaled-at"};
        input_error("%s cannot be used with --resync: a resynced run has no whole frames",
                    names[r.dumps[0].kind]);
    }
    if (r.resync && r.scale != 0) {
        input_error("--scaled-check cannot be used with --resync: a resynced run has no whole frames");
    }
    for (int i = 0; i < r.dump_count; i++) {
        if (r.dumps[i].kind == DUMP_SCALED && r.scale == 0) input_error("--scaled-at needs --scaled-check S");
    }
    if (r.resync && r.dashes_check) {
        input_error("--dashes-check cannot be used with --resync: a resynced pass is not the game's own");
    }
    if (r.resync && r.objects_check) {
        input_error("--objects-check cannot be used with --resync: a resynced pass is not the game's own");
    }
    if (r.objects_check) {
        r.object_set = calloc(1, sizeof *r.object_set);
        r.object_work = calloc(RAM_SIZE, 1);
        if (r.object_set == NULL || r.object_work == NULL) input_error("out of memory");
    }
    if (r.fine_objects && r.scale == 0) input_error("--fine-objects needs --scaled-check S");
    if (r.soft_objects && !r.fine_objects) input_error("--soft-objects needs --fine-objects");
    if (r.fine_objects) {
        r.objects = calloc(1, sizeof *r.objects);
        r.covered = calloc(RR_SCREEN_W * RR_SCREEN_H, 1);
        r.strips = calloc(1, sizeof *r.strips);
        if (r.objects == NULL || r.covered == NULL || r.strips == NULL) input_error("out of memory");
        rr_object_tracker_reset(r.objects);
    }
    if (r.between_check && between_mode == RR_BETWEEN_OFF) input_error("--inbetween-check needs --inbetween MODE");
    if (between_mode != RR_BETWEEN_OFF) {
        if (r.scale == 0) input_error("--inbetween needs --scaled-check S");
        if (!r.fine_objects) input_error("--inbetween needs --fine-objects");
        if (r.points_none || r.road.mode != RR_ROAD_FINE) input_error("--inbetween needs --crisp-points fine (the default)");
        r.between = calloc(1, sizeof *r.between);
        if (r.between == NULL) input_error("out of memory");
        rr_between_reset(r.between);
        rr_between_set_mode(r.between, between_mode);
        if (r.between_check) {
            r.check = calloc(1, sizeof *r.check);
            if (r.check == NULL) input_error("out of memory");
            r.check->first_difference = -1;
        }
    }
    if (r.points_given && r.scale == 0) input_error("--crisp-points needs --scaled-check S");
    if (r.ground_check && r.scale == 0) input_error("--ground-check needs --scaled-check S");
    if (r.wide_margin != 0 && r.scale == 0) input_error("--wide needs --scaled-check S");
    if (r.wide_margin != 0 && !r.fine_objects) input_error("--wide needs --fine-objects");
    if (r.ground_every == 0) r.ground_every = 499;
    if (r.ground_log != NULL && !r.ground_check) input_error("--ground-log needs --ground-check");
    if (r.edge_log_path != NULL && r.scale == 0) input_error("--edge-log needs --scaled-check S");
    if (r.edge_log_over >= 0 && r.edge_log_path == NULL) input_error("--edge-log-over needs --edge-log FILE");
    if (r.scale != 0) {
        r.scaled = calloc((size_t)RR_SCREEN_W * RR_SCREEN_H * (size_t)r.scale * (size_t)r.scale, sizeof *r.scaled);
        if (r.scaled == NULL) input_error("out of memory");
        rr_road_tracker_reset(&r.road);
        g->hires = r.scaled;
        g->hires_scale = r.scale;
        g->hires_begin = choose_road;
        g->hires_ctx = &r;
        if (r.ground_check) g->hires_line = measure_ground_line;
        if (r.wide_margin != 0) {
            size_t pixels = (size_t)(RR_SCREEN_W + 2 * r.wide_margin) * RR_SCREEN_H * (size_t)r.scale * (size_t)r.scale;
            r.wide_px = calloc(pixels, sizeof *r.wide_px);
            r.wide_src = calloc(pixels, sizeof *r.wide_src);
            r.wide_cover = calloc((size_t)RR_SCREEN_W * RR_SCREEN_H * (size_t)r.scale * (size_t)r.scale, 1);
            r.margin_set = calloc(1, sizeof *r.margin_set);
            r.margin_table = calloc(RR_SPRITES_MAX, 8);
            r.margin_work = calloc(RAM_SIZE, 1);
            if (r.wide_px == NULL || r.wide_src == NULL || r.wide_cover == NULL || r.margin_set == NULL
                || r.margin_table == NULL || r.margin_work == NULL)
                input_error("out of memory");
            r.wide.margin = r.wide_margin;
            r.wide.panel = 0x000000;
            rr_wide_blocks(r.wide.block);
            rr_wide_dashboard(&r.wide);
            r.wide.ground_backdrop = RR_WIDE_ROAD_BACKDROP;
            r.wide_first = -1;
            /* The wide picture is the one drawn; `scaled` receives the same frame without
             * margins, so every check of the narrow picture is made as before. */
            g->hires = r.wide_px;
            g->hires_narrow = r.scaled;
            g->hires_sources = r.wide_src;
            g->hires_wide = &r.wide;
            g->hires_line_drawn = measure_wide_line;
        }
    }
    if (r.edge_log_path != NULL) {
        r.edge_log = fopen(r.edge_log_path, "w");
        if (r.edge_log == NULL) input_error("cannot write %s", r.edge_log_path);
    }
    clock_t started = clock();
    for (int i = 0; i < r.dump_count; i++) {
        /* The layer map and the per-line state are only kept when asked for. */
        if (r.dumps[i].kind == DUMP_SOURCES && g->sources == NULL) {
            g->sources = calloc(RR_SCREEN_W * RR_SCREEN_H, sizeof *g->sources);
            if (g->sources == NULL) input_error("out of memory");
        }
        if (r.dumps[i].kind == DUMP_LINE_STATE && g->line_state == NULL) {
            g->line_state = calloc(RR_SCREEN_H, sizeof *g->line_state);
            if (g->line_state == NULL) input_error("out of memory");
        }
    }
    while (!r.finished && r.frames < max_frames) {
        rr_genesis_run_frame(g);
        if (g->sound != NULL) take_sound(&r, g);
        if (r.scale != 0) {
            r.scaled_frames++;
            r.sampled_back += (unsigned long)samples_back(&r, g);
            r.off_road += (unsigned long)off_road_is_plain(&r, g);
            if (r.fine_objects) measure_objects(&r, g);
            if (r.wide_margin != 0) measure_wide_frame(&r, g);
        }
        write_dumps(&r, g);
        r.frames++;
        if (r.speed_due) { /* between frames, as the hardware model asks */
            r.speed_due = 0;
            rr_genesis_set_cpu_speed(g, (unsigned)cpu_speed);
            printf("CPU_SPEED percent=%u from_handoff=%ld\n", rr_genesis_cpu_speed(g), r.speed_from);
        }
    }
    if (r.scale != 0) {
        double ms = (double)(clock() - started) * 1000.0 / CLOCKS_PER_SEC;
        printf("SCALED scale=%d points=%s frames=%lu off_road_identical=%lu sampled_back_identical=%lu "
               "crisp_frames=%llu fine_frames=%llu race_frames=%llu edge_lines=%llu within_1px=%llu "
               "within_2px_and_a_line=%llu over_2px=%llu over_4px=%llu over_8px=%llu max_edge_difference=%ld "
               "run_ms_per_frame=%.3f\n",
               r.scale, r.points_none ? "none" : r.road.mode == RR_ROAD_FINE ? "fine" : "whole", r.scaled_frames,
               r.off_road,
               r.sampled_back, (unsigned long long)r.road.crisp_frames,
               (unsigned long long)r.road.fine_frames, (unsigned long long)r.road.race_frames,
               r.edge_lines, r.edge_within_1px, r.edge_within_bound, r.edge_over[0], r.edge_over[1],
               r.edge_over[2], r.edge_max,
               r.scaled_frames ? ms / (double)r.scaled_frames : 0.0);
        if (r.ground_check) {
            printf("GROUND ground_lines=%llu block_rows=%llu full_rows=%llu empty_rows=%llu other_ground_rows=%llu "
                   "sky_lines=%llu sky_rows_passing=%llu mark_max=%d period_max=%d "
                   "ground_lines_sky_backdrop=%llu sky_rows_passing_road_backdrop=%llu\n",
                   r.ground_lines, r.ground_block, r.ground_full, r.ground_empty, r.ground_other, r.sky_lines,
                   r.sky_passing, r.ground_mark_max, r.ground_period_max, r.ground_sky_backdrop,
                   r.sky_passing_road);
            if (r.ground_log != NULL) fclose(r.ground_log);
        }
        if (r.wide_margin != 0) {
            printf("WIDE_CENTRE frames=%lu identical=%lu pixels=%llu differing_pixels=%llu road_line=%llu object=%llu "
                   "dash=%llu other=%llu first_frame=",
                   r.wide_frames, r.wide_identical, (unsigned long long)r.wide_centre.pixels,
                   (unsigned long long)r.wide_centre.differing, (unsigned long long)r.wide_centre.road_line,
                   (unsigned long long)r.wide_centre.object, (unsigned long long)r.wide_centre.dash,
                   (unsigned long long)r.wide_centre.other);
            if (r.wide_first < 0) puts("none");
            else printf("%ld\n", r.wide_first);
            printf("WIDE_GROUND examined=%llu grass=%llu mark=%llu road=%llu wrong=%llu short_lines=%llu "
                   "short_updates=%lu beyond_lines=%llu no_edge_lines=%llu\n",
                   (unsigned long long)r.wide_ground.examined, (unsigned long long)r.wide_ground.grass,
                   (unsigned long long)r.wide_ground.mark, (unsigned long long)r.wide_ground.road,
                   (unsigned long long)r.wide_ground.wrong, (unsigned long long)r.wide_ground.short_lines,
                   r.wide_short_updates, (unsigned long long)r.wide_ground.beyond_lines, r.wide_no_edge);
            printf("WIDE_SKY examined=%llu plane=%llu backdrop=%llu panel=%llu untracked_race_frames=%lu\n",
                   (unsigned long long)r.wide_sky.examined, (unsigned long long)r.wide_sky.plane,
                   (unsigned long long)r.wide_sky.backdrop, (unsigned long long)r.wide_sky.panel, r.wide_untracked);
            printf("WIDE_OBJECTS frames=%lu margin_pictures=%llu uncut=%llu off_view=%llu copies=%llu "
                   "left_out_room=%llu seam_pixels=%llu differing_outside_seam=%llu gaps=%llu\n",
                   r.margin_frames, r.margin_pictures, r.margin_uncut, r.margin_off_view, r.margin_copies,
                   r.margin_left_out, r.seam_pixels, r.outside_seam, r.margin_gaps);
        }
    }
    if (r.fine_objects) {
        printf("FINE_OBJECTS frames=%lu set_frames=%llu fine_frames=%llu stale_lines=%llu entries=%llu "
               "fine_entries=%llu covered_pixels=%llu same_at_centre=%llu\n",
               r.scaled_frames, (unsigned long long)r.objects->frames, (unsigned long long)r.objects->fine_frames,
               (unsigned long long)g->hires_sprites_stale_lines, r.fine_entries_all, r.fine_entries,
               r.covered_pixels, r.same_at_centre);
    }
    if (r.fine_objects) {
        printf("FINE_DASHES frames=%lu strip_frames=%lu sprite_frames=%lu\n", r.dash_frames, r.dash_strip_frames,
               r.dash_frames - r.dash_strip_frames);
    }
    if (r.between != NULL) {
        static const char *const modes[] = {"off", "predict", "blend", "late"};
        const RrBetween *b = r.between;
        printf("INBETWEEN mode=%s frames=%llu between=%llu guessed=%llu no_pair=%llu road=%llu objects=%llu "
               "table=%llu returned=%llu stale_lines=%llu\n",
               modes[b->mode], (unsigned long long)b->frames, (unsigned long long)b->between_frames,
               (unsigned long long)b->guessed_frames, (unsigned long long)b->fell_back[RR_BETWEEN_NO_PAIR],
               (unsigned long long)b->fell_back[RR_BETWEEN_ROAD], (unsigned long long)b->fell_back[RR_BETWEEN_OBJECTS],
               (unsigned long long)b->fell_back[RR_BETWEEN_TABLE], (unsigned long long)b->returned,
               (unsigned long long)g->hires_sprites_stale_lines);
    }
    if (r.check != NULL) {
        BetweenCheck *c = r.check;
        printf("INBETWEEN_ENDS updates=%lu made=%lu not_made_road=%lu not_made_objects=%lu not_made_table=%lu "
               "road_at_0=%lu road_at_1=%lu objects_at_0=%lu objects_at_1=%lu table_at_1=%lu first_difference=",
               c->ends, c->ends_made, c->ends_unmade[RR_BETWEEN_ROAD], c->ends_unmade[RR_BETWEEN_OBJECTS],
               c->ends_unmade[RR_BETWEEN_TABLE], c->road_at_0, c->road_at_1, c->objects_at_0, c->objects_at_1,
               c->table_at_1);
        if (c->first_difference < 0) puts("none");
        else printf("%ld\n", c->first_difference);
        printf("INBETWEEN_SKIP_ONE triples=%lu edge_samples=%zu edge_median=%.2f edge_p90=%.2f edge_p99=%.2f "
               "object_samples=%zu object_median=%.2f object_p90=%.2f object_p99=%.2f\n",
               c->triples, c->skip_edge.count, percentile(&c->skip_edge, 0.5), percentile(&c->skip_edge, 0.9),
               percentile(&c->skip_edge, 0.99), c->skip_object.count, percentile(&c->skip_object, 0.5),
               percentile(&c->skip_object, 0.9), percentile(&c->skip_object, 0.99));
        printf("INBETWEEN_MOTION edge_still=%lu edge_moving=%lu edge_largest_step=%.2f object_still=%lu "
               "object_moving=%lu today_still=%lu today_moving=%lu dash_still=%lu dash_moving=%lu\n",
               c->edge_still, c->edge_moving, c->largest_step, c->object_still, c->object_moving, c->today_still,
               c->today_moving, c->dash_still, c->dash_moving);
        printf("INBETWEEN_GUESS updates=%lu edge_samples=%zu edge_median=%.2f edge_p90=%.2f edge_p99=%.2f "
               "object_samples=%zu object_median=%.2f object_p90=%.2f object_p99=%.2f\n",
               c->guesses, c->guess_edge.count, percentile(&c->guess_edge, 0.5), percentile(&c->guess_edge, 0.9),
               percentile(&c->guess_edge, 0.99), c->guess_object.count, percentile(&c->guess_object, 0.5),
               percentile(&c->guess_object, 0.9), percentile(&c->guess_object, 0.99));
    }
    if (r.dashes_check) {
        printf("DASHES passes=%lu covered=%lu identical=%lu first_difference=", r.dash_passes, r.dash_covered,
               r.dash_identical);
        if (r.dash_first_difference < 0) fputs("none", stdout);
        else printf("%ld", r.dash_first_difference);
        printf(" sprites_after_objects=%lu dash_sprites=%lu dash_colours=", r.dash_after, r.dash_sprites);
        for (int n = 1, any = 0; n < 16; n++)
            if (r.dash_colours & (1u << n)) printf(any++ ? ",%d" : "%d", n);
        putchar('\n');
    }
    if (r.objects_check) {
        printf("OBJECTS passes=%lu identical=%lu records=%lu dropped=%lu pictures=%lu first_difference=",
               r.object_passes, r.object_identical, r.object_records, r.object_dropped, r.object_pictures);
        if (r.object_first_difference < 0) puts("none");
        else printf("%ld\n", r.object_first_difference);
    }
    if (r.edge_log != NULL && fclose(r.edge_log) != 0) input_error("cannot write the edge log");
    if (r.report != NULL) fclose(r.report);
    if (r.pad_log != NULL) fclose(r.pad_log);
    if (r.video_log != NULL && fclose(r.video_log) != 0) input_error("cannot write the video log");
    for (int i = 0; i < r.pc_dump_count; i++) {
        if (fclose(r.pc_dumps[i].f) != 0 || fclose(r.pc_dumps[i].index) != 0) input_error("cannot write a --ram-at-pc file");
    }
    if (r.ram_writes != NULL && fclose(r.ram_writes) != 0) input_error("cannot write the RAM write log");
    for (int i = 0; i < r.dump_count; i++) {
        if (!r.dumps[i].done) {
            input_error("frame %lu was not reached (the run ended after %lu frames): %s not written",
                        r.dumps[i].frame, r.frames, r.dumps[i].path);
        }
    }
    if (offsets_path != NULL) {
        FILE *out = fopen(offsets_path, "w");
        if (out == NULL) input_error("cannot write %s", offsets_path);
        for (unsigned i = 0; i < RAM_SIZE; i++) {
            if (r.offset_tally[i] != 0) fprintf(out, "%04X %u\n", i, r.offset_tally[i]);
        }
        fclose(out);
    }
    if (r.profile_path != NULL) {
        FILE *out = fopen(r.profile_path, "w");
        if (out == NULL) input_error("cannot write %s", r.profile_path);
        for (long i = 0; i <= rom_size / 2; i++) {
            if (r.profile_count[i] != 0) {
                fprintf(out, "%06lX %u %u\n", i * 2, r.profile_count[i], r.profile_cycles[i]);
            }
        }
        fclose(out);
    }

    if (r.by_frame) {
        printf("FREE_RUN handoffs=%lu frames=%lu\n", r.k, r.frames);
        return 0;
    }
    printf("HANDOFFS compared=%lu of %lu\n", r.k, r.total);
    printf("TIMESTEP same=%lu different=%lu forced=%d\n", r.timestep_same, r.timestep_different,
           r.force_timestep);
    if (r.compare_regs) {
        printf("REGS identical=%lu differing=%lu\n", r.regs_identical, r.regs_differing);
    }
    printf("PADS native_reads=%lu recorded=%lu repeated=%lu dropped=%lu\n", r.pad_reads,
           r.pad_count, r.pad_repeated, r.pad_dropped);
    if (r.first_differing < 0) {
        printf("RAM identical=%lu differing=%lu first_differing=none\n", r.ram_identical,
               r.ram_differing);
    } else {
        printf("RAM identical=%lu differing=%lu first_differing=%ld offsets=", r.ram_identical,
               r.ram_differing, r.first_differing);
        for (int i = 0; i < r.first_count; i++) printf("%s%04X", i ? "," : "", r.first_offsets[i]);
        putchar('\n');
    }
    if (r.stack_floor >= 0) {
        printf("EXCLUDED start=%04lX end=stack-pointer bytes_hidden=%llu reason=dead stack\n",
               (unsigned long)r.stack_floor, r.stack_hidden);
    }
    for (int i = 0; i < r.exclude_count; i++) {
        printf("EXCLUDED start=%04X end=%04X bytes_hidden=%llu reason=%s\n", r.excludes[i].start,
               r.excludes[i].end, r.excludes[i].hidden, r.excludes[i].reason);
    }
    if (!r.finished) {
        fprintf(stderr, "rr-race-replay: the native run reached %lu of %lu handoffs in %lu frames\n",
                r.k, r.total, r.frames);
        return 1;
    }
    if (g->sound != NULL) {
        take_sound(&r, g);
        if (r.sound_wav != NULL) {
            wav_header(r.sound_wav, r.sound_samples);
            fclose(r.sound_wav);
        }
        if (r.fm_log != NULL) {
            /* When the run ended, so a comparison knows how far this log reaches. */
            fprintf(r.fm_log, "END %llu\n", (unsigned long long)rr_sound_now(g->sound));
            fclose(r.fm_log);
        }
        printf("SOUND z80_steps=%llu fm_writes=%llu fm_writes_68000=%llu psg_writes_z80=%llu "
               "psg_writes_68000=%llu z80_ram_writes_68000=%llu samples=%lu\n",
               (unsigned long long)rr_sound_z80_steps(g->sound),
               (unsigned long long)rr_sound_fm_writes(g->sound),
               (unsigned long long)rr_sound_fm_writes_68000(g->sound),
               (unsigned long long)rr_sound_psg_writes(g->sound),
               (unsigned long long)g->psg_writes, (unsigned long long)g->z80_ram_writes,
               r.sound_samples);
    }
    if (r.follow_differed || r.ram_differing != 0 ||
        (r.timestep_different != 0 && !r.force_timestep)) {
        puts("RESULT differ");
        return 1;
    }
    puts(r.timestep_different != 0 ? "RESULT logic-match" : "RESULT match");
    return 0;
}
