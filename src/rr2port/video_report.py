"""Summarise rr-race-replay's --video-log: which code writes which part of video memory.

The game prepares most of what it shows in work RAM and has the video chip
copy it across ("DMA"). Grouping the writes by the instruction that made
them, and naming the part of video memory they land in, shows which routines
own the sprite list, the per-line scroll table, the tile sheets and the tile
pictures, and where in work RAM each is staged.
"""

from __future__ import annotations

from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path

from rr2port.picture_report import VideoState

KINDS = {"reg", "vram", "cram", "vsram", "dma-vram", "dma-cram", "dma-vsram", "fill", "copy"}
# By the CPU's interrupt level at the time of the write. Level 7 is code that has
# shut interrupts out; in the race that is the frame interrupt handler's block copies.
CONTEXTS = {4: "line interrupt", 6: "frame interrupt", 7: "interrupts masked"}


class VideoLogError(ValueError):
    """A --video-log file has a line that is not a log line."""


@dataclass(frozen=True)
class VideoEvent:
    handoff: int
    frame: int
    line: int
    pc: int
    level: int
    kind: str
    target: int
    value: int
    source: int | None


@dataclass(frozen=True)
class Row:
    writer: int  # address of the writing instruction (or of its routine)
    context: str  # main loop, line interrupt or frame interrupt
    region: str  # what part of the video chip is written
    events: int
    words: int  # words written (bytes for fill and copy)
    first_target: int
    last_target: int
    sources: tuple[int, ...]  # distinct 68000 addresses DMA copied from
    lines: tuple[int, int]  # earliest and latest screen line at which it happened


def read_video_log(path: Path) -> list[VideoEvent]:
    events = []
    for number, text in enumerate(Path(path).read_text().splitlines(), start=1):
        fields = text.split()
        try:
            if len(fields) != 9 or fields[5] not in KINDS:
                raise ValueError(text)
            events.append(
                VideoEvent(
                    handoff=int(fields[0]),
                    frame=int(fields[1]),
                    line=int(fields[2]),
                    pc=int(fields[3], 16),
                    level=int(fields[4]),
                    kind=fields[5],
                    target=int(fields[6], 16),
                    value=int(fields[7], 16),
                    source=None if fields[8] == "-" else int(fields[8], 16),
                )
            )
        except ValueError as error:
            raise VideoLogError(f"{path}: line {number} is not a video log line: {text}") from error
    return events


def _plane_cells(bits: int) -> int:
    return {0: 32, 1: 64}.get(bits, 128)


def region_of(event: VideoEvent, state: VideoState) -> str:
    """Name the part of the video chip an event writes, from the table addresses in `state`."""
    if event.kind == "reg":
        return f"register {event.target}"
    if event.kind in ("cram", "dma-cram"):
        return "colours"
    if event.kind in ("vsram", "dma-vsram"):
        return "vertical scroll"
    plane_bytes = _plane_cells(state.reg[16] & 3) * _plane_cells((state.reg[16] >> 4) & 3) * 2
    tables = (
        ("sprite table", state.sprite_table, 80 * 8),
        ("horizontal scroll table", state.hscroll_table, 224 * 4),
        ("plane A sheet", state.plane_a_table, plane_bytes),
        ("plane B sheet", state.plane_b_table, plane_bytes),
        ("window sheet", state.window_table, 64 * 32 * 2),
    )
    for name, base, size in tables:
        if base <= event.target < base + size:
            return name
    return "tile patterns"


def summarise(
    events: Sequence[VideoEvent],
    state: VideoState,
    routine_of: Callable[[int], int] = lambda pc: pc,
) -> list[Row]:
    groups: dict[tuple[int, str, str], list[VideoEvent]] = {}
    for event in events:
        context = CONTEXTS.get(event.level, "main loop")
        groups.setdefault((routine_of(event.pc), context, region_of(event, state)), []).append(
            event
        )
    rows = []
    for (writer, context, region), members in groups.items():
        block = [e for e in members if e.kind.startswith("dma") or e.kind in ("fill", "copy")]
        words = sum(e.value for e in block) + len(members) - len(block)
        rows.append(
            Row(
                writer=writer,
                context=context,
                region=region,
                events=len(members),
                words=words,
                first_target=min(e.target for e in members),
                last_target=max(e.target for e in members),
                sources=tuple(sorted({e.source for e in members if e.kind.startswith("dma")})),
                lines=(min(e.line for e in members), max(e.line for e in members)),
            )
        )
    return sorted(rows, key=lambda row: (row.context, row.writer, row.region))


def format_summary(rows: Sequence[Row], passes: int) -> str:
    out = [f"VIDEO WRITES over {passes} passes, by writing instruction"]
    for row in rows:
        sources = ""
        if row.sources:
            shown = " ".join(f"{s:06X}" for s in row.sources[:6])
            more = f" (+{len(row.sources) - 6} more)" if len(row.sources) > 6 else ""
            sources = f" from {shown}{more}"
        out.append(
            f"  {row.writer:06X} {row.context:15s} {row.region:24s} events={row.events:6d} "
            f"words={row.words:7d} target={row.first_target:04X}..{row.last_target:04X} "
            f"lines={row.lines[0]}..{row.lines[1]}{sources}"
        )
    return "\n".join(out) + "\n"
