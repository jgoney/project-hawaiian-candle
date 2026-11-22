"""Summarising rr-race-replay's --ram-writes: which instructions write a range of work RAM."""

import pytest

from rr2port.ram_writes import RamWriteLogError, read_ram_writes, writers

LOG = """\
5 100 00A100 FF3A40 2 0150
5 100 00A100 FF3A44 2 0151
5 100 00A100 FF3A48 2 0152
5 100 00B200 FF3A42 2 03FF
6 105 00A100 FF3A40 2 0160
6 105 00C300 FF3DC0 4 12345678
"""


def test_read_ram_writes(tmp_path):
    path = tmp_path / "w.txt"
    path.write_text(LOG)
    writes = read_ram_writes(path)
    assert len(writes) == 6
    assert (writes[0].handoff, writes[0].frame, writes[0].pc) == (5, 100, 0xA100)
    assert (writes[0].address, writes[0].size, writes[0].value) == (0xFF3A40, 2, 0x150)


def test_a_bad_line_is_named(tmp_path):
    path = tmp_path / "w.txt"
    path.write_text(LOG + "5 100 zz\n")
    with pytest.raises(RamWriteLogError, match="line 7"):
        read_ram_writes(path)


def test_writers_groups_by_instruction(tmp_path):
    path = tmp_path / "w.txt"
    path.write_text(LOG)
    found = writers(read_ram_writes(path))
    assert sorted(found) == [0xA100, 0xB200, 0xC300]
    a = found[0xA100]
    assert (a.first, a.last, a.writes, a.size) == (0xFF3A40, 0xFF3A49, 4, 2)
    assert a.passes == 2 and a.stride == 4
    assert found[0xC300].last == 0xFF3DC3 and found[0xC300].stride is None
