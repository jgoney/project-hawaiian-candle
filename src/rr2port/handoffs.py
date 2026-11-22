"""Work RAM at every game-loop handoff, stored as changes from the previous one.

File format: ASCII ``RR2HAND1``, then for each handoff in order a big-endian
``u32 index`` and ``u32 changed_count`` followed by ``changed_count`` records
of ``u16 offset, u8 value``. Handoff 0 is relative to an all-zero image, so the
file needs nothing else to be read.
"""

from __future__ import annotations

import struct
from collections.abc import Iterable, Iterator
from pathlib import Path

MAGIC = b"RR2HAND1"
RAM_SIZE = 0x10000
_HEADER = struct.Struct(">II")
_CHANGE = struct.Struct(">HB")


class HandoffFormatError(ValueError):
    """Raised when a handoff file is malformed. ``index`` is the handoff involved."""

    def __init__(self, index: int, reason: str) -> None:
        super().__init__(f"handoff {index}: {reason}")
        self.index = index
        self.reason = reason


def write_handoffs(path: Path, images: Iterable[bytes]) -> int:
    """Write full RAM images as a handoff file. Returns the number written."""
    previous = bytes(RAM_SIZE)
    count = 0
    with path.open("wb") as out:
        out.write(MAGIC)
        for image in images:
            if len(image) != RAM_SIZE:
                raise ValueError(f"handoff {count}: image is {len(image)} bytes, not {RAM_SIZE}")
            changes = [offset for offset in range(RAM_SIZE) if image[offset] != previous[offset]]
            out.write(_HEADER.pack(count, len(changes)))
            out.write(b"".join(_CHANGE.pack(offset, image[offset]) for offset in changes))
            previous = image
            count += 1
    return count


def iter_handoffs(path: Path) -> Iterator[tuple[int, bytes]]:
    """Yield (index, full 64 KiB image) for each handoff in the file."""
    ram = bytearray(RAM_SIZE)
    with path.open("rb") as handle:
        if handle.read(len(MAGIC)) != MAGIC:
            raise HandoffFormatError(0, "not an RR2HAND1 file")
        expected = 0
        while True:
            header = handle.read(_HEADER.size)
            if not header:
                return
            if len(header) != _HEADER.size:
                raise HandoffFormatError(expected, "truncated record header")
            index, changed = _HEADER.unpack(header)
            if index != expected:
                raise HandoffFormatError(expected, f"found index {index} instead")
            body = handle.read(changed * _CHANGE.size)
            if len(body) != changed * _CHANGE.size:
                raise HandoffFormatError(index, "truncated record")
            for offset, value in _CHANGE.iter_unpack(body):
                ram[offset] = value
            yield index, bytes(ram)
            expected += 1


def handoff_count(path: Path) -> int:
    """Number of handoffs in the file; the whole file is validated."""
    return sum(1 for _ in iter_handoffs(path))
