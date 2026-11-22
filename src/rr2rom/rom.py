"""Parsers and validation for linear, big-endian Sega Genesis ROM images."""

from __future__ import annotations

import hashlib
import struct
import zlib
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

HEADER_OFFSET = 0x100
HEADER_END = 0x200
CHECKSUM_START = 0x200
VECTOR_COUNT = 64
GENESIS_RAM_START = 0xFF0000
GENESIS_RAM_END = 0xFFFFFF

VECTOR_NAMES = (
    "initial_stack_pointer",
    "reset",
    "bus_error",
    "address_error",
    "illegal_instruction",
    "division_by_zero",
    "chk",
    "trapv",
    "privilege_violation",
    "trace",
    "line_1010_emulator",
    "line_1111_emulator",
    "reserved_12",
    "coprocessor_protocol_violation",
    "format_error",
    "uninitialized_interrupt",
    "reserved_16",
    "reserved_17",
    "reserved_18",
    "reserved_19",
    "reserved_20",
    "reserved_21",
    "reserved_22",
    "reserved_23",
    "spurious_interrupt",
    "interrupt_level_1",
    "interrupt_level_2",
    "interrupt_level_3",
    "interrupt_level_4_hblank",
    "interrupt_level_5",
    "interrupt_level_6_vblank",
    "interrupt_level_7",
    *(f"trap_{number}" for number in range(16)),
    *(f"reserved_{number}" for number in range(48, 64)),
)


class RomFormatError(ValueError):
    """Raised when an input is too short to contain required ROM structures."""


def _ascii(raw: bytes) -> str:
    return raw.decode("ascii", errors="replace").rstrip(" \0")


def _u16(data: bytes, offset: int) -> int:
    return struct.unpack_from(">H", data, offset)[0]


def _u32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


@dataclass(frozen=True)
class GenesisHeader:
    system_type: str
    copyright: str
    domestic_title: str
    international_title: str
    serial_and_revision: str
    checksum: int
    io_support: str
    rom_start: int
    rom_end: int
    ram_start: int
    ram_end: int
    backup_ram_info_hex: str
    modem_info: str
    memo: str
    regions: str

    @classmethod
    def parse(cls, data: bytes) -> GenesisHeader:
        if len(data) < HEADER_END:
            raise RomFormatError(
                f"ROM is {len(data)} bytes; at least {HEADER_END} bytes are required"
            )
        return cls(
            system_type=_ascii(data[0x100:0x110]),
            copyright=_ascii(data[0x110:0x120]),
            domestic_title=_ascii(data[0x120:0x150]),
            international_title=_ascii(data[0x150:0x180]),
            serial_and_revision=_ascii(data[0x180:0x18E]),
            checksum=_u16(data, 0x18E),
            io_support=_ascii(data[0x190:0x1A0]),
            rom_start=_u32(data, 0x1A0),
            rom_end=_u32(data, 0x1A4),
            ram_start=_u32(data, 0x1A8),
            ram_end=_u32(data, 0x1AC),
            backup_ram_info_hex=data[0x1B0:0x1BC].hex(),
            modem_info=_ascii(data[0x1BC:0x1C8]),
            memo=_ascii(data[0x1C8:0x1F0]),
            regions=_ascii(data[0x1F0:0x200]),
        )


@dataclass(frozen=True)
class Vector:
    index: int
    offset: int
    name: str
    address: int


@dataclass(frozen=True)
class Hashes:
    crc32: str
    sha1: str
    sha256: str


@dataclass(frozen=True)
class Validation:
    size_is_even: bool
    header_signature_present: bool
    declared_rom_range_matches_size: bool
    declared_ram_range_is_genesis_work_ram: bool
    reset_vector_is_even: bool
    reset_vector_is_in_rom: bool
    checksum_matches: bool
    warnings: tuple[str, ...]


@dataclass(frozen=True)
class RomInspection:
    size: int
    header: GenesisHeader
    vectors: tuple[Vector, ...]
    hashes: Hashes
    calculated_checksum: int
    validation: Validation

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


def parse_vectors(data: bytes) -> tuple[Vector, ...]:
    required = VECTOR_COUNT * 4
    if len(data) < required:
        raise RomFormatError(f"ROM is {len(data)} bytes; vector table requires {required}")
    return tuple(
        Vector(index, index * 4, VECTOR_NAMES[index], _u32(data, index * 4))
        for index in range(VECTOR_COUNT)
    )


def standard_checksum(data: bytes) -> int:
    """Return the standard Genesis 16-bit word sum over bytes 0x200 through EOF."""
    payload = data[CHECKSUM_START:]
    if len(payload) % 2:
        payload += b"\x00"
    return sum(struct.unpack(f">{len(payload) // 2}H", payload)) & 0xFFFF if payload else 0


def inspect_rom_bytes(data: bytes) -> RomInspection:
    header = GenesisHeader.parse(data)
    vectors = parse_vectors(data)
    calculated_checksum = standard_checksum(data)
    reset = vectors[1].address
    warnings: list[str] = []

    checks = {
        "size_is_even": len(data) % 2 == 0,
        "header_signature_present": header.system_type.startswith(
            ("SEGA GENESIS", "SEGA MEGA DRIVE")
        ),
        "declared_rom_range_matches_size": (
            header.rom_start == 0 and header.rom_end + 1 == len(data)
        ),
        "declared_ram_range_is_genesis_work_ram": (
            header.ram_start == GENESIS_RAM_START and header.ram_end == GENESIS_RAM_END
        ),
        "reset_vector_is_even": reset % 2 == 0,
        "reset_vector_is_in_rom": 0 <= reset < len(data),
        "checksum_matches": header.checksum == calculated_checksum,
    }
    warning_messages = {
        "size_is_even": "ROM size is odd",
        "header_signature_present": "Genesis header signature is missing",
        "declared_rom_range_matches_size": "declared ROM range does not match file size",
        "declared_ram_range_is_genesis_work_ram": "declared RAM range is nonstandard",
        "reset_vector_is_even": "reset vector is odd",
        "reset_vector_is_in_rom": "reset vector is outside the ROM",
        "checksum_matches": "stored checksum does not match the standard checksum",
    }
    for name, passed in checks.items():
        if not passed:
            warnings.append(warning_messages[name])

    return RomInspection(
        size=len(data),
        header=header,
        vectors=vectors,
        hashes=Hashes(
            crc32=f"{zlib.crc32(data) & 0xFFFFFFFF:08X}",
            sha1=hashlib.sha1(data).hexdigest(),  # noqa: S324 - identification, not security
            sha256=hashlib.sha256(data).hexdigest(),
        ),
        calculated_checksum=calculated_checksum,
        validation=Validation(**checks, warnings=tuple(warnings)),
    )


def inspect_rom(path: str | Path) -> RomInspection:
    """Read and inspect a ROM without opening it for writing."""
    return inspect_rom_bytes(Path(path).read_bytes())
