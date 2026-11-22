"""Summarising rr-race-replay's --video-log: who writes what part of video memory."""

import pytest

from rr2port.picture_report import VideoState
from rr2port.video_report import VideoLogError, read_video_log, summarise


def state() -> VideoState:
    reg = bytearray(24)
    reg[2] = 0x10  # plane A at 0x4000
    reg[3] = 0x0C  # window at 0x3000
    reg[4] = 0x03  # plane B at 0x6000
    reg[5] = 0x62  # sprites at 0xC400
    reg[13] = 0x30  # horizontal scroll at 0xC000
    reg[16] = 0x01  # planes 64 x 32 cells
    return VideoState(bytes(reg), bytes(0x10000), (0,) * 64, (0,) * 40)


LOG = """\
5 100 224 00A123 6 dma-vram C400 0140 FF8000
5 100 224 00A123 6 dma-vram C000 0190 FF9000
5 100 30 0014D2 4 reg 000A 001E -
5 100 30 0014D6 4 vsram 0000 009D -
5 101 10 00B000 0 vram 2000 1234 -
5 101 10 00B000 0 vram 2002 5678 -
6 102 224 00A123 6 dma-vram C400 0140 FF8000
"""


def test_read_video_log(tmp_path):
    path = tmp_path / "v.txt"
    path.write_text(LOG)
    events = read_video_log(path)
    assert len(events) == 7
    first = events[0]
    assert (first.handoff, first.frame, first.line, first.pc, first.level) == (
        5,
        100,
        224,
        0xA123,
        6,
    )
    assert (first.kind, first.target, first.value, first.source) == (
        "dma-vram",
        0xC400,
        0x140,
        0xFF8000,
    )
    assert events[2].source is None


def test_read_video_log_names_a_bad_line(tmp_path):
    path = tmp_path / "v.txt"
    path.write_text(LOG + "nonsense\n")
    with pytest.raises(VideoLogError, match="line 8"):
        read_video_log(path)


def test_summary_names_the_sprite_and_scroll_tables(tmp_path):
    path = tmp_path / "v.txt"
    path.write_text(LOG)
    rows = summarise(read_video_log(path), state())
    by_region = {row.region: row for row in rows}
    assert by_region["sprite table"].events == 2 and by_region["sprite table"].words == 0x280
    assert by_region["sprite table"].sources == (0xFF8000,)
    assert by_region["horizontal scroll table"].words == 0x190
    assert by_region["tile patterns"].words == 2
    assert by_region["register 10"].context == "line interrupt"
    assert by_region["vertical scroll"].events == 1


def test_writes_made_with_interrupts_masked_are_not_called_main_loop(tmp_path):
    path = tmp_path / "v.txt"
    path.write_text("5 100 227 012300 7 dma-vram C000 0340 FF3A40\n")
    (row,) = summarise(read_video_log(path), state())
    assert row.context == "interrupts masked"


def test_summary_groups_by_writer(tmp_path):
    path = tmp_path / "v.txt"
    path.write_text(LOG)
    rows = summarise(read_video_log(path), state(), routine_of=lambda pc: pc & ~0xFFF)
    writers = {(row.writer, row.region) for row in rows}
    assert (0xA000, "sprite table") in writers and (0xA000, "horizontal scroll table") in writers
    assert (0x1000, "register 10") in writers and (0x1000, "vertical scroll") in writers
    assert len(rows) == 5
