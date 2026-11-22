"""Reading object pictures and repeating the game's drawing of them, on synthetic data."""

import struct

import pytest

from rr2port.objects import (
    PICTURE_TABLE,
    SPRITE_SHAPES,
    draw_objects,
    draw_objects_detail,
    extract_art,
    read_picture,
    roadside_placement,
    scale_picture,
    staged_sprites,
)

SPRITE_LIST = 0x3E40
PICTURE_BUFFER = 0x4742
FIRST_TILE = 0x634
PICTURES_AT = 0x58000


def plain_picture(width: int, height: int) -> bytes:
    """A picture without an anchor: every byte different, so a wrong byte shows."""
    row_bytes = (width + 1) // 2
    rows = b"".join(
        bytes(((r * row_bytes + c) % 15 + 1) * 0x11 for c in range(row_bytes)) + b"\0"
        for r in range(height)
    )
    return bytes([width, height]) + b"\x20\x00" + rows


def rom_with(pictures: dict[int, bytes]) -> bytes:
    rom = bytearray(0x60000)
    # The table of sprite shapes: for each height and width in tiles, the height in pixels,
    # the size bits, the tiles used and the width in pixels.
    for high in range(1, 5):
        for wide in range(1, 5):
            at = SPRITE_SHAPES + (high - 1) * 0x20 + (wide - 1) * 8
            struct.pack_into(
                ">4H", rom, at, high * 8, ((wide - 1) << 2 | (high - 1)) << 8, wide * high, wide * 8
            )
    at = PICTURES_AT
    for index, data in pictures.items():
        struct.pack_into(">I", rom, PICTURE_TABLE + 4 * index, at)
        rom[at : at + len(data)] = data
        at += len(data)
    struct.pack_into(">I", rom, PICTURE_TABLE + 4 * (max(pictures) + 1), 0xFFFFFFFF)
    return bytes(rom)


def race_ram(records: list[dict]) -> bytearray:
    ram = bytearray(0x10000)
    struct.pack_into(">I", ram, 0x27A6, 0xFF0000 | SPRITE_LIST)
    struct.pack_into(">I", ram, 0x27CE, 0xFF0000 | PICTURE_BUFFER)
    struct.pack_into(">H", ram, 0xCE86, FIRST_TILE)
    for i, record in enumerate(records):
        at = 0xCEB8 + 22 * i
        struct.pack_into(
            ">hhHhHH", ram, at,
            record["x"], record["foot"], record.get("size", 0x4000),
            record.get("hidden", 0x98), record["picture"], record.get("flags", 0x1000),
        )  # fmt: skip
        struct.pack_into(">h", ram, at + 0x0C, record.get("offset", 0))
        struct.pack_into(">H", ram, at + 0x12, record.get("second", 0))
        struct.pack_into(">H", ram, 0x08D6 + 2 * i, i)
    struct.pack_into(">H", ram, 0x08D6 + 2 * len(records), 0xFFFF)
    return ram


def test_read_picture_decodes_rows_and_header():
    data = bytes([4, 3]) + b"\x20\x00" + b"\x12\x34\x00" + b"\x50\x06\x00" + b"\xff\xff\x00"
    rom = rom_with({1: data, 2: bytes([1, 2]) + data})
    plain = read_picture(rom, 1, anchored=False)
    assert (plain.width, plain.height, plain.anchor_x, plain.anchor_y) == (4, 3, None, None)
    assert plain.rows == (bytes([1, 2, 3, 4]), bytes([5, 0, 0, 6]), bytes([15, 15, 15, 15]))
    assert plain.colour_set == 1
    anchored = read_picture(rom, 2, anchored=True)
    assert (anchored.anchor_x, anchored.anchor_y) == (1, 2)
    assert anchored.rows == plain.rows


def test_scale_picture_at_natural_half_and_double_size():
    rom = rom_with({1: plain_picture(8, 4)})
    picture = read_picture(rom, 1, anchored=False)
    assert scale_picture(picture, 0x4000) == picture.rows
    half = scale_picture(picture, 0x2000)
    # Two source steps for each output step: every other pixel pair and every other row.
    assert len(half) == 2 and all(len(row) == 4 for row in half)
    assert half[0][:2] in (picture.rows[0][0:2], picture.rows[0][2:4])
    double = scale_picture(picture, 0x8000)
    assert len(double) == 8 and all(len(row) == 16 for row in double)
    assert double[0] == double[1] != double[2]
    # The first pixel pair comes out once and the following ones twice: the game's phase.
    assert double[0][0:6] == bytes([1, 1, 2, 2, 2, 2])


def test_staged_sprites_for_one_record():
    rom = rom_with({1: plain_picture(16, 16)})
    ram = race_ram([{"x": 100, "foot": 80, "picture": 1}])
    sprites, pictures = draw_objects(bytes(ram), rom)
    # Centred on x and standing on the foot line. The row's spare byte makes it 3 tiles wide.
    assert len(sprites) == 1
    sprite = sprites[0]
    assert (sprite.x, sprite.y) == (92, 64)
    assert (sprite.cells_wide, sprite.cells_high) == (3, 2)
    assert (sprite.tile, sprite.palette, sprite.priority, sprite.mirrored, sprite.link) == (
        FIRST_TILE, 1, True, False, 0,
    )  # fmt: skip
    # Tiles go down the sprite's columns; a tile is 8 rows of 4 bytes.
    source = rom[PICTURES_AT + 4 :]
    assert pictures[0:4] == source[0:4]  # row 0, first 8 pixels
    assert pictures[4:8] == source[9:13]  # row 1 (rows are 9 bytes in the ROM)
    assert pictures[2 * 32 : 2 * 32 + 4] == source[4:8]  # row 0, next tile column
    assert pictures[32 : 32 + 4] == source[8 * 9 : 8 * 9 + 4]  # row 8: the tile below
    assert len(pictures) == 0x150 * 32


def test_mirrored_and_clipped_records():
    anchored = bytes([2, 16]) + plain_picture(16, 16)
    rom = rom_with({1: plain_picture(16, 16), 2: anchored})
    ram = race_ram(
        [
            {"x": 100, "foot": 80, "picture": 2, "flags": 0x9080},  # mirrored, with an anchor
            {"x": 200, "foot": 80, "picture": 1, "hidden": 72},  # a hill hides its lower half
            {"x": 400, "foot": 80, "picture": 1},  # wholly off the right edge
            {"x": 316, "foot": 80, "picture": 1},  # half of it off the right edge
        ]
    )
    drawn = draw_objects_detail(bytes(ram), rom)
    mirrored, hidden, cut = drawn.sprites
    # The anchor is 2 from the left, so mirrored it is 14 from the left: 100 - 14 = 86. The
    # 16 pixels of picture end at 102 and the 24-pixel sprite is laid from there leftwards.
    assert (mirrored.x, mirrored.y, mirrored.mirrored) == (78, 64, True)
    assert (hidden.cells_high, hidden.y) == (1, 64)  # rows 64..71 only
    assert cut.x == 308 and cut.cells_wide == 2  # 5 pixel pairs kept, rounded up to tiles
    assert drawn.cases["mirrored"] == 1 and drawn.cases["hidden_cut"] == 1
    assert drawn.cases["clipped"] == 1 and drawn.cases["dropped"] == 1
    assert drawn.cases["colour_sets"] == {1: 4}


def test_wide_picture_is_cut_into_sprites():
    rom = rom_with({1: plain_picture(80, 16)})
    ram = race_ram([{"x": 160, "foot": 80, "picture": 1}])
    sprites, _ = draw_objects(bytes(ram), rom)
    assert [(s.x, s.cells_wide, s.cells_high) for s in sprites] == [
        (120, 4, 2), (152, 4, 2), (184, 3, 2),
    ]  # fmt: skip
    assert [s.tile for s in sprites] == [FIRST_TILE, FIRST_TILE + 8, FIRST_TILE + 16]
    assert [s.link for s in sprites] == [1, 2, 0]


def test_copies_repeat_the_sprites_with_a_sideways_step():
    rom = rom_with({1: plain_picture(16, 16)})
    ram = race_ram([{"x": 100, "foot": 80, "picture": 1, "flags": 0x1200, "offset": 40}])
    sprites, _ = draw_objects(bytes(ram), rom)
    assert [s.x for s in sprites] == [92, 132, 172]
    assert len({s.tile for s in sprites}) == 1  # the same picture, drawn once
    assert [s.link for s in sprites] == [1, 2, 0]


def test_staged_sprites_reads_back_what_was_drawn():
    rom = rom_with({1: plain_picture(80, 16)})
    drawn = draw_objects_detail(bytes(race_ram([{"x": 160, "foot": 80, "picture": 1}])), rom)
    assert staged_sprites(drawn.ram) == drawn.sprites


def test_roadside_placement_follows_the_road_point():
    ram = bytearray(0x10000)
    rom = bytearray(0x60000)
    struct.pack_into(">H", rom, 0x27C84 + 2 * 0x2C0, 0x8000)  # scale one half at this distance
    struct.pack_into(">H", ram, 0x0A0C, 0x4000)  # a quarter of the way through the segment
    for pointer, array in ((0x1CDC, 0x1E20), (0x1CE0, 0x1DBC), (0x1CE4, 0xD2B8)):
        struct.pack_into(">I", ram, pointer, 0xFF0000 | array)
    struct.pack_into(">h", ram, 0x1E20 + 6, 70)
    struct.pack_into(">h", ram, 0x1DBC + 6, 150)
    struct.pack_into(">h", ram, 0xD2B8 + 6, 68)
    record = 0xCEB8
    struct.pack_into(">H", ram, record + 0x0A, 0x4003)  # road point 3, the far side by bit 14
    struct.pack_into(">H", ram, record + 0x10, 0x23)  # size class 2, offset class 3
    placed = roadside_placement(bytes(ram), bytes(rom), 0)
    distance = 3 * 256 - 0x40
    assert placed.distance == distance
    assert placed.size == (2 * 128 + 0x200) * 0x4000 // (distance + 0x200)
    assert (placed.foot_line, placed.hidden_below) == (70, 68)
    assert placed.offset == -(3 * 64 // 2) and placed.x == 150 - 96


def test_extract_art_refuses_a_directory_outside_generated(tmp_path):
    rom = rom_with({1: plain_picture(16, 16)})
    with pytest.raises(ValueError, match="generated"):
        extract_art(rom, [0] * 64, tmp_path / "art")
    out = tmp_path / "generated" / "art"
    written = extract_art(rom, [0x0EEE] * 64, out)
    assert written == 1 and (out / "picture-0001.png").exists() and (out / "sheet.png").exists()


def anchored_picture(anchor_x: int, anchor_y: int, width: int, height: int) -> bytes:
    return bytes([anchor_x, anchor_y]) + plain_picture(width, height)


def test_each_colour_set_reaches_the_sprite():
    rom = rom_with({1: plain_picture(16, 16)})
    records = [
        {"x": 40 + 60 * colour_set, "foot": 80, "picture": 1, "flags": colour_set << 12}
        for colour_set in range(4)
    ]
    drawn = draw_objects_detail(bytes(race_ram(records)), rom)
    assert [s.palette for s in drawn.sprites] == [0, 1, 2, 3]
    assert drawn.cases["colour_sets"] == {0: 1, 1: 1, 2: 1, 3: 1}


def test_a_two_part_record_draws_both_pictures_from_the_same_anchor():
    # The second picture's anchor is 32 above its foot, so it stands on top of the first.
    rom = rom_with({2: anchored_picture(8, 16, 16, 16), 3: anchored_picture(8, 32, 16, 16)})
    ram = race_ram([{"x": 100, "foot": 80, "picture": 2, "flags": 0x1080, "second": 3}])
    drawn = draw_objects_detail(bytes(ram), rom)
    first, second = drawn.sprites
    assert drawn.cases["two_part"] == 1 and drawn.cases["anchored"] == 1
    assert (first.x, first.y) == (92, 64)
    assert (second.x, second.y) == (92, 49)  # the game places it one line lower than 80 - 32
    assert (first.tile, second.tile) == (FIRST_TILE, FIRST_TILE + 6)
    assert (first.link, second.link) == (1, 0)


def test_an_anchored_picture_is_clipped_at_the_right_edge():
    rom = rom_with({2: anchored_picture(8, 16, 16, 16)})
    ram = race_ram([{"x": 312, "foot": 80, "picture": 2, "flags": 0x1080}])
    drawn = draw_objects_detail(bytes(ram), rom)
    (sprite,) = drawn.sprites
    assert drawn.cases["clipped"] == 1
    assert (sprite.x, sprite.cells_wide) == (304, 2)  # 4 of its 16 pixels are past the limit


def test_rows_above_the_view_are_skipped():
    rom = rom_with({1: plain_picture(16, 16)})
    ram = race_ram([{"x": 100, "foot": 8, "picture": 1}])  # its top would be at line -8
    sprites, pictures = draw_objects(bytes(ram), rom)
    (sprite,) = sprites
    assert (sprite.y, sprite.cells_high) == (0, 1)
    source = rom[PICTURES_AT + 4 :]
    # As the game's code reads: the first row drawn is still the picture's row 0, and the
    # 8 skipped rows are jumped over after it, so the second row drawn is row 9.
    assert pictures[0:4] == source[0:4]
    assert pictures[4:8] == source[9 * 9 : 9 * 9 + 4]
