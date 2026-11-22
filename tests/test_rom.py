from __future__ import annotations

import hashlib
import struct
import zlib

import pytest

from rr2rom.rom import GenesisHeader, RomFormatError, inspect_rom_bytes, standard_checksum
from rr2rom.text import TextRun, fixed_ascii_records, printable_runs, unique_text


def make_rom(*, stored_checksum: int | None = None) -> bytes:
    rom = bytearray(0x400)
    struct.pack_into(">II", rom, 0, 0x00FFFFF6, 0x00000200)
    struct.pack_into(">I", rom, 28 * 4, 0x000014BE)
    struct.pack_into(">I", rom, 30 * 4, 0x000011B8)
    rom[0x100:0x110] = b"SEGA GENESIS    "
    rom[0x110:0x120] = b"(C)TEST 2026    "
    rom[0x120:0x150] = b"TEST DOMESTIC".ljust(48)
    rom[0x150:0x180] = b"TEST INTERNATIONAL".ljust(48)
    rom[0x180:0x18E] = b"GM 00000000-00"
    rom[0x190:0x1A0] = b"J".ljust(16)
    struct.pack_into(">IIII", rom, 0x1A0, 0, len(rom) - 1, 0xFF0000, 0xFFFFFF)
    rom[0x1F0:0x200] = b"UEJ".ljust(16)
    for offset in range(0x200, len(rom), 2):
        struct.pack_into(">H", rom, offset, offset & 0xFFFF)
    checksum = standard_checksum(bytes(rom)) if stored_checksum is None else stored_checksum
    struct.pack_into(">H", rom, 0x18E, checksum)
    return bytes(rom)


def test_header_and_vectors_are_big_endian() -> None:
    report = inspect_rom_bytes(make_rom())

    assert report.header.system_type == "SEGA GENESIS"
    assert report.header.serial_and_revision == "GM 00000000-00"
    assert report.header.rom_end == 0x3FF
    assert report.vectors[0].address == 0x00FFFFF6
    assert report.vectors[1].address == 0x200
    assert report.vectors[28].name == "interrupt_level_4_hblank"
    assert report.vectors[28].address == 0x14BE
    assert report.vectors[30].address == 0x11B8
    assert report.validation.checksum_matches


def test_checksum_sums_big_endian_words_from_0x200() -> None:
    data = bytes(0x200) + b"\x12\x34\xAB\xCD"
    assert standard_checksum(data) == 0xBE01


def test_checksum_zero_pads_an_odd_final_byte() -> None:
    assert standard_checksum(bytes(0x200) + b"\x12\x34\x56") == 0x6834


def test_mismatching_checksum_is_reported_not_repaired() -> None:
    rom = make_rom(stored_checksum=0x1234)
    report = inspect_rom_bytes(rom)

    assert report.header.checksum == 0x1234
    assert not report.validation.checksum_matches
    assert "stored checksum does not match the standard checksum" in report.validation.warnings
    assert rom[0x18E:0x190] == b"\x12\x34"


def test_hashes_match_standard_library() -> None:
    rom = make_rom()
    report = inspect_rom_bytes(rom)

    assert report.hashes.crc32 == f"{zlib.crc32(rom):08X}"
    assert report.hashes.sha1 == hashlib.sha1(rom).hexdigest()
    assert report.hashes.sha256 == hashlib.sha256(rom).hexdigest()


@pytest.mark.parametrize("size", [0, 0xFF, 0x1FF])
def test_short_rom_is_rejected(size: int) -> None:
    with pytest.raises(RomFormatError):
        GenesisHeader.parse(bytes(size))


def test_printable_text_runs_keep_offsets() -> None:
    data = b"\x00ABCD\x01too short\x00LAST!"

    assert printable_runs(data, minimum_length=5) == (
        TextRun(6, "too short"),
        TextRun(16, "LAST!"),
    )


def test_fixed_records_and_unique_names() -> None:
    data = b"xxALPHA   yyalpha   zzBETA    "
    records = fixed_ascii_records(data, first_text=2, stride=10, text_length=8, count=3)

    assert [record.offset for record in records] == [2, 12, 22]
    assert unique_text(records) == ("ALPHA", "BETA")
