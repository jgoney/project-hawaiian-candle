"""Object pictures, and the game's drawing of them into sprites, repeated in Python.

Every rider, car, tree and sign is one **draw record** (rr2port.scene). Each
pass the game scales each record's picture in software into a buffer of
8x8-pixel tiles and adds the sprites that show it to the sprite list
(analysis/race-picture.md, section 3.3). `draw_objects` repeats that from a
work-RAM image taken just before the game does it (at 0xA4EE), so the result
can be compared with what the game produced (rr2-object-check). The
arithmetic follows the 68000 code (0xA4EE to 0xA99E and 0x7A68) step by
step, including its rounding and its oddities, because the check is for
equality.

A picture in the ROM is a width byte, a height byte, a colour-set word and
then rows of ceil(width / 2) + 1 bytes, two pixels to a byte (the last byte
of a row is spare). Pictures drawn with an anchor point have two more bytes
in front: the anchor's distance from the left edge and from the top.
"""

from __future__ import annotations

import struct
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

PICTURE_TABLE = 0x56A0A  # ROM: pointers to object pictures
SPRITE_SHAPES = 0x7C58  # ROM: per sprite shape, height in pixels, size bits, tiles, width
COPY_SPACING = 0x16E2C  # ROM: irregular spacing for copies of an object
PERSPECTIVE_TABLE = 0x27C84
DRAW_RECORDS = 0xCEB8
DRAW_RECORD_SIZE = 22
DRAW_ORDER = 0x08D6
RECORD_DISTANCES = 0x06D6  # per record: distance from the camera, a long
MAX_SPRITES = 0x50
MAX_TILES = 0x150
TILE_BYTES = 32
VIEW = (0, 0x140, 0, 0x98)  # left, right, top, bottom of the one-player 3D view


@dataclass(frozen=True)
class Picture:
    index: int
    width: int
    height: int
    anchor_x: int | None  # None for a picture without an anchor point
    anchor_y: int | None
    colour_set: int  # the picture's own; the game uses the draw record's instead
    rows: tuple[bytes, ...]  # colour numbers 0..15, 0 = see-through


@dataclass(frozen=True)
class StagedSprite:
    """One entry of the sprite list as the game stages it, in screen terms."""

    x: int
    y: int
    cells_wide: int
    cells_high: int
    tile: int
    palette: int
    priority: bool
    mirrored: bool
    upside_down: bool
    link: int  # index of the next sprite to draw, 0 at the end


@dataclass(frozen=True)
class Placement:
    """Where a draw record puts its object: the fields the record makers compute."""

    x: int
    foot_line: int
    size: int
    hidden_below: int
    offset: int | None = None
    distance: int | None = None


@dataclass
class Drawn:
    sprites: list[StagedSprite]
    pictures: bytes  # the whole sprite-picture buffer after the drawing
    ram: bytes  # the work-RAM image with the drawing applied
    cases: dict = field(default_factory=dict)  # how many records were of each kind


def _s16(value: int) -> int:
    value &= 0xFFFF
    return value - 0x10000 if value & 0x8000 else value


def _s8(value: int) -> int:
    value &= 0xFF
    return value - 0x100 if value & 0x80 else value


def _negative(value: int) -> bool:
    """The 68000's N flag for a word result (a signed compare that ignores overflow)."""
    return bool(value & 0x8000)


def _scaled(value: int, size: int) -> int:
    """value x size, size being the record's size x 4 (0x10000 = natural), as the game does."""
    return ((((value & 0xFFFF) * ((size >> 2) & 0xFFFF)) << 2) >> 16) & 0xFFFF


def _picture_address(rom: bytes, index: int) -> int:
    return struct.unpack_from(">I", rom, PICTURE_TABLE + 4 * index)[0]


def read_picture(rom: bytes, index: int, anchored: bool) -> Picture:
    """Picture `index` of the ROM's table. `anchored` is the draw record's flag bit 7."""
    at = _picture_address(rom, index)
    anchor_x = anchor_y = None
    if anchored:
        anchor_x, anchor_y = _s8(rom[at]), _s8(rom[at + 1])
        at += 2
    width, height = rom[at], rom[at + 1]
    colour_word = struct.unpack_from(">H", rom, at + 2)[0]
    stride = (width + 1) // 2 + 1
    rows = []
    for r in range(height):
        data = rom[at + 4 + r * stride : at + 4 + (r + 1) * stride - 1]
        pixels = bytearray()
        for byte in data:
            pixels += bytes((byte >> 4, byte & 0x0F))
        rows.append(bytes(pixels[:width]))
    return Picture(index, width, height, anchor_x, anchor_y, (colour_word >> 13) & 3, tuple(rows))


def _column_steps(width: int, size: int, clip: int) -> tuple[list[int], int, int, int]:
    """How far to move through a source row after each output byte (0xA21C).

    A byte is two pixels: pictures are scaled sideways in pixel pairs. `clip`
    is 0, or positive to keep only that many output bytes, or negative to
    drop that many from the start. Returns the steps, the number of output
    bytes, the number of 8-pixel columns less one (0xFFFF if there is nothing
    to draw) and the row length in the ROM.
    """
    steps: list[int] = []
    total = pending = made = 0
    if clip == 0:
        skip, keep = 0, 0x7FFF
    elif _negative(clip):
        skip, keep = _s16(-clip), 0x7FFF
    else:
        skip, keep = 0, _s16(clip)
    left = (width >> 1) + (width & 1) + 1
    stride = left
    left -= 2
    stopped = False
    while True:
        pending += 1
        total += size
        while total >= 0x10000:
            skip -= 1
            if skip < 0:
                steps.append(pending)
                pending = 0
                made += 1
                keep -= 1
                if keep < 0:
                    pending = left & 0xFFFF
                    stopped = True
                    break
            total -= 0x10000
        if stopped:
            break
        left -= 1
        if left == -1:
            break
    produced = made
    steps.append(pending)
    if made & 3 != 3:
        for _ in range(3 - (made & 3)):
            steps.append(0)
            made += 1
    if made == 0:
        return steps, produced, 0xFFFF, stride
    return steps, produced, ((((made + 1) & 0xFFFF) >> 2) - 1) & 0xFFFF, stride


def _row_steps(height: int, stride: int, size: int, skip: int) -> list[int]:
    """How far to move through the picture after each output row (0xA2CA)."""
    steps: list[int] = []
    total = 0
    pending = 1
    left = max(height - 1, 0)
    skip = _s16(skip)
    while True:
        if skip > 0:
            skip -= 1
            pending += stride
        else:
            total += size
            if total < 0x10000:
                pending += stride
            else:
                while True:
                    steps.append(pending)
                    pending = 1
                    total -= 0x10000
                    if total < 0x10000:
                        break
                    steps[-1] -= stride
        left -= 1
        if left == -1:
            break
    return steps


def scale_picture(picture: Picture, size: int) -> tuple[bytes, ...]:
    """The picture at a draw record's size (0x4000 = natural), unclipped, as colour numbers."""
    size = max(size << 2, 4)
    stride = (picture.width + 1) // 2 + 1
    source = bytearray()
    for row in picture.rows:
        padded = row + b"\0" * (2 * stride - len(row))
        source += bytes(padded[i] << 4 | padded[i + 1] for i in range(0, 2 * stride, 2))
    columns, produced, _, _ = _column_steps(picture.width, size, 0)
    rows = _row_steps(picture.height, stride, size, 0)
    out = []
    at = 0
    for step in rows:
        pixels = bytearray()
        for i, column in enumerate(columns):
            if i < produced:
                byte = source[at] if at < len(source) else 0
                pixels += bytes((byte >> 4, byte & 0x0F))
            at += _s16(column)
        out.append(bytes(pixels))
        at += _s16(step)
    return tuple(out)


class _Drawer:
    """One run of the game's object drawing over a RAM image (0xA4EE)."""

    def __init__(self, ram: bytes, rom: bytes) -> None:
        self.ram = bytearray(ram)
        self.rom = rom
        self.cases = {
            "records": 0, "mirrored": 0, "anchored": 0, "two_part": 0, "clipped": 0,
            "hidden_cut": 0, "copies": 0, "dropped": 0, "colour_sets": {},
        }  # fmt: skip

    def _rw(self, address: int) -> int:
        return struct.unpack_from(">H", self.ram, address & 0xFFFF)[0]

    def _ww(self, address: int, value: int) -> None:
        struct.pack_into(">H", self.ram, address & 0xFFFF, value & 0xFFFF)

    def _romw(self, address: int) -> int:
        return struct.unpack_from(">H", self.rom, address)[0]

    def run(self) -> None:
        self.next_tile = self._rw(0xDC30)
        self.next_sprite = self._rw(0xDC34)
        self.list = struct.unpack_from(">I", self.ram, 0x27A6)[0]
        self.buffer = struct.unpack_from(">I", self.ram, 0x27CE)[0]
        self.first_tile = self._rw(0xCE86)
        self.left, self.right, self.top, _ = VIEW
        for k in range(128):
            index = self._rw(DRAW_ORDER + 2 * k)
            if index == 0xFFFF:
                break
            self.kept_tile, self.kept_sprite = self.next_tile, self.next_sprite
            self._record(DRAW_RECORDS + DRAW_RECORD_SIZE * index)
        self._ww(0xDC30, self.next_tile)
        self._ww(0xDC34, self.next_sprite)

    def _drop(self) -> None:
        self.cases["dropped"] += 1

    def _record(self, record: int) -> None:  # 0xA5C8
        cases = self.cases
        cases["records"] += 1
        self.failed = 0
        self.record = record
        self.x = self._rw(record)
        self.y = self._rw(record + 2)
        start_x, start_y = self.x, self.y
        self.size = max(self._rw(record + 4) << 2, 4)
        self.hidden = self._rw(record + 6)
        flags = self.flags = self._rw(record + 0x0A)
        self.mirror = bool(flags & 0x8000)
        cases["mirrored"] += self.mirror
        colour_set = (flags >> 12) & 3
        cases["colour_sets"][colour_set] = cases["colour_sets"].get(colour_set, 0) + 1
        self.palette = ((flags & 0x3000) << 1) & 0xFFFF
        second = self._rw(record + 0x12)
        picture = _picture_address(self.rom, _s16(self._rw(record + 8) << 2) >> 2)
        self.skip_rows = 0
        later = 0  # the second picture, drawn after the first
        if flags & 0x0080:
            cases["anchored"] += 1
            while True:
                self.clip = 0
                across, down = self._anchor(picture)
                self.down = down
                self.across, self.clip = self._edges(picture, across)
                if not self.failed:
                    break
                # The first picture is wholly off the screen: try the second alone.
                self.failed = 0
                if second == 0:
                    return self._drop()
                picture = _picture_address(self.rom, second)
                second = 0
            data = picture + 2
            if second != 0:
                cases["two_part"] += 1
                other = _picture_address(self.rom, second)
                across2, down2 = self._anchor(other)
                second_data = other + 2
                down2 = (down2 - 1) & 0xFFFF
                across2, clip2 = self._edges(second_data, across2)
                later = second_data
                if self.failed:
                    self.failed = 0
                    later = 0
                # Rows of the first picture that the second one covers are skipped.
                covered = (self.rom[second_data - 1] - self.rom[second_data + 1]) & 0xFF
                covered = ((down2 & 0xFF00) | covered) + 1
                rows = _s8(self.rom[data - 1] - (covered & 0xFF))
                if rows >= 0:
                    self.skip_rows = rows
                    self.down = (self.down - _scaled(rows, self.size)) & 0xFFFF
        else:
            data = picture
            self._centre(picture)
            if self.failed:
                return self._drop()
        first_sprite = self.next_sprite
        while True:
            self.out_of_room = 0
            draw = True
            above = (self.y - self.down - self.top) & 0xFFFF
            if _negative(above):
                # Part of it is above the view: skip the source rows that would land there.
                above = (-above) & 0xFFFF
                quotient = (above << 16) // ((self.size >> 2) & 0xFFFF)
                rows = (quotient & 0xFFFF) >> 2
                if not _negative(rows - self.skip_rows):
                    more = (rows - self.skip_rows) & 0xFFFF
                    self.skip_rows = rows
                    self.down = (self.down - _scaled(more, self.size)) & 0xFFFF
                    if not _s16(self.down) > 0:
                        draw = False
            if draw:
                self._draw(data)
            if not draw or self.out_of_room:
                self.next_tile, self.next_sprite = self.kept_tile, self.kept_sprite
                return self._drop()
            if later == 0:
                break
            data = later
            self.x, self.y = start_x, start_y
            self.across, self.down, self.clip = across2, down2, clip2
            self.skip_rows = 0
            later = 0
        self._copies(record, first_sprite)

    def _anchor(self, picture: int) -> tuple[int, int]:  # 0xA9A0
        across = _s8(self.rom[picture]) & 0xFFFF
        down = _s8(self.rom[picture + 1]) & 0xFFFF
        if self.mirror:
            across = (_s8(self.rom[picture + 2]) - _s16(across)) & 0xFFFF
        return _scaled(across, self.size), _scaled(down, self.size)

    def _clip_amount(self, amount: int) -> int:
        self.cases["clipped"] += 1
        odd = amount & 1
        return ((_s16(amount) >> 1) + odd) & 0xFFFF

    def _edges(self, picture: int, across: int) -> tuple[int, int]:  # 0xA9E4
        """Clip an anchored picture against the view's sides. Returns the anchor's
        distance from the left of what is drawn, and the clip for `_column_steps`."""
        width = _scaled(_s8(self.rom[picture + 2]) & 0xFFFF, self.size)
        x = self._rw(self.record)
        limit = self.right if self.mirror else self.right - 4
        over = (width - across + x - limit) & 0xFFFF
        if not _negative(over):
            if not _negative(over - width):
                self.failed = 1
                return across, over
            return across, self._clip_amount(-over if self.mirror else width - over)
        under = (x - across - self.left) & 0xFFFF
        if not _negative(under):
            return across, 0
        if not _negative((-under & 0xFFFF) - width):
            self.failed = 1
            return across, (-under) & 0xFFFF
        across = (across + under) & 0xFFFF
        return across, self._clip_amount(under + width if self.mirror else under)

    def _centre(self, picture: int) -> None:  # 0xAA78
        """Place and clip a picture without an anchor: centred on x, standing on y."""
        width = _scaled(self.rom[picture], self.size)
        self.across = width >> 1
        self.down = _scaled(self.rom[picture + 1], self.size)
        x = self._rw(self.record)
        over = (self.across + x - (self.right - 4)) & 0xFFFF
        if not _negative(over):
            if not _negative(over - width):
                self.failed = 1
                return
            self.cases["clipped"] += 1
            amount = -over if self.mirror else width - over
            self.clip = (_s16(amount) >> 1) & 0xFFFF
            return
        under = (x - (width >> 1) - self.left) & 0xFFFF
        if not _negative(under):
            self.clip = 0
            return
        if not _negative((-under & 0xFFFF) - width):
            self.failed = 1
            return
        self.cases["clipped"] += 1
        self.x = self.left
        self.across = 0
        amount = under + width if self.mirror else under
        self.clip = (_s16(amount) >> 1) & 0xFFFF

    def _draw(self, data: int) -> None:  # 0xA08E
        """Scale the picture whose width byte is at `data` and stage its sprites."""
        tile_offset = (self.next_tile << 5) & 0xFFFF
        columns, self.produced, self.groups, stride = _column_steps(
            self.rom[data], self.size, self.clip
        )
        if _negative(self.groups):
            self.failed = 1
            return
        rows = _row_steps(self.rom[data + 1], stride, self.size, self.skip_rows)
        last_row = (len(rows) - 1) & 0xFFFF
        if not _s16(last_row) > 0:
            self.failed = 1
            return
        self.sprite_word = self.palette  # the picture's own colour-set word is never used
        self.x = (self.x - self.across) & 0xFFFF
        self.y = (self.y - self.down) & 0xFFFF
        if not _negative(self.y - self.hidden):
            return  # wholly below the line a hill hides it under
        height = (last_row + 1) & 0xFFFF
        bottom = (height + self.y) & 0xFFFF
        if not _negative(bottom - self.hidden):
            self.cases["hidden_cut"] += 1
            cut = (bottom - self.hidden) & 0xFFFF
            last_row = (last_row - cut) & 0xFFFF
            height = (height - cut) & 0xFFFF
        tiles_high = (height & 0xFFF8) + (8 if height & 7 else 0)
        self._sprites(((self.groups + 1) << 3) & 0xFFFF, tiles_high)
        if self.failed == 0xFFFF:
            return
        at = self._copy(data + 4, tile_offset, columns, rows, last_row)
        # Blank the rows left over in the last row of tiles.
        spare = 7 - ((last_row + 1) & 7)
        if spare == 7:
            return
        for _ in range(spare + 1):
            to = at
            for group in range(self.groups + 1):
                if group:
                    to += self.short_gap
                self.ram[to & 0xFFFF : (to & 0xFFFF) + 4] = b"\0\0\0\0"
                to += 4
            at += 4

    def _sprites(self, width: int, height: int) -> None:  # 0x7A68
        """Cut a box of whole tiles into sprites of at most 32x32 and add them to the list."""
        shape = lambda offset, which: self._romw(SPRITE_SHAPES + offset + which)  # noqa: E731
        self.band_gap = 0x8000
        entry = self.list + self.next_sprite * 8
        if self.next_sprite != 0:
            if self.next_sprite >= MAX_SPRITES:
                self.out_of_room = self.failed = 0xFFFF
                return
            self._ww(entry - 6, self._rw(entry - 6) | self.next_sprite)
        across = width >> 5
        last_width = width & 0x18
        if last_width == 0:
            across -= 1
            last_width = 0x20
        last_width -= 8
        down = height >> 5
        last_height = height & 0x18
        if last_height == 0:
            down -= 1
            last_height = 0x20
        last_height = (last_height - 8) << 2
        per_row = across
        self.gap = 0x7C
        self.short_gap = last_height + 0x20 - 4
        if down == 0:
            self.gap = self.short_gap
        y = (self.y + 0x80) & 0xFFFF
        link = self.next_sprite + 1
        tile = self.next_tile
        while True:
            band_start = tile
            x = (self.x + 0x80) & 0xFFFF
            if self.mirror:
                x = (x + 2 * self.produced) & 0xFFFF
            tall = last_height if down == 0 else 0x60
            while True:
                offset = (last_width if across == 0 else 0x18) + tall
                link |= shape(offset, 2)
                self._ww(entry, y)
                self._ww(entry + 2, link)
                word = tile | 0x8000 if _negative(y - 0x118) else tile
                word = ((word | self.sprite_word) + self.first_tile) & 0xFFFF
                if self.mirror:
                    word |= 0x800
                    x = (x - shape(offset, 6)) & 0xFFFF
                self._ww(entry + 4, word)
                self._ww(entry + 6, x)
                entry += 8
                link = (link & 0xFF) + 1
                tile = (tile + shape(offset, 4)) & 0xFFFF
                if not _negative(tile - MAX_TILES):
                    self.out_of_room = self.failed = 0xFFFF
                    if self.next_sprite != 0:
                        last = self.list + self.next_sprite * 8
                        self._ww(last - 6, self._rw(last - 6) & 0xFF00)
                    return
                if not self.mirror:
                    x = (x + shape(offset, 6)) & 0xFFFF
                across -= 1
                if across == -1:
                    break
            y = (y + shape(offset, 0)) & 0xFFFF
            across = per_row
            if self.band_gap == 0x8000:
                self.band_gap = ((tile - band_start - 4) << 5) & 0xFFFF
            down -= 1
            if down == -1:
                break
        self._ww(entry - 6, self._rw(entry - 6) & 0xFF00)
        self.next_sprite = link - 1
        self.next_tile = tile

    def _copy(
        self,
        source: int,
        tile_offset: int,
        columns: Sequence[int],
        rows: Sequence[int],
        last_row: int,
    ) -> int:  # 0xA3CA; fmt: skip
        """Copy the scaled picture into the tile buffer. Tiles run down each sprite's
        columns, so the next 8 pixels of a row are a whole column of tiles further on."""
        at = self.buffer + _s16(tile_offset)
        rows_left_in_band = 0x1F
        gap = self.gap
        count = last_row + 1
        for r in range(count):
            to = at
            step = 0
            for group in range(self.groups + 1):
                if group:
                    to += gap
                for _ in range(4):
                    self.ram[to & 0xFFFF] = self.rom[source]
                    to += 1
                    source += _s16(columns[step])
                    step += 1
            at += 4
            rows_left_in_band -= 1
            if rows_left_in_band == -1:
                if count - 1 - r < 0x1F:
                    gap = self.short_gap
                rows_left_in_band = 0x1F
                at += _s16(self.band_gap)
            source += _s16(rows[r]) if r < len(rows) else 0
        return at

    def _copies(self, record: int, first: int) -> None:  # 0xA8AA
        """Extra copies of an object: its sprite entries repeated with a sideways step."""
        copies = self.ram[(record + 0x0A) & 0xFFFF] & 7
        if copies == 0 or self.clip != 0:
            return
        made = self.next_sprite - first
        if made == 0:
            return
        self.cases["copies"] += 1
        source = self.list + first * 8
        index = self.next_sprite
        to = self.list + index * 8
        irregular = bool(self.flags & 0x0020)
        spacing = 0
        if irregular:
            spacing = struct.unpack_from(
                ">I", self.rom, COPY_SPACING + (self._rw(record + 0x14) & 7) * 4
            )[0]

        def step() -> int:
            nonlocal spacing
            value = _s16(self._rw(record + 0x0C))
            if irregular:
                part = value
                shift = _s16(self._romw(spacing))
                spacing += 2
                if shift < 0:
                    shift, part = -shift, -part
                value += _s16(part) >> (shift & 63)
            return value

        offset = step()
        for copy in range(copies):
            for _ in range(made):
                self.ram[(to - 5) & 0xFFFF] = index & 0xFF
                start, end = source & 0xFFFF, to & 0xFFFF
                self.ram[end : end + 8] = self.ram[start : start + 8]
                source += 8
                to += 8
                self._ww(to - 2, self._rw(to - 2) + offset)
                x = self._rw(to - 2)
                width = ((self.ram[(to - 6) & 0xFFFF] & 0x0C) << 1) + 8
                inside = not _negative(self.right + 0x80 - width - x) and _negative(
                    self.left + 0x80 - x
                )
                if not inside:
                    source -= 8
                    to -= 8
                    continue
                index += 1
                if index >= MAX_SPRITES:
                    self.ram[(to - 5) & 0xFFFF] = 0
                    return
            self.next_sprite = index
            if irregular and copy != copies - 1:
                offset = step()
        self.ram[(to - 5) & 0xFFFF] = 0


def staged_sprites(ram: bytes) -> list[StagedSprite]:
    """The sprite list a RAM image holds in the buffer being built, in screen terms."""
    at = struct.unpack_from(">I", ram, 0x27A6)[0] & 0xFFFF
    sprites = []
    for _ in range(struct.unpack_from(">H", ram, 0xDC34)[0]):
        y, shape, word, x = struct.unpack_from(">4H", ram, at)
        sprites.append(
            StagedSprite(
                x=_s16(x - 0x80),
                y=_s16(y - 0x80),
                cells_wide=((shape >> 10) & 3) + 1,
                cells_high=((shape >> 8) & 3) + 1,
                tile=word & 0x7FF,
                palette=(word >> 13) & 3,
                priority=bool(word & 0x8000),
                mirrored=bool(word & 0x0800),
                upside_down=bool(word & 0x1000),
                link=shape & 0x7F,
            )
        )
        at += 8
    return sprites


def draw_objects_detail(ram: bytes, rom: bytes) -> Drawn:
    """Repeat the game's object drawing for a one-player race image taken at 0xA4EE."""
    drawer = _Drawer(ram, rom)
    drawer.run()
    out = bytes(drawer.ram)
    buffer = drawer.buffer & 0xFFFF
    return Drawn(
        sprites=staged_sprites(out),
        pictures=out[buffer : buffer + MAX_TILES * TILE_BYTES],
        ram=out,
        cases=drawer.cases,
    )


def draw_objects(ram: bytes, rom: bytes) -> tuple[list[StagedSprite], bytes]:
    """The sprite list and the picture-buffer bytes the game stages for this image."""
    drawn = draw_objects_detail(ram, rom)
    return drawn.sprites, drawn.pictures


def _u16(ram: bytes, address: int) -> int:
    return struct.unpack_from(">H", ram, address & 0xFFFF)[0]


def _u32(ram: bytes, address: int) -> int:
    return struct.unpack_from(">I", ram, address & 0xFFFF)[0]


def _perspective(rom: bytes, distance: int) -> int:
    return struct.unpack_from(">H", rom, PERSPECTIVE_TABLE + 2 * distance)[0]


def _project(value: int, scale: int) -> int:
    """A signed world value x a perspective scale / 65536, rounding toward zero."""
    value = _s16(value)
    if value < 0:
        return -(((-value) * scale) >> 16)
    return (value * scale) >> 16


def record_placement(ram: bytes, index: int) -> Placement:
    """What draw record `index` holds."""
    at = DRAW_RECORDS + DRAW_RECORD_SIZE * index
    return Placement(
        x=_s16(_u16(ram, at)),
        foot_line=_s16(_u16(ram, at + 2)),
        size=_u16(ram, at + 4),
        hidden_below=_s16(_u16(ram, at + 6)),
        offset=_s16(_u16(ram, at + 0x0C)),
        distance=_s16(_u16(ram, at + 0x0E)),
    )


def roadside_placement(ram: bytes, rom: bytes, index: int) -> Placement:
    """Where the game puts roadside draw record `index` (0x17198), from the record's
    road point and list byte, the camera's fraction and the road point arrays."""
    at = DRAW_RECORDS + DRAW_RECORD_SIZE * index
    flags = _u16(ram, at + 0x0A)
    point = flags & 0x1F
    classes = _u16(ram, at + 0x10)
    distance = ((point << 8) - (_u16(ram, 0x0A0C) >> 8)) & 0xFFFF
    size = (((((classes & 0xF0) << 3) + 0x200) << 14) // ((distance + 0x200) & 0xFFFF)) & 0xFFFF
    offset = (((classes & 0x0F) << 6) * _perspective(rom, distance)) >> 16
    if flags & 0x4000:
        offset = -offset
    return Placement(
        x=_s16(_u16(ram, _u32(ram, 0x1CE0) + 2 * point) + offset),
        foot_line=_s16(_u16(ram, _u32(ram, 0x1CDC) + 2 * point)),
        size=size,
        hidden_below=_s16(_u16(ram, _u32(ram, 0x1CE4) + 2 * point)),
        offset=offset,
        distance=_s16(distance),
    )


def _between(near: int, far: int, fraction: int) -> int:
    """The part of the way from `near` to `far`, rounded, that `fraction` / 65536 is."""
    difference = _s16(near - far)
    product = abs(difference) * fraction
    part = (product >> 16) + (1 if product & 0x8000 else 0)
    return -part if difference < 0 else part


def vehicle_placement(ram: bytes, rom: bytes, index: int, vehicle: int) -> Placement | None:
    """Where the game puts the draw record of the vehicle object at `vehicle` (0x1628A).

    The road's world centre and height are blended between the road point
    behind the vehicle and the next one by the fraction of the vehicle's
    track position; the vehicle's own sideways position and height are added
    and the result is scaled by the perspective at the vehicle's own
    distance. None if the vehicle is behind the camera.
    """
    at = DRAW_RECORDS + DRAW_RECORD_SIZE * index
    point = _u16(ram, at + 0x0A) & 0x1F
    distance = ((_u32(ram, RECORD_DISTANCES + 4 * index) << 8) >> 16) & 0xFFFF
    if distance & 0x8000:
        return None
    fraction = _u16(ram, vehicle + 6)
    scale = _perspective(rom, distance)
    wide = _s8(ram[(vehicle + 0x74) & 0xFFFF]) >= 0x10
    size = (((0x3000000 if wide else 0x2800000) >> 2) // (distance + 0x200)) & 0xFFFF
    world = _u32(ram, 0x1CE8) + 4 * point
    height = _u16(ram, world + 2) - _between(_u16(ram, world + 2), _u16(ram, world + 6), fraction)
    line = (_project(height - _u16(ram, vehicle + 0x32), scale) + _u16(ram, 0xF2BC)) & 0xFFFF
    if _negative(line - _u16(ram, 0xF2B0)):
        line = _u16(ram, 0xF2B0)
    centre = _u16(ram, world) - _between(_u16(ram, world), _u16(ram, world + 4), fraction)
    x = _project(centre + _u16(ram, vehicle + 0x30), scale) + _u16(ram, 0xF2BE)
    return Placement(
        x=_s16(x),
        foot_line=_s16(line),
        size=size,
        hidden_below=_s16(_u16(ram, _u32(ram, 0x1CE4) + 2 * point)),
    )


def vehicle_addresses(ram: bytes, rom: bytes) -> list[int]:
    """Work-RAM addresses of the vehicle objects the game makes draw records for."""
    out = []
    at = 0x04F8
    while len(out) < 40:
        address = _u32(ram, at)
        at += 4
        if address & 0xFFFF == 0xFFFF:
            break
        out.append(address & 0xFFFF)
    for table in (0x272A8, 0x272B4, 0x272BC):  # ROM lists of further vehicle objects
        for i in range(24):
            address = struct.unpack_from(">I", rom, table + 4 * i)[0]
            if address >> 16 != 0xFF or address & 0xFFFF == 0xFFFF:
                break
            out.append(address & 0xFFFF)
    return out


def _find_pictures(rom: bytes) -> list[tuple[int, bool]]:
    """(index, anchored) of every entry of the picture table that is a whole picture.

    Whether a picture has an anchor is not stored with it (the draw record
    says), so it is told from its length: the gap to the next picture fits
    one layout or the other.
    """
    entries = []
    lowest = len(rom)
    for index in range(1, 0x1000):
        at = PICTURE_TABLE + 4 * index
        if at + 4 > min(lowest, len(rom)):
            break
        address = struct.unpack_from(">I", rom, at)[0]
        if address == 0:
            continue
        if not PICTURE_TABLE < address < len(rom) - 6:
            break
        lowest = min(lowest, address)
        entries.append((index, address))
    starts = sorted({address for _, address in entries})
    following = dict(zip(starts, starts[1:] + [len(rom)], strict=True))
    found = []
    for index, address in entries:
        room = following[address] - address
        for anchored in (False, True):
            head = address + (2 if anchored else 0)
            width, height = rom[head], rom[head + 1]
            length = (2 if anchored else 0) + 4 + ((width + 1) // 2 + 1) * height
            last = following[address] == len(rom) and length <= room
            if width and height and (last or 0 <= room - length <= 1):
                found.append((index, anchored))
                break
    return found


def _rgb(colour: int) -> tuple[int, int, int]:
    """A colour as the video chip stores it (0000 BBB0 GGG0 RRR0)."""
    return tuple(((colour >> shift) & 7) * 255 // 7 for shift in (1, 5, 9))


def extract_art(rom: bytes, colours: Sequence[int], out: Path) -> int:
    """Write every object picture as a PNG at natural size, and one sheet of them all.

    `colours` are the video chip's 64 colours during a race. The directory
    must be under a `generated` directory: the art is the ROM's, and nothing
    derived from the ROM may be committed. Returns the number of pictures.
    """
    from PIL import Image

    if "generated" not in out.resolve().parts:
        raise ValueError(f"{out} is not under a generated/ directory; ROM art must stay there")
    out.mkdir(parents=True, exist_ok=True)
    images = []
    seen = set()
    for index, anchored in _find_pictures(rom):
        address = _picture_address(rom, index)
        if address in seen:
            continue
        seen.add(address)
        picture = read_picture(rom, index, anchored)
        image = Image.new("RGBA", (picture.width, picture.height), (0, 0, 0, 0))
        for y, row in enumerate(picture.rows):
            for x, colour in enumerate(row):
                if colour:
                    rgb = _rgb(colours[picture.colour_set * 16 + colour])
                    image.putpixel((x, y), (*rgb, 255))
        image.save(out / f"picture-{index:04d}.png")
        images.append(image)
    if images:
        cell_w = max(i.width for i in images) + 2
        cell_h = max(i.height for i in images) + 2
        per_row = max(1, min(16, len(images)))
        sheet = Image.new(
            "RGBA", (cell_w * per_row, cell_h * -(-len(images) // per_row)), (64, 64, 64, 255)
        )
        for n, image in enumerate(images):
            sheet.paste(image, ((n % per_row) * cell_w + 1, (n // per_row) * cell_h + 1), image)
        sheet.save(out / "sheet.png")
    return len(images)
