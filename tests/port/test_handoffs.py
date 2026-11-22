from __future__ import annotations

import struct

import pytest

from rr2port.handoffs import HandoffFormatError, handoff_count, iter_handoffs, write_handoffs

RAM = 0x10000


def image(changes: dict[int, int]) -> bytes:
    data = bytearray(RAM)
    for offset, value in changes.items():
        data[offset] = value
    return bytes(data)


IMAGES = [image({0: 1}), image({0: 1, 0x1C08: 5}), image({0x1C08: 6, 0xFFFF: 0xAA})]


def test_round_trip(tmp_path):
    path = tmp_path / "handoffs.bin"
    assert write_handoffs(path, IMAGES) == 3
    assert list(iter_handoffs(path)) == list(enumerate(IMAGES))
    assert handoff_count(path) == 3


def test_file_is_small_because_only_changes_are_stored(tmp_path):
    path = tmp_path / "handoffs.bin"
    write_handoffs(path, IMAGES)
    assert path.stat().st_size < 200


def test_bad_magic(tmp_path):
    path = tmp_path / "handoffs.bin"
    path.write_bytes(b"NOTRIGHT" + bytes(16))
    with pytest.raises(HandoffFormatError):
        list(iter_handoffs(path))


def test_truncated_final_record_names_its_index(tmp_path):
    path = tmp_path / "handoffs.bin"
    write_handoffs(path, IMAGES)
    path.write_bytes(path.read_bytes()[:-2])
    with pytest.raises(HandoffFormatError) as e:
        list(iter_handoffs(path))
    assert e.value.index == 2
    with pytest.raises(HandoffFormatError):
        handoff_count(path)


def test_index_out_of_sequence(tmp_path):
    path = tmp_path / "handoffs.bin"
    path.write_bytes(b"RR2HAND1" + struct.pack(">II", 1, 0))
    with pytest.raises(HandoffFormatError) as e:
        list(iter_handoffs(path))
    assert e.value.index == 0


def test_image_of_the_wrong_size_is_rejected_on_write(tmp_path):
    with pytest.raises(ValueError):
        write_handoffs(tmp_path / "handoffs.bin", [bytes(100)])
