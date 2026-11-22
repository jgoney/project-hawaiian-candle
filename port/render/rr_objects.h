/* The game's drawing of objects into sprites, repeated outside the game.
 *
 * Every rider, car, tree and sign is one draw record. Each pass the game
 * scales each record's picture into a buffer of tiles and adds the sprites
 * that show it to the sprite list (0xA4EE; analysis/race-picture.md, section
 * 3.3). rr_objects_draw repeats that from work RAM as it is just before, step
 * for step with the 68000 code, so that its result can be compared with the
 * game's for equality. What it adds is the description the game throws away:
 * which picture each sprite shows and how that picture is placed, scaled and
 * cut, so a renderer can draw the picture itself at a finer resolution.
 *
 * The reference is src/rr2port/objects.py, which was measured against the
 * game first; this follows it routine for routine.
 */
#ifndef RR_OBJECTS_H
#define RR_OBJECTS_H

#include <stddef.h>
#include <stdint.h>

#include "rr_vdp.h"

enum {
    RR_OBJECT_TILES = 0x150,     /* tiles in the game's sprite-picture buffer */
    RR_OBJECTS_NOTE_PC = 0xA4EE, /* the game is about to draw the objects */
    RR_OBJECTS_DONE_PC = 0xA5C6  /* it has drawn them */
};

/* What became of a draw record. */
enum {
    RR_RECORD_DRAWN,      /* it has sprites */
    RR_RECORD_OFF_VIEW,   /* wholly outside the view, to a side or above */
    RR_RECORD_NO_ROOM,    /* no sprites or tiles left for it */
    RR_RECORD_NO_PICTURE, /* nothing to draw: a picture the game makes nothing of */
    RR_RECORD_HIDDEN      /* wholly below the line a hill hides it under */
};
/* The most record slots the drawing keeps an outcome for. The game's table
 * (0xFFCEB8, 22 bytes a record) has room for 46 before the next thing in
 * memory at 0xFFD2B8; the draw order can name any slot, and a slot from
 * here up is drawn as ever but its outcome is not kept. */
enum { RR_OBJECT_RECORDS = 96 };

typedef struct {
    int ok;                    /* 0: the image is not one this covers; nothing below is set */
    int first_sprite, sprites; /* the objects' sprite-list entries are [first_sprite, sprites) */
    int first_tile, tiles;     /* their tiles in the buffer are [first_tile, tiles) */
    uint16_t tile_base;        /* 0xFFCE86: the video-memory tile number of buffer tile 0 */
    int pictures;
    RrFinePicture picture[RR_SPRITES_MAX];
    uint16_t picture_record[RR_SPRITES_MAX];   /* the draw record (its slot) each picture is of */
    RrFineSprites fine;        /* fine.pictures points at `picture` */
    uint8_t list[RR_SPRITES_MAX * 8];          /* the sprite list as the game stages it */
    uint8_t tile_bytes[RR_OBJECT_TILES * 32];  /* the picture buffer as the game stages it */
    unsigned records, dropped;
    /* What became of each draw record, by its slot (RR_RECORD_*). Only the
     * slots in the draw order are written; the rest stay 0. */
    uint8_t outcome[RR_OBJECT_RECORDS];
    /* Set by rr_objects_margin only: pictures with an entry in the margin
     * table, and of those the ones that are a picture of the narrow set
     * uncut, the ones the narrow view does not show at all, the copies the
     * game did not make, and the records left out for want of sprites. */
    unsigned margin_pictures, margin_uncut, margin_off_view, margin_copies, margin_left_out;
} RrObjectSet;

/* `ram` is work RAM (0x10000 bytes) when the main loop is at
 * RR_OBJECTS_NOTE_PC. `work` (0x10000 bytes) receives RAM as the game's
 * drawing leaves it. Returns out->ok. Nothing outside `work` and `out` is
 * written, and nothing outside `ram` and `rom` is read, whatever they hold. */
int rr_objects_draw(const uint8_t *ram, const uint8_t *rom, size_t rom_size, RrObjectSet *out,
                    uint8_t *work);

/* The same drawing for another view: pictures are cut to x in [left, right)
 * instead of the screen's [0, 0x140). The game stops a picture it cuts at
 * its right edge four pixels short of that edge; that belongs to the game's
 * view only, and with any other view the cut is at `right` exactly.
 * With `tiles` 0 there is no tile budget and nothing is written to the
 * picture buffer (a fine picture needs no tiles; the tile numbers in the
 * sprite list then mean nothing); the list still holds 80 sprites at most.
 * With the view {0, 0x140} and `tiles` 1 this is rr_objects_draw. */
typedef struct { int16_t left, right; } RrObjectView;
int rr_objects_draw_view(const uint8_t *ram, const uint8_t *rom, size_t rom_size,
                         const RrObjectView *view, int tiles, RrObjectSet *out, uint8_t *work);

/* The objects of the margins of a picture `margin` pixels wider on either
 * side. `narrow` is the set drawn with the game's view from `narrow_ram`;
 * `wide_ram` holds the records to draw wide (the same image; or a copy with
 * more roadside records). The records are drawn once more with a view one
 * pixel wider than [-margin, 320 + margin) and no tile budget into `out`, and what the
 * narrow picture does not already show is put in `table` (RR_SPRITES_MAX * 8
 * bytes: a sprite table of its own, linked from entry 0) and described in
 * `fine` (fine->table is `table`, fine->pictures is out->picture) for the
 * renderer's margin layer, which draws only beyond the screen's sides:
 *   - a record the narrow set drew whose picture is cut at a side of the
 *     screen (it reaches x = 0, or x = 315 or beyond): that same picture
 *     uncut, standing where the narrow one stands, so that both show the
 *     same column of the art on every line;
 *   - a picture the narrow view does not show at all (the record is off
 *     the narrow view, or is only in `wide_ram`, or it is one part of a
 *     two-part record whose other part the game drew);
 *   - a copy of an object that the game did not make because it stands
 *     across the screen's side, or because the game makes none for a
 *     picture it cut.
 * Left out: records the narrow set dropped for want of room, made nothing
 * of, or had hidden by a hill; pictures whose top line is at `bottom` or
 * below (the riders in the mirrors); entries that do not reach a margin
 * (or the screen's last five columns, where the game stops short) and
 * entries without a picture. Returns out->ok: 0 if `narrow` is not ok or
 * the image is not one the drawing covers. `work` is 0x10000 bytes. */
int rr_objects_margin(const RrObjectSet *narrow, const uint8_t *narrow_ram, const uint8_t *wide_ram,
                      const uint8_t *rom, size_t rom_size, int margin, int bottom,
                      RrObjectSet *out, uint8_t *table, RrFineSprites *fine, uint8_t *work);

/* For checking a margin set: the screen columns, beyond the screen's sides
 * or in its last five, on which a margin picture stands but no entry of
 * the table shows it (counted over the pictures that have an entry at all;
 * copies are not looked at). 0 for a set made by rr_objects_margin. */
unsigned rr_objects_margin_gaps(const RrObjectSet *out, const uint8_t *table, const RrFineSprites *fine,
                                int margin);

/* Which object set is on screen. The game shows a pass's objects a frame or
 * more after drawing them (it sends the tiles, then the sprite list, in the
 * blank lines of later frames), so the sets are noted as the game is about to
 * draw them and the newest one that is in video memory is used. A set is in
 * video memory when every one of its sprite entries (all bytes but the link,
 * which the game changes when it adds more sprites after the objects) and
 * every one of its tiles are there byte for byte. If none is, nothing is
 * returned and the sprites are drawn from their tiles: never a picture for a
 * sprite that is not the one described. The set chosen for a frame stays as
 * it is until the next choice: a set noted meanwhile takes the oldest of the
 * other places, because the frame is still being drawn from the chosen one. */
enum { RR_OBJECT_RECENT = 4 };
typedef struct {
    RrObjectSet recent[RR_OBJECT_RECENT];
    uint64_t age[RR_OBJECT_RECENT]; /* when each was noted (larger is newer); 0 for an empty slot */
    uint64_t clock;
    int chosen;                 /* index in `recent` of the set last chosen, or -1 */
    uint8_t work[0x10000];
    uint8_t ram[RR_OBJECT_RECENT][0x10000]; /* work RAM as each was noted from */
    uint64_t noted;             /* sets kept */
    uint64_t frames;            /* calls of choose with a usable set noted */
    uint64_t fine_frames;       /* those that found one in video memory */
    /* For a wider picture: with `margin` above 0 (set by the owner) the
     * objects of the margins are made beside each set as it is noted, once
     * an update, from the same image (rr_objects_margin). */
    int margin;
    RrObjectSet margin_set[RR_OBJECT_RECENT];
    uint8_t margin_table[RR_OBJECT_RECENT][RR_SPRITES_MAX * 8];
    RrFineSprites margin_fine[RR_OBJECT_RECENT];
    int margin_of[RR_OBJECT_RECENT]; /* the margin each was made for; 0: none */
    uint64_t margins_made;
} RrObjectTracker;

void rr_object_tracker_reset(RrObjectTracker *t);
/* Call when the main loop reaches RR_OBJECTS_NOTE_PC. An image the model does
 * not cover is not kept. */
void rr_object_tracker_note(RrObjectTracker *t, const uint8_t *ram, const uint8_t *rom, size_t rom_size);
/* Call before a frame's first line. */
const RrFineSprites *rr_object_tracker_choose(RrObjectTracker *t, const RrVdp *v);
/* The set last chosen, or NULL. */
const RrObjectSet *rr_object_tracker_chosen(const RrObjectTracker *t);
/* The margin objects that go with the set last chosen, if they were made
 * for this margin; else NULL. */
const RrFineSprites *rr_object_tracker_chosen_margin(const RrObjectTracker *t, int margin);
/* The work RAM the set last chosen was noted from, or NULL. */
const uint8_t *rr_object_tracker_chosen_ram(const RrObjectTracker *t);

#endif
