"""Text-resource extraction helpers for RR206."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class TextRun:
    offset: int
    text: str


def printable_runs(
    data: bytes, *, start: int = 0, end: int | None = None, minimum_length: int = 4
) -> tuple[TextRun, ...]:
    """Return contiguous printable-ASCII runs with their ROM offsets."""
    if minimum_length < 1:
        raise ValueError("minimum_length must be positive")
    stop = len(data) if end is None else min(end, len(data))
    start = max(start, 0)
    runs: list[TextRun] = []
    run_start: int | None = None

    for offset in range(start, stop + 1):
        value = data[offset] if offset < stop else 0
        if 0x20 <= value <= 0x7E:
            if run_start is None:
                run_start = offset
            continue
        if run_start is not None and offset - run_start >= minimum_length:
            runs.append(TextRun(run_start, data[run_start:offset].decode("ascii")))
        run_start = None
    return tuple(runs)


def fixed_ascii_records(
    data: bytes, *, first_text: int, stride: int, text_length: int, count: int
) -> tuple[TextRun, ...]:
    """Extract fixed-width ASCII fields from regularly spaced records."""
    if min(first_text, stride, text_length, count) < 0 or stride == 0:
        raise ValueError("record arguments must be non-negative and stride must be positive")
    result: list[TextRun] = []
    for index in range(count):
        offset = first_text + index * stride
        end = offset + text_length
        if end > len(data):
            raise ValueError(f"record at 0x{offset:X} extends beyond input")
        text = data[offset:end].decode("ascii", errors="replace").rstrip(" \0")
        result.append(TextRun(offset, text))
    return tuple(result)


def unique_text(records: tuple[TextRun, ...]) -> tuple[str, ...]:
    """Preserve the first occurrence of each nonempty, case-normalized name."""
    seen: set[str] = set()
    names: list[str] = []
    for record in records:
        name = record.text.strip()
        key = name.casefold()
        if name and key not in seen:
            seen.add(key)
            names.append(name)
    return tuple(names)

