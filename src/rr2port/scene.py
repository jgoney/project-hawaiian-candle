"""Read the race scene from a work-RAM image, and predict what the game makes of it.

The scene is what the game has decided to show before any of it reaches the
video chip (analysis/race-picture.md, section 3):

* the road: 32 projected segment points ahead of the camera, each a screen
  line, a centre and two edges, computed from the track's curve and hill
  bytes and the camera's place on the track;
* the objects: one draw record each (screen position of the foot, size,
  picture, mirror, colour set), in drawing order.

`project_road`, `road_edges` and `predict_line_tables` repeat the game's own
arithmetic, so that the reading can be checked against what the game
produced (rr2-scene-check). They follow the 68000 code instruction by
instruction, including its rounding, because the check is for equality.
"""

from __future__ import annotations

import struct
from collections.abc import Sequence
from dataclasses import dataclass

RACE_CALLBACK = 0xE038  # the frame callback while a race is running
ROAD_HALF_WIDTH = 0xE0  # world units
PERSPECTIVE_TABLE = 0x27C84  # ROM: scale for each distance (256ths of a segment)
RECIPROCAL_TABLE = 0x15472  # ROM: 65536 / n
PICTURE_TABLE = 0x56A0A  # ROM: pointers to object pictures
DRAW_RECORDS = 0xCEB8
DRAW_RECORD_SIZE = 22
DRAW_ORDER = 0x08D6
TRACK_RING = 0xD828


class NotARaceScene(Exception):
    """The RAM image is not a one-player race, so the scene cannot be read from it."""


@dataclass(frozen=True)
class RoadTables:
    """The two ROM tables the road arithmetic uses."""

    perspective: Sequence[int]  # scale (16-bit fraction) by distance
    reciprocal: Sequence[int]  # 65536 / n for n up to 255


@dataclass(frozen=True)
class RoadPoint:
    """One projected track segment boundary."""

    line: int  # screen line
    clipped_line: int  # the same, never lower on screen than a nearer point (hills hide)
    centre: int  # screen x of the road's centre
    left: int  # left and right edge, relative to the screen centre
    right: int


@dataclass(frozen=True)
class SceneObject:
    """One draw record: a vehicle, roadside object or sign."""

    order: int  # position in the drawing order (0 is drawn first)
    x: int  # screen x of its anchor point
    foot_line: int  # screen line of its foot
    size: float  # 1.0 is the picture's natural size
    hidden_below: int  # screen line below which it is hidden
    picture: int  # index into the ROM's picture table
    second_picture: int  # a second picture drawn with it, or 0
    mirrored: bool
    colour_set: int
    segment: int  # which projected road point it stands at
    offset: int  # sideways offset from the road edge, in screen pixels
    distance: int  # 256ths of a segment from the camera


@dataclass(frozen=True)
class Scene:
    camera_segment: int
    camera_fraction: int  # how far through the segment, 0..65535
    camera_sideways: int
    camera_height: int
    view_top: int  # first and one-past-last screen line of the 3D view
    view_bottom: int
    horizon_reference: int
    centre_x: int
    horizon_line: int  # first line the road is drawn on
    track: tuple[tuple[int, int], ...]  # (curve, hill) of each segment ahead
    road: tuple[RoadPoint, ...]
    objects: tuple[SceneObject, ...]


def load_tables(rom: bytes) -> RoadTables:
    return RoadTables(
        perspective=struct.unpack_from(">16384H", rom, PERSPECTIVE_TABLE),
        reciprocal=struct.unpack_from(">256H", rom, RECIPROCAL_TABLE),
    )


def _u16(ram: bytes, address: int) -> int:
    return struct.unpack_from(">H", ram, address & 0xFFFF)[0]


def _s16(ram: bytes, address: int) -> int:
    return struct.unpack_from(">h", ram, address & 0xFFFF)[0]


def _word(value: int) -> int:
    """The value as a signed 16-bit word, as a 68000 word register holds it."""
    value &= 0xFFFF
    return value - 0x10000 if value & 0x8000 else value


def _long(value: int) -> int:
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value & 0x80000000 else value


def _scaled(value: int, scale: int) -> int:
    """value x scale / 65536, rounding toward zero, as the game does with mulu and swap."""
    if scale == 0:
        return value
    if value < 0:
        return -((-value * scale) >> 16)
    return (value * scale) >> 16


def _require_race(ram: bytes) -> None:
    if len(ram) != 0x10000:
        raise NotARaceScene(f"a work-RAM image is 65536 bytes, not {len(ram)}")
    callback = struct.unpack_from(">I", ram, 0x0300)[0]
    if callback != RACE_CALLBACK:
        raise NotARaceScene(
            f"the frame callback is {callback:06X}, not the race's {RACE_CALLBACK:06X}"
        )
    if _u16(ram, 0x0304) == 1:
        raise NotARaceScene("a two-player race: the split screen is not read")


def project_road(ram: bytes, tables: RoadTables) -> tuple[RoadPoint, ...]:
    """The game's road projection (0x14B44), from the track ring and the camera."""
    count = _u16(ram, 0x1CA0)
    fraction = _u16(ram, 0x0A0C)
    index = (_u16(ram, 0x0A0A) & 0xFF) * 2

    def next_byte() -> int:
        nonlocal index
        value = struct.unpack_from(">b", ram, TRACK_RING + index)[0]
        index = (index + 1) % 0x200
        return value

    curve = next_byte()
    # The part of the first segment's curve already behind the camera, rounded.
    product = abs(curve) * fraction
    done = (product >> 16) + (1 if product & 0x8000 else 0)
    done = -done if curve < 0 else done
    step = _word(curve - done)
    sideways = _word(-_s16(ram, 0x0A02))
    hill = next_byte()
    rise = (abs(hill) * fraction) >> 16
    rise = _word(-(-rise if hill < 0 else rise))
    distance = _word(-(fraction >> 8))
    top, bottom = _s16(ram, 0xF2B0), _u16(ram, 0xF2B2)
    reference, centre_x = _s16(ram, 0xF2BC), _s16(ram, 0xF2BE)
    height = _s16(ram, 0x0A08)
    lowest = bottom
    points = []
    for _ in range(count):
        scale = tables.perspective[distance] if distance > 0 else 0
        under = _word(height - rise)
        line = _word(_scaled(under, scale) + reference)
        if under < 0 and line < top:
            line = top
        if (line & 0xFFFF) < (lowest & 0xFFFF) and distance >= 0x100:
            lowest = line
        x = _scaled(sideways, scale)
        half = _scaled(ROAD_HALF_WIDTH, scale)
        centre = _word(x + centre_x)
        points.append(RoadPoint(line, lowest, centre, _word(x - half), _word(x + half)))
        distance = _word(distance + 0x100)
        curve = next_byte()
        sideways = _word(sideways + step)
        step = _word(step + curve)
        rise = _word(rise + next_byte())
    return tuple(points)


def _interpolate(
    xs: Sequence[int], points: Sequence[RoadPoint], tables: RoadTables
) -> dict[int, int]:
    """One value per screen line between neighbouring points (0x153EA)."""
    out: dict[int, int] = {}
    for i in range(len(points) - 1):
        near, far = points[i], points[i + 1]
        lines = _word(near.clipped_line - far.line)
        if lines <= 0:
            continue
        across = _word(xs[i] - xs[i + 1])
        down = _word(near.line - far.line)
        if down <= 1:
            step = _long((across & 0xFFFF) << 16)
        elif down == 2:
            step = _long((across & 0xFFFF) << 16) >> 1
        else:
            shift = 0
            while down >= 0x100:
                down >>= 1
                shift += 1
            step = across * _word(tables.reciprocal[down] >> shift)
        value = _long((xs[i + 1] & 0xFFFF) << 16)
        for line in range(far.line, far.line + lines):
            out[line] = _word(value >> 16)
            value = _long(value + step)
    return out


def road_edges(points: Sequence[RoadPoint], tables: RoadTables) -> dict[int, tuple[int, int]]:
    """(left, right) road edge for each screen line that has both, relative to the centre."""
    left = _interpolate([p.left for p in points], points, tables)
    right = _interpolate([p.right for p in points], points, tables)
    return {line: (left[line], right[line]) for line in left if line in right}


def line_shifts(left: int, right: int, centre_x: int) -> tuple[int, int]:
    """The sideways shifts of plane A and plane B that put the road edges on one line."""
    a = min(_word(left + centre_x), 0x140)
    a = max(a, -0x160)
    b = min(_word(right + centre_x), 0x180)
    b = max(b, 0)
    return (a - 0x2A0) & 0xFFFF, (b + 0x2A0) & 0xFFFF


def predict_line_tables(scene: Scene, tables: RoadTables) -> dict[int, tuple[int, int]]:
    """What the game stages for the video chip's per-line shift table (0x14822).

    Lines from the horizon to the bottom of the 3D view; lines above the
    horizon are filled in by the scenery code, which is not predicted. A line
    the road arrays do not reach is left out.
    """
    edges = road_edges(scene.road, tables)
    return {
        line: line_shifts(edges[line][0], edges[line][1], scene.centre_x)
        for line in range(scene.horizon_line, scene.view_bottom)
        if line in edges
    }


def _objects(ram: bytes) -> tuple[SceneObject, ...]:
    objects = []
    for order in range(128):
        index = _u16(ram, DRAW_ORDER + 2 * order)
        if index == 0xFFFF:
            break
        at = DRAW_RECORDS + DRAW_RECORD_SIZE * index
        flags = _u16(ram, at + 0x0A)
        objects.append(
            SceneObject(
                order=order,
                x=_s16(ram, at),
                foot_line=_s16(ram, at + 2),
                size=_u16(ram, at + 4) / 0x4000,
                hidden_below=_s16(ram, at + 6),
                picture=_u16(ram, at + 8),
                second_picture=_u16(ram, at + 0x12),
                mirrored=bool(flags & 0x8000),
                colour_set=(flags >> 12) & 3,
                segment=flags & 0x1F,
                offset=_s16(ram, at + 0x0C),
                distance=_s16(ram, at + 0x0E),
            )
        )
    return tuple(objects)


def read_scene(ram: bytes) -> Scene:
    _require_race(ram)
    count = _u16(ram, 0x1CA0)
    road = tuple(
        RoadPoint(
            line=_s16(ram, 0x1E20 + 2 * i),
            clipped_line=_s16(ram, 0xD2B8 + 2 * i),
            centre=_s16(ram, 0x1DBC + 2 * i),
            left=_s16(ram, 0x1E84 + 2 * i),
            right=_s16(ram, 0x1F4C + 2 * i),
        )
        for i in range(count)
    )
    segment = _u16(ram, 0x0A0A)
    track = tuple(
        struct.unpack_from(">bb", ram, TRACK_RING + ((segment + i) & 0xFF) * 2)
        for i in range(count)
    )
    bottom = _u16(ram, 0xF2B2)
    return Scene(
        camera_segment=segment,
        camera_fraction=_u16(ram, 0x0A0C),
        camera_sideways=_s16(ram, 0x0A02),
        camera_height=_s16(ram, 0x0A08),
        view_top=_s16(ram, 0xF2B0),
        view_bottom=bottom,
        horizon_reference=_s16(ram, 0xF2BC),
        centre_x=_s16(ram, 0xF2BE),
        horizon_line=min(road[-1].clipped_line, bottom - 2) if road else bottom,
        track=track,
        road=road,
        objects=_objects(ram),
    )
