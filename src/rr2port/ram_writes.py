"""Summarise rr-race-replay's --ram-writes: which instructions write a range of work RAM."""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path


class RamWriteLogError(ValueError):
    """A --ram-writes file has a line that is not a log line."""


@dataclass(frozen=True)
class RamWrite:
    handoff: int
    frame: int
    pc: int
    address: int
    size: int
    value: int


@dataclass(frozen=True)
class Writer:
    pc: int
    first: int  # lowest address written
    last: int  # highest address written (last byte)
    writes: int
    size: int  # bytes per write (the commonest)
    passes: int  # passes in which it wrote at all
    stride: int | None  # commonest step between consecutive writes, if it writes more than once


def read_ram_writes(path: Path) -> list[RamWrite]:
    writes = []
    for number, text in enumerate(Path(path).read_text().splitlines(), start=1):
        fields = text.split()
        try:
            if len(fields) != 6:
                raise ValueError(text)
            writes.append(
                RamWrite(
                    handoff=int(fields[0]),
                    frame=int(fields[1]),
                    pc=int(fields[2], 16),
                    address=int(fields[3], 16),
                    size=int(fields[4]),
                    value=int(fields[5], 16),
                )
            )
        except ValueError as error:
            raise RamWriteLogError(
                f"{path}: line {number} is not a RAM write line: {text}"
            ) from error
    return writes


def _commonest(values: Sequence[int]) -> int:
    return max(set(values), key=values.count)


def writers(writes: Sequence[RamWrite]) -> dict[int, Writer]:
    by_pc: dict[int, list[RamWrite]] = {}
    for write in writes:
        by_pc.setdefault(write.pc, []).append(write)
    found = {}
    for pc, group in by_pc.items():
        steps = [
            b.address - a.address
            for a, b in zip(group, group[1:], strict=False)
            if a.handoff == b.handoff and b.address != a.address
        ]
        found[pc] = Writer(
            pc=pc,
            first=min(w.address for w in group),
            last=max(w.address + w.size - 1 for w in group),
            writes=len(group),
            size=_commonest([w.size for w in group]),
            passes=len({w.handoff for w in group}),
            stride=_commonest(steps) if steps else None,
        )
    return found


def format_writers(found: dict[int, Writer]) -> str:
    out = ["RAM WRITERS by instruction"]
    for pc in sorted(found):
        w = found[pc]
        stride = "-" if w.stride is None else str(w.stride)
        out.append(
            f"  {pc:06X} writes={w.writes:7d} passes={w.passes:4d} size={w.size} "
            f"range={w.first:06X}..{w.last:06X} stride={stride}"
        )
    return "\n".join(out) + "\n"
