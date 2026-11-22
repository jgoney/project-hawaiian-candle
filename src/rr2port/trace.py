"""Readers for MAME reference trace windows used by the port conformance checks."""

from __future__ import annotations

import re
import sys
from collections.abc import Iterator, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

DEFAULT_VECTORS: Mapping[int, int] = {4: 0x14BE, 6: 0x11B8}
START_MARKER = "RR2START"
END_MARKER = "RR2END"
HW_PREFIX = "RR2HW "
END_LINE_PREFIX = "E "
_DISASM = re.compile(r"^([0-9A-Fa-f]{6}): ")


class TraceFormatError(ValueError):
    """Raised when a trace or hardware log line cannot be parsed."""

    def __init__(self, line_number: int, reason: str) -> None:
        super().__init__(f"line {line_number}: {reason}")
        self.line_number = line_number
        self.reason = reason


@dataclass(frozen=True)
class TraceStep:
    pc: int
    sr: int
    d: tuple[int, ...]
    a: tuple[int, ...]
    cycles: int


@dataclass(frozen=True)
class Interrupt:
    index: int
    level: int


@dataclass(frozen=True)
class HwAccess:
    kind: str
    addr: int
    size: int
    value: int


def _parse_step(line: str, line_number: int) -> TraceStep:
    fields = line.split()
    if len(fields) != 20 or fields[0] != "T":
        raise TraceFormatError(line_number, f"expected 'T' and 19 fields, found {len(fields)}")
    try:
        pc = int(fields[1], 16)
        sr = int(fields[2], 16)
        regs = tuple(int(value, 16) for value in fields[3:19])
        cycles = int(fields[19])
    except ValueError as error:
        raise TraceFormatError(line_number, str(error)) from None
    return TraceStep(pc=pc, sr=sr, d=regs[:8], a=regs[8:], cycles=cycles)


def read_trace(path: Path) -> Iterator[TraceStep]:
    """Yield one step per line of a processed ``trace.txt``.

    The file must end with ``E <count>``. A file without it was cut short, and
    that is an error even when the cut falls between two lines.
    """
    count = 0
    line_number = 0
    with path.open() as handle:
        for line_number, line in enumerate(handle, 1):
            if line.startswith(END_LINE_PREFIX):
                fields = line.split()
                if len(fields) != 2 or not fields[1].isdigit() or int(fields[1]) != count:
                    raise TraceFormatError(
                        line_number, f"end line {line.strip()!r} does not match {count} steps"
                    )
                if handle.readline():
                    raise TraceFormatError(line_number + 1, "content after the end line")
                return
            yield _parse_step(line, line_number)
            count += 1
    raise TraceFormatError(line_number + 1, "missing end line; the trace is truncated")


def find_interrupts(
    steps: Sequence[TraceStep], vectors: Mapping[int, int] = DEFAULT_VECTORS
) -> list[Interrupt]:
    """Find the steps that are the first instruction of an interrupt handler.

    A step counts as an interrupt entry when its PC is a vector target and the
    interrupt mask equals that vector's level. The previous step's mask is no
    guide: it may be lower (ordinary entry), higher (the instruction lowered it
    from 7 with an interrupt pending) or equal (an RTE followed at once by the
    next interrupt). This assumes no code branches to a handler's first
    instruction, which holds for RR206; rr-trace-check uses machine state and
    does not rely on it.
    """
    level_for_target = {target: level for level, target in vectors.items()}
    found = []
    for index in range(1, len(steps)):
        current = steps[index]
        level = level_for_target.get(current.pc)
        if level is not None and (current.sr >> 8) & 7 == level:
            found.append(Interrupt(index=index, level=level))
    return found


def read_hw_log(path: Path) -> list[HwAccess]:
    """Read a processed ``hw.txt``."""
    accesses = []
    with path.open() as handle:
        for line_number, line in enumerate(handle, 1):
            fields = line.split()
            if len(fields) != 4 or fields[0] not in ("R", "W"):
                raise TraceFormatError(line_number, "expected 'R|W addr size value'")
            try:
                addr, size, value = int(fields[1], 16), int(fields[2]), int(fields[3], 16)
            except ValueError as error:
                raise TraceFormatError(line_number, str(error)) from None
            if size not in (1, 2):
                raise TraceFormatError(line_number, f"unsupported access size {size}")
            accesses.append(HwAccess(kind=fields[0], addr=addr, size=size, value=value))
    return accesses


def split_raw_trace(raw_path: Path, out_path: Path) -> int:
    """Convert MAME's raw trace into ``trace.txt`` and return the number of steps.

    The raw file alternates a register line written by the trace action with
    MAME's disassembly line, whose address is the instruction's true PC. Kept
    steps run from the pair just before ``RR2START`` to the pair just before
    ``RR2END``, inclusive.
    The output ends with ``E <count>`` so a truncated copy can be
    recognised.
    """
    pending: str | None = None
    pending_line = 0
    last: str | None = None
    started = False
    count = 0
    with raw_path.open() as raw, out_path.open("w") as out:
        for line_number, line in enumerate(raw, 1):
            line = line.rstrip("\n")
            if line.startswith("T "):
                pending, pending_line = line, line_number
                continue
            match = _DISASM.match(line)
            if match and pending is not None:
                fields = pending.split()
                if len(fields) != 19:
                    raise TraceFormatError(pending_line, "raw register line needs 18 values")
                step = f"T {match.group(1).upper()} {' '.join(fields[1:])}"
                pending = None
                if started and last is not None:
                    out.write(last + "\n")
                    count += 1
                last = step
                continue
            if line == START_MARKER:
                if last is None:
                    raise TraceFormatError(line_number, "start marker before any instruction")
                started = True
                continue
            if line == END_MARKER:
                if not started or last is None:
                    raise TraceFormatError(line_number, "end marker before start marker")
                out.write(last + "\n")
                out.write(f"{END_LINE_PREFIX}{count + 1}\n")
                return count + 1
    raise TraceFormatError(0, "raw trace has no complete RR2START..RR2END window")


def split_hw_log(error_log: Path, out_path: Path) -> int:
    """Extract ``RR2HW`` rows from MAME's error log into ``hw.txt``."""
    count = 0
    with error_log.open(errors="replace") as log, out_path.open("w") as out:
        for line_number, line in enumerate(log, 1):
            if not line.startswith(HW_PREFIX):
                continue
            fields = line.split()
            if len(fields) != 5 or fields[1] not in ("R", "W"):
                raise TraceFormatError(line_number, "malformed RR2HW row")
            try:
                bits = int(fields[3], 16)
                int(fields[2], 16)
                int(fields[4], 16)
            except ValueError as error:
                raise TraceFormatError(line_number, str(error)) from None
            if bits not in (8, 16):
                raise TraceFormatError(line_number, f"unsupported access width {bits} bits")
            out.write(f"{fields[1]} {fields[2].upper()} {bits // 8} {fields[4].upper()}\n")
            count += 1
    return count


def main(argv: Sequence[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if len(args) != 3:
        print("usage: python -m rr2port.trace RAW_TRACE ERROR_LOG OUT_DIR", file=sys.stderr)
        return 2
    raw, error_log, out_dir = Path(args[0]), Path(args[1]), Path(args[2])
    try:
        steps = split_raw_trace(raw, out_dir / "trace.txt")
        accesses = split_hw_log(error_log, out_dir / "hw.txt")
    except TraceFormatError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(f"steps={steps} hardware_accesses={accesses}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
