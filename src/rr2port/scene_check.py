"""Check the scene reader against what the game itself produced.

Two checks over a file of work-RAM images taken at every pass of a race
(`rr2port.handoffs`), both for exact equality:

* the road projection, repeated from the track bytes and the camera, against
  the game's own segment arrays in the same image;
* the per-line shift tables the game staged for the video chip, against the
  tables predicted from the segment arrays.

An image is taken when the frame interrupt closes a pass, wherever the main
loop happens to be. So a staged table can belong to the road of this image
or of one of the few before it, and now and then an image catches the
projection half-way through. The check allows for the first by trying the
last few images, and counts the second separately.
"""

from __future__ import annotations

import struct
from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

from rr2port.handoffs import iter_handoffs
from rr2port.picture_report import VideoState
from rr2port.scene import RoadTables, predict_line_tables, project_road, read_scene

STAGED_TABLES = (0x3A40, 0x40C2)  # the two staged copies of the per-line shift table
LOOK_BACK = 4  # images tried for each staged table: this one and three before
VIEW_LINES = 152


@dataclass
class CheckResult:
    handoffs: int = 0
    segments_identical: int = 0
    first_differing_handoff: int | None = None
    segments_interrupted: int = 0  # differing, but equal up to a point and older after it
    tables_checked: int = 0
    tables_explained: int = 0
    tables_half_written: int = 0  # lower lines from a newer road, upper lines from an older one
    handoffs_back: dict[int, int] = field(default_factory=dict)
    unexplained: list[int] = field(default_factory=list)  # handoffs with an unexplained table


def _staged(ram: bytes, base: int, lines: Sequence[int]) -> dict[int, tuple[int, int]]:
    return {line: struct.unpack_from(">HH", ram, base + 4 * line) for line in lines}


def check_handoffs(path: Path, tables: RoadTables) -> CheckResult:
    result = CheckResult()
    back = Counter()
    # For the last few images: the predicted shift table and the projected road.
    recent: list[tuple[bytes, dict[int, tuple[int, int]], tuple]] = []
    for k, ram in iter_handoffs(path):
        scene = read_scene(ram)
        projected = project_road(ram, tables)
        result.handoffs += 1
        if projected == scene.road:
            result.segments_identical += 1
        else:
            if result.first_differing_handoff is None:
                result.first_differing_handoff = k
            # Caught half-way through: the game's arrays follow this image's projection
            # up to some point and still hold the previous image's from there on.
            first = next(
                i for i, (a, b) in enumerate(zip(projected, scene.road, strict=True)) if a != b
            )
            if recent and first > 0 and recent[-1][2][first:] == scene.road[first:]:
                result.segments_interrupted += 1
        recent.append((ram, predict_line_tables(scene, tables), projected))
        del recent[:-LOOK_BACK]
        predictions = [predicted for _, predicted, _ in reversed(recent) if predicted]
        for base in STAGED_TABLES:
            result.tables_checked += 1
            ages = [
                age
                for age, predicted in enumerate(predictions)
                if _staged(ram, base, predicted) == predicted
            ]
            if ages:
                result.tables_explained += 1
                back[ages[0]] += 1
            elif _half_written(ram, base, predictions):
                result.tables_half_written += 1
            elif k not in result.unexplained:
                result.unexplained.append(k)
    result.handoffs_back = dict(sorted(back.items()))
    return result


def _half_written(ram: bytes, base: int, predictions: Sequence[dict]) -> bool:
    """The table is being rewritten from the bottom line up: its lower lines equal one
    prediction and all its other lines an older one."""
    for newer, new in enumerate(predictions):
        for old in predictions[newer + 1 :]:
            lines = sorted(set(new) & set(old), reverse=True)
            staged = _staged(ram, base, lines)
            done = 0
            while done < len(lines) and staged[lines[done]] == new[lines[done]]:
                done += 1
            if 0 < done < len(lines) and all(staged[line] == old[line] for line in lines[done:]):
                return True
    return False


def find_pass_shown(
    state: VideoState, images: Sequence[tuple[int, bytes]]
) -> list[tuple[int, int]]:
    """Which staged shift table is in video memory: (handoff, which of the two copies).

    Compares the 3D view's lines of the video chip's table with every staged
    copy in the given images. Empty if none is equal.
    """
    size = VIEW_LINES * 4
    shown = state.vram[state.hscroll_table : state.hscroll_table + size]
    return [
        (k, copy)
        for k, ram in images
        for copy, base in enumerate(STAGED_TABLES)
        if ram[base : base + size] == shown
    ]


def format_result(result: CheckResult) -> str:
    first = result.first_differing_handoff
    out = [
        f"SEGMENTS identical={result.segments_identical} of {result.handoffs} "
        f"caught_mid_projection={result.segments_interrupted} "
        f"first_differing={'none' if first is None else first}",
        f"SHIFT_TABLES explained={result.tables_explained} of {result.tables_checked} "
        f"half_written={result.tables_half_written}",
        "HANDOFFS_BACK " + " ".join(f"{age}:{n}" for age, n in result.handoffs_back.items()),
    ]
    if result.unexplained:
        shown = " ".join(str(k) for k in result.unexplained[:40])
        out.append(f"UNEXPLAINED handoffs={len(result.unexplained)}: {shown}")
    return "\n".join(out) + "\n"
