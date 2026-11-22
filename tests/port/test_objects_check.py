"""Checking the object reader against image files taken before and after the game's drawing."""

import struct

from test_objects import plain_picture, race_ram, rom_with

from rr2port.handoffs import write_handoffs
from rr2port.objects import draw_objects_detail
from rr2port.objects_check import check_drawing, format_drawing


def pair(x: int, rom: bytes) -> tuple[bytes, bytes]:
    before = bytes(race_ram([{"x": x, "foot": 80, "picture": 1, "flags": 0x9000}]))
    return before, draw_objects_detail(before, rom).ram


def test_check_reports_counts_and_cases(tmp_path):
    rom = rom_with({1: plain_picture(16, 16)})
    pairs = [pair(100, rom), pair(120, rom), pair(140, rom)]
    wrong_list = bytearray(pairs[1][1])
    wrong_list[0x3E40 + 6] ^= 1  # the first sprite's x
    wrong_picture = bytearray(pairs[2][1])
    wrong_picture[0x4742 + 5] ^= 0x10
    before, after = tmp_path / "before.bin", tmp_path / "after.bin"
    write_handoffs(before, [b for b, _ in pairs])
    write_handoffs(after, [pairs[0][1], bytes(wrong_list), bytes(wrong_picture)])
    result = check_drawing(before, after, rom)
    assert (result.images, result.lists_identical, result.pictures_identical) == (3, 2, 2)
    assert result.first_differing == 1
    assert result.cases["mirrored"] == 3 and result.cases["colour_sets"] == {1: 3}
    text = format_drawing(result)
    assert "SPRITE_LISTS identical=2 of 3" in text and "SPRITE_PICTURES identical=2 of 3" in text
    assert "mirrored=3" in text and "colour_sets=1:3" in text


def test_every_takes_a_sample(tmp_path):
    rom = rom_with({1: plain_picture(16, 16)})
    pairs = [pair(100 + 10 * i, rom) for i in range(5)]
    before, after = tmp_path / "before.bin", tmp_path / "after.bin"
    write_handoffs(before, [b for b, _ in pairs])
    write_handoffs(after, [a for _, a in pairs])
    assert check_drawing(before, after, rom, every=2).images == 3


def test_an_after_image_of_an_earlier_pass_is_skipped(tmp_path):
    # The run can start part-way through a pass, so the after file may begin with an image
    # that has no before image. Its draw records are not the first before image's.
    rom = rom_with({1: plain_picture(16, 16)})
    pairs = [pair(100, rom), pair(120, rom)]
    stray = bytearray(pairs[0][1])
    struct.pack_into(">h", stray, 0xCEB8, 55)
    before, after = tmp_path / "before.bin", tmp_path / "after.bin"
    write_handoffs(before, [b for b, _ in pairs])
    write_handoffs(after, [bytes(stray)] + [a for _, a in pairs])
    result = check_drawing(before, after, rom)
    assert (result.images, result.lists_identical, result.unpaired) == (2, 2, 1)


def test_the_index_files_keep_an_earlier_image_with_the_same_records_out(tmp_path):
    # At the start line nothing moves, so a stray after image can hold the same records as
    # the first before image. The frame numbers rr-race-replay writes tell them apart.
    rom = rom_with({1: plain_picture(16, 16)})
    first, second = pair(100, rom), pair(120, rom)
    stray = bytearray(first[1])
    stray[0x3E40 + 6] ^= 1  # same records, but not this pass's drawing
    before, after = tmp_path / "before.bin", tmp_path / "after.bin"
    write_handoffs(before, [first[0], second[0]])
    write_handoffs(after, [bytes(stray), first[1], second[1]])
    (tmp_path / "before.bin.index").write_text("0 0 1027\n1 1 1034\n")
    (tmp_path / "after.bin.index").write_text("0 0 1025\n1 0 1032\n2 1 1039\n")
    result = check_drawing(before, after, rom)
    assert (result.images, result.lists_identical, result.unpaired) == (2, 2, 1)
