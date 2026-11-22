"""Checking the scene reader against a handoff file and against video memory."""

import struct

from test_scene import TABLES, put_points, race_ram

from rr2port.handoffs import write_handoffs
from rr2port.picture_report import VideoState
from rr2port.scene import predict_line_tables, project_road, read_scene
from rr2port.scene_check import STAGED_TABLES, check_handoffs, find_pass_shown


def image(sideways: int, stage_at: int | None = None, staged_from: bytes | None = None) -> bytes:
    """A race image with its road projected; optionally with a staged shift table."""
    ram = race_ram(sideways=sideways)
    put_points(ram, project_road(bytes(ram), TABLES))
    if stage_at is not None:
        source = bytes(ram) if staged_from is None else staged_from
        for line, (a, b) in predict_line_tables(read_scene(source), TABLES).items():
            struct.pack_into(">HH", ram, stage_at + 4 * line, a, b)
    return bytes(ram)


def test_check_reports_counts(tmp_path):
    first = image(0, STAGED_TABLES[0])
    second = image(40, STAGED_TABLES[1], staged_from=first)  # shows the previous pass's road
    broken = bytearray(image(80, STAGED_TABLES[0], staged_from=second))
    struct.pack_into(">h", broken, 0x1DBC + 4, 999)  # a segment array the projection disagrees with
    struct.pack_into(">H", broken, STAGED_TABLES[0] + 4 * 120, 0x1234)  # and a wrong shift
    path = tmp_path / "handoffs.bin"
    write_handoffs(path, [first, second, bytes(broken)])
    result = check_handoffs(path, TABLES)
    assert (result.handoffs, result.segments_identical) == (3, 2)
    assert result.first_differing_handoff == 2
    # Each handoff has two staged tables. Explained: the first's own, the second's (from one
    # handoff back). The untouched all-zero tables and the damaged one are not.
    assert result.tables_checked == 6 and result.tables_explained == 2
    assert result.tables_half_written == 0
    assert result.handoffs_back == {0: 1, 1: 1}


def test_a_table_caught_half_written_is_counted_apart(tmp_path):
    first = image(0, STAGED_TABLES[0])
    second = bytearray(image(40, STAGED_TABLES[0], staged_from=first))
    # The main loop was rewriting the table from the bottom up when the image was taken:
    # lines 130 and below already hold this image's road, the rest still the first's.
    own = predict_line_tables(read_scene(bytes(second)), TABLES)
    for line in range(130, 152):
        struct.pack_into(">HH", second, STAGED_TABLES[0] + 4 * line, *own[line])
    path = tmp_path / "handoffs.bin"
    write_handoffs(path, [first, bytes(second)])
    result = check_handoffs(path, TABLES)
    assert result.tables_explained == 1 and result.tables_half_written == 1
    assert result.unexplained == [0, 1]  # the two all-zero copies


def test_pairing_of_pass_and_frame_is_measured():
    images = [image(0, STAGED_TABLES[0]), image(40, STAGED_TABLES[1]), image(80, STAGED_TABLES[0])]
    vram = bytearray(0x10000)
    vram[0xC000 : 0xC000 + 608] = images[1][STAGED_TABLES[1] : STAGED_TABLES[1] + 608]
    reg = bytearray(24)
    reg[13] = 0x30  # the shift table is at 0xC000 in video memory
    state = VideoState(bytes(reg), bytes(vram), (0,) * 64, (0,) * 40)
    assert find_pass_shown(state, list(enumerate(images))) == [(1, 1)]
    vram[0xC000 + 4 * 120] ^= 0xFF  # a picture no pass staged
    state = VideoState(bytes(reg), bytes(vram), (0,) * 64, (0,) * 40)
    assert find_pass_shown(state, list(enumerate(images))) == []


def test_the_command_refuses_a_rom_that_is_not_rr206(tmp_path, capsys):
    from rr2port.cli import scene_check_main

    rom = tmp_path / "other.gen"
    rom.write_bytes(bytes(0x100000))
    handoffs = tmp_path / "handoffs.bin"
    write_handoffs(handoffs, [image(0)])
    assert scene_check_main(["--rom", str(rom), "--handoffs", str(handoffs)]) == 2
    assert "SHA-256" in capsys.readouterr().err
