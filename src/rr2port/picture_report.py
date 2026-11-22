"""Read what rr-race-replay writes about one frame of the picture.

The video chip builds the picture from layers: a backdrop colour, two
scrolling tile planes (B behind A), a fixed window plane that replaces part
of plane A, and sprites. The replay tool can write, for a frame, which layer
drew each pixel (`--sources-at`), the state each display line was drawn from
(`--line-state-at`) and the chip's memories at the end of the frame
(`--video-state-at`). This module reads those files and summarises them.
"""

from __future__ import annotations

import struct
from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

WIDTH = 320
HEIGHT = 224
LAYERS = ("backdrop", "plane B", "plane A", "window", "sprite")
STATE_MAGIC = b"RR2VDPSTATE1\n"
REGISTERS = 24
MAX_SPRITES = 80


class PictureFormatError(ValueError):
    """A picture file is not what its reader expects."""


@dataclass(frozen=True)
class PixelSource:
    layer: str
    colour: int  # 0..63, index into the colour memory
    detail: int  # sprite: its index in the sprite table; plane or window: the name-table entry


@dataclass(frozen=True)
class Band:
    """A run of display lines drawn by the same set of layers."""

    first_line: int
    last_line: int
    pixels: dict[str, int]  # pixels per layer over the whole band

    @property
    def layers(self) -> frozenset[str]:
        return frozenset(self.pixels)


@dataclass(frozen=True)
class VideoState:
    reg: bytes
    vram: bytes
    cram: tuple[int, ...]
    vsram: tuple[int, ...]

    @property
    def plane_a_table(self) -> int:
        return (self.reg[2] & 0x38) << 10

    @property
    def plane_b_table(self) -> int:
        return (self.reg[4] & 0x07) << 13

    @property
    def window_table(self) -> int:
        return (self.reg[3] & 0x3C) << 10

    @property
    def sprite_table(self) -> int:
        return (self.reg[5] & 0x7E) << 9

    @property
    def hscroll_table(self) -> int:
        return (self.reg[13] & 0x3F) << 10


@dataclass(frozen=True)
class Sprite:
    index: int  # position in the sprite table
    x: int  # screen position of the left edge (can be negative)
    y: int
    width: int  # pixels
    height: int
    tile: int  # first tile of its picture
    palette: int
    priority: bool
    flip_h: bool
    flip_v: bool
    link: int  # index of the sprite drawn next (behind this one)


@dataclass(frozen=True)
class LineState:
    line: int
    reg: bytes
    hscroll: tuple[int, int]  # plane A, plane B
    vscroll: tuple[int, int]


def read_sources(path: Path) -> list[list[PixelSource]]:
    data = Path(path).read_bytes()
    if len(data) != WIDTH * HEIGHT * 4:
        raise PictureFormatError(
            f"{path}: {len(data)} bytes, expected {WIDTH * HEIGHT * 4} (a --sources-at file)"
        )
    rows = []
    for line in range(HEIGHT):
        row = []
        for x in range(WIDTH):
            layer, colour, detail = struct.unpack_from(">BBH", data, (line * WIDTH + x) * 4)
            if layer >= len(LAYERS):
                raise PictureFormatError(f"{path}: unknown layer {layer} at line {line}, x {x}")
            row.append(PixelSource(LAYERS[layer], colour, detail))
        rows.append(row)
    return rows


def layer_bands(sources: Sequence[Sequence[PixelSource]]) -> list[Band]:
    bands: list[Band] = []
    for line, row in enumerate(sources):
        counts = Counter(pixel.layer for pixel in row)
        if bands and bands[-1].layers == frozenset(counts):
            last = bands[-1]
            merged = Counter(last.pixels)
            merged.update(counts)
            bands[-1] = Band(last.first_line, line, dict(merged))
        else:
            bands.append(Band(line, line, dict(counts)))
    return bands


def read_video_state(path: Path) -> VideoState:
    data = Path(path).read_bytes()
    expected = len(STATE_MAGIC) + REGISTERS + 0x10000 + 64 * 2 + 40 * 2
    if not data.startswith(STATE_MAGIC) or len(data) != expected:
        raise PictureFormatError(f"{path}: not an RR2VDPSTATE1 file of {expected} bytes")
    at = len(STATE_MAGIC)
    reg = data[at : at + REGISTERS]
    at += REGISTERS
    vram = data[at : at + 0x10000]
    at += 0x10000
    cram = struct.unpack_from(">64H", data, at)
    vsram = struct.unpack_from(">40H", data, at + 128)
    return VideoState(reg, vram, cram, vsram)


def sprites_on_screen(state: VideoState) -> list[Sprite]:
    """The sprite list in drawing order (first is in front), following the links."""
    sprites = []
    index = 0
    for _ in range(MAX_SPRITES):
        at = (state.sprite_table + index * 8) & 0xFFFF
        y, size, link, attr, x = struct.unpack_from(">HBBHH", state.vram.ljust(0x10008, b"\0"), at)
        link &= 0x7F
        sprites.append(
            Sprite(
                index=index,
                x=(x & 0x1FF) - 128,
                y=(y & 0x1FF) - 128,
                width=(((size >> 2) & 3) + 1) * 8,
                height=((size & 3) + 1) * 8,
                tile=attr & 0x07FF,
                palette=(attr >> 13) & 3,
                priority=bool(attr & 0x8000),
                flip_h=bool(attr & 0x0800),
                flip_v=bool(attr & 0x1000),
                link=link,
            )
        )
        if link == 0 or link >= MAX_SPRITES:
            break
        index = link
    return sprites


def read_line_state(path: Path) -> list[LineState]:
    states = []
    for number, text in enumerate(Path(path).read_text().splitlines()):
        fields = text.split()
        if len(fields) != 1 + REGISTERS + 4 or int(fields[0]) != number:
            raise PictureFormatError(f"{path}: line {number + 1} is not a --line-state-at line")
        values = [int(field, 16) for field in fields[1:]]
        states.append(
            LineState(
                line=number,
                reg=bytes(values[:REGISTERS]),
                hscroll=(values[REGISTERS], values[REGISTERS + 1]),
                vscroll=(values[REGISTERS + 2], values[REGISTERS + 3]),
            )
        )
    if len(states) != HEIGHT:
        raise PictureFormatError(f"{path}: {len(states)} lines, expected {HEIGHT}")
    return states


def register_changes(states: Sequence[LineState]) -> list[tuple[int, int, int, int]]:
    """(line, register, value before, value from this line on) for every change down the frame."""
    changes = []
    for before, after in zip(states, states[1:], strict=False):
        for reg in range(REGISTERS):
            if before.reg[reg] != after.reg[reg]:
                changes.append((after.line, reg, before.reg[reg], after.reg[reg]))
    return changes


def _runs(values: Sequence[int]) -> str:
    """Summarise a per-line value as runs: "0-99:0010 100-223:varies(124)"."""
    parts = []
    start = 0
    while start < len(values):
        end = start
        while end + 1 < len(values) and values[end + 1] == values[start]:
            end += 1
        if end - start >= 3 or (start == 0 and end == len(values) - 1):
            parts.append(f"{start}-{end}:{values[start]:04X}")
            start = end + 1
            continue
        # A stretch where the value changes almost every line.
        end = start
        while end + 1 < len(values) and not (
            end + 4 < len(values) and len(set(values[end + 1 : end + 5])) == 1
        ):
            end += 1
        distinct = len(set(values[start : end + 1]))
        low, high = min(values[start : end + 1]), max(values[start : end + 1])
        parts.append(f"{start}-{end}:varies({distinct} values, {low:04X}..{high:04X})")
        start = end + 1
    return " ".join(parts)


def format_report(
    sources: Sequence[Sequence[PixelSource]] | None,
    states: Sequence[LineState] | None,
    state: VideoState | None,
) -> str:
    out = []
    if sources is not None:
        out.append("LAYERS by band of lines (pixels per layer)")
        for band in layer_bands(sources):
            present = [name for name in LAYERS if name in band.pixels]
            shares = ", ".join(f"{name} {band.pixels[name]}" for name in present)
            out.append(f"  lines {band.first_line:3d}-{band.last_line:3d}: {shares}")
        used = Counter(pixel.detail for row in sources for pixel in row if pixel.layer == "sprite")
        out.append(f"SPRITES that show: {len(used)}")
    if states is not None:
        out.append("REGISTERS that change down the frame")
        changes = register_changes(states)
        for line, reg, before, after in changes:
            out.append(f"  line {line:3d}: register {reg} {before:02X} -> {after:02X}")
        if not changes:
            out.append("  none")
        out.append("SCROLL down the frame (lines:value)")
        out.append(f"  plane A horizontal: {_runs([s.hscroll[0] for s in states])}")
        out.append(f"  plane B horizontal: {_runs([s.hscroll[1] for s in states])}")
        out.append(f"  plane A vertical:   {_runs([s.vscroll[0] for s in states])}")
        out.append(f"  plane B vertical:   {_runs([s.vscroll[1] for s in states])}")
    if state is not None:
        out.append(
            "TABLES (video memory addresses): "
            f"plane A {state.plane_a_table:04X}, plane B {state.plane_b_table:04X}, "
            f"window {state.window_table:04X}, sprites {state.sprite_table:04X}, "
            f"horizontal scroll {state.hscroll_table:04X}"
        )
        mode = ("whole screen", "invalid", "per 8 lines", "per line")[state.reg[11] & 3]
        columns = "per 2-cell column" if state.reg[11] & 4 else "whole screen"
        out.append(f"SCROLL MODE: horizontal {mode}, vertical {columns}")
        sprites = sprites_on_screen(state)
        showing = None
        if sources is not None:
            showing = Counter(
                pixel.detail for row in sources for pixel in row if pixel.layer == "sprite"
            )
        out.append(f"SPRITE LIST at the end of the frame: {len(sprites)} linked")
        for sprite in sprites:
            shown = "" if showing is None else f" pixels_shown={showing.get(sprite.index, 0)}"
            out.append(
                f"  #{sprite.index:2d} x={sprite.x:4d} y={sprite.y:4d} "
                f"{sprite.width}x{sprite.height} tile={sprite.tile:03X} pal={sprite.palette} "
                f"pri={int(sprite.priority)} flip={'H' if sprite.flip_h else '-'}"
                f"{'V' if sprite.flip_v else '-'}{shown}"
            )
    return "\n".join(out) + "\n"
