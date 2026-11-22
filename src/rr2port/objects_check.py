"""Check the object reader against what the game itself produced.

The inputs are files of work-RAM images taken by `rr-race-replay --ram-at-pc`
when the main loop reaches a chosen instruction, so each image shows finished
work:

* drawing: images at 0xA4EE (before the game draws the objects) and at 0xA5C6
  (after). `draw_objects` is run on each before image and its sprite list
  and sprite pictures are compared, for equality, with the after image;
* records: images at 0x16E8C (before the roadside records are made) and at
  0x17196 (after). Each roadside record is recomputed from its road point,
  and each record in the vehicle part of the table from the vehicle object
  at its track position.

A run can start part-way through a pass, so the after file may hold an image
whose before image was never taken. An after image is paired with a before
image only if it holds the same draw records.
"""

from __future__ import annotations

import struct
from collections.abc import Iterator
from dataclasses import dataclass, field
from pathlib import Path

from rr2port.handoffs import iter_handoffs
from rr2port.objects import (
    DRAW_ORDER,
    DRAW_RECORD_SIZE,
    DRAW_RECORDS,
    MAX_TILES,
    RECORD_DISTANCES,
    TILE_BYTES,
    draw_objects_detail,
    record_placement,
    roadside_placement,
    staged_sprites,
    vehicle_addresses,
    vehicle_placement,
)


@dataclass
class DrawingCheck:
    images: int = 0
    lists_identical: int = 0
    pictures_identical: int = 0
    first_differing: int | None = None
    unpaired: int = 0  # after images that matched no before image
    cases: dict = field(default_factory=dict)


@dataclass
class RecordCheck:
    images: int = 0
    roadside: int = 0
    roadside_identical: int = 0
    vehicles: int = 0
    vehicles_identical: int = 0
    vehicles_but_low_bits: int = 0  # identical except the low two bits of the foot line
    unpaired: int = 0


def _records(ram: bytes) -> bytes:
    """The draw order and every record in it: what identifies a pass's objects."""
    out = bytearray()
    for k in range(128):
        index = struct.unpack_from(">H", ram, DRAW_ORDER + 2 * k)[0]
        out += index.to_bytes(2, "big")
        if index == 0xFFFF:
            break
        at = DRAW_RECORDS + DRAW_RECORD_SIZE * index
        out += ram[at : at + DRAW_RECORD_SIZE]
    return bytes(out)


def _frames(path: Path) -> list[int] | None:
    """The frame each image was taken in, from the .index file beside it, if there is one."""
    index = path.with_name(path.name + ".index")
    if not index.exists():
        return None
    return [int(line.split()[2]) for line in index.read_text().splitlines()]


def _pairs(before: Path, after: Path, same, result) -> Iterator[tuple[int, bytes, bytes]]:
    """(index, before image, after image) for each before image that has its after image."""
    taken, done = _frames(before), _frames(after)
    later = iter_handoffs(after)
    for index, first in iter_handoffs(before):
        tries = 0
        while tries < 2:  # at most one stray image is expected between two passes
            found = next(later, None)
            if found is None:
                return
            if taken is not None and done is not None and done[found[0]] < taken[index]:
                result.unpaired += 1  # taken before this pass began
                continue
            if same(first, found[1]):
                yield index, first, found[1]
                break
            result.unpaired += 1
            tries += 1


def _add(total: dict, cases: dict) -> None:
    for name, count in cases.items():
        if isinstance(count, dict):
            _add(total.setdefault(name, {}), count)
        else:
            total[name] = total.get(name, 0) + count


def check_drawing(before: Path, after: Path, rom: bytes, every: int = 1) -> DrawingCheck:
    result = DrawingCheck()
    same = lambda a, b: _records(a) == _records(b)  # noqa: E731
    for index, first, then in _pairs(before, after, same, result):
        if index % every:
            continue
        drawn = draw_objects_detail(first, rom)
        buffer = struct.unpack_from(">I", first, 0x27CE)[0] & 0xFFFF
        lists = drawn.sprites == staged_sprites(then)
        pictures = drawn.pictures == then[buffer : buffer + MAX_TILES * TILE_BYTES]
        result.images += 1
        result.lists_identical += lists
        result.pictures_identical += pictures
        if not (lists and pictures) and result.first_differing is None:
            result.first_differing = index
        _add(result.cases, drawn.cases)
    return result


def check_records(before: Path, after: Path, rom: bytes) -> RecordCheck:
    result = RecordCheck()

    def same(a: bytes, b: bytes) -> bool:
        made = struct.unpack_from(">H", a, 0xDC42)[0] * DRAW_RECORD_SIZE
        return a[DRAW_RECORDS : DRAW_RECORDS + made] == b[DRAW_RECORDS : DRAW_RECORDS + made]

    for _, first, then in _pairs(before, after, same, result):
        result.images += 1
        vehicles = struct.unpack_from(">H", first, 0xDC42)[0]
        total = struct.unpack_from(">H", then, 0xDC42)[0]
        for index in range(vehicles, total):
            result.roadside += 1
            result.roadside_identical += roadside_placement(then, rom, index) == record_placement(
                then, index
            )
        camera = struct.unpack_from(">I", then, 0x0A0A)[0]
        candidates = vehicle_addresses(then, rom)
        for index in range(vehicles):
            result.vehicles += 1
            held = record_placement(then, index)
            want = (held.x, held.foot_line, held.size, held.hidden_below)
            away = struct.unpack_from(">I", then, RECORD_DISTANCES + 4 * index)[0]
            position = (away + camera) & 0xFFFFFFFF
            placed = [
                vehicle_placement(then, rom, index, address)
                for address in candidates
                if struct.unpack_from(">I", then, address + 4)[0] == position
            ]
            found = [(p.x, p.foot_line, p.size, p.hidden_below) for p in placed if p is not None]
            if want in found:
                result.vehicles_identical += 1
            elif any(
                (p[0], p[1] & ~3, p[2], p[3]) == (want[0], want[1] & ~3, *want[2:]) for p in found
            ):
                result.vehicles_but_low_bits += 1
    return result


def _cases(cases: dict) -> str:
    parts = []
    for name, count in cases.items():
        if isinstance(count, dict):
            inner = ",".join(f"{key}:{value}" for key, value in sorted(count.items()))
            parts.append(f"{name}={inner}")
        else:
            parts.append(f"{name}={count}")
    return " ".join(parts)


def format_drawing(result: DrawingCheck) -> str:
    first = "none" if result.first_differing is None else result.first_differing
    return (
        f"SPRITE_LISTS identical={result.lists_identical} of {result.images}\n"
        f"SPRITE_PICTURES identical={result.pictures_identical} of {result.images} "
        f"first_differing={first} unpaired_images={result.unpaired}\n"
        f"CASES {_cases(result.cases)}\n"
    )


def format_records(result: RecordCheck) -> str:
    return (
        f"ROADSIDE_RECORDS identical={result.roadside_identical} of {result.roadside}\n"
        f"VEHICLE_RECORDS identical={result.vehicles_identical} of {result.vehicles} "
        f"identical_but_foot_line_low_bits={result.vehicles_but_low_bits} "
        f"images={result.images} unpaired_images={result.unpaired}\n"
    )
