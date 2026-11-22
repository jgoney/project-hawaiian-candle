"""Read and reconstruct RR2 per-frame RAM differential captures."""

from __future__ import annotations

import hashlib
import math
import struct
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path

MAGIC = b"RR2DIFF1"
RAM_SIZE = 0x10000
FRAME_HEADER = struct.Struct(">II")
CHANGE = struct.Struct(">HB")


@dataclass(frozen=True)
class FrameDiff:
    number: int
    changes: tuple[tuple[int, int], ...]


@dataclass(frozen=True)
class CaptureSummary:
    frames: int
    changed_bytes: int
    first_frame: int | None
    last_frame: int | None
    baseline_sha256: str


@dataclass(frozen=True)
class WordCorrelation:
    offset: int
    correlation: float
    changes: int


def read_baseline(path: Path) -> bytes:
    data = path.read_bytes()
    if len(data) != RAM_SIZE:
        raise ValueError(f"baseline must be {RAM_SIZE} bytes, found {len(data)}")
    return data


def iter_frame_diffs(path: Path) -> Iterator[FrameDiff]:
    with path.open("rb") as stream:
        if stream.read(len(MAGIC)) != MAGIC:
            raise ValueError("not an RR2DIFF1 capture")

        previous_frame = 0
        while header := stream.read(FRAME_HEADER.size):
            if len(header) != FRAME_HEADER.size:
                raise ValueError("truncated frame header")
            frame, count = FRAME_HEADER.unpack(header)
            if frame <= previous_frame:
                raise ValueError("frame numbers must increase")

            payload = stream.read(count * CHANGE.size)
            if len(payload) != count * CHANGE.size:
                raise ValueError(f"truncated change list for frame {frame}")
            changes = tuple(CHANGE.iter_unpack(payload))
            if len({offset for offset, _ in changes}) != len(changes):
                raise ValueError(f"duplicate RAM offset in frame {frame}")

            yield FrameDiff(frame, changes)
            previous_frame = frame


def apply_frame(ram: bytearray, frame: FrameDiff) -> None:
    if len(ram) != RAM_SIZE:
        raise ValueError(f"RAM image must be {RAM_SIZE} bytes")
    for offset, value in frame.changes:
        ram[offset] = value


def summarize_capture(baseline_path: Path, diffs_path: Path) -> CaptureSummary:
    baseline = read_baseline(baseline_path)
    frames = 0
    changed_bytes = 0
    first_frame = None
    last_frame = None
    for frame in iter_frame_diffs(diffs_path):
        frames += 1
        changed_bytes += len(frame.changes)
        first_frame = frame.number if first_frame is None else first_frame
        last_frame = frame.number
    return CaptureSummary(
        frames=frames,
        changed_bytes=changed_bytes,
        first_frame=first_frame,
        last_frame=last_frame,
        baseline_sha256=hashlib.sha256(baseline).hexdigest(),
    )


def correlate_words(
    baseline_path: Path,
    diffs_path: Path,
    target_offset: int,
    *,
    trim_to_nonzero_target: bool = True,
) -> tuple[tuple[int, int], list[WordCorrelation]]:
    """Correlate every aligned big-endian RAM word with a target word per frame."""
    if not 0 <= target_offset < RAM_SIZE - 1:
        raise ValueError("target word offset is outside work RAM")

    baseline = read_baseline(baseline_path)
    ram = bytearray(baseline)
    target_values: list[int] = []
    expected_frame = 1
    for frame in iter_frame_diffs(diffs_path):
        if frame.number != expected_frame:
            raise ValueError("correlation requires consecutive frame records starting at 1")
        apply_frame(ram, frame)
        target_values.append(int.from_bytes(ram[target_offset : target_offset + 2], "big"))
        expected_frame += 1
    if not target_values:
        raise ValueError("capture has no frame records")

    start = 1
    end = len(target_values)
    if trim_to_nonzero_target:
        nonzero = [index for index, value in enumerate(target_values, 1) if value]
        if not nonzero:
            raise ValueError("target word is zero for the entire capture")
        start, end = nonzero[0], nonzero[-1]

    prefix = [0]
    prefix_squared = [0]
    for value in target_values:
        prefix.append(prefix[-1] + value)
        prefix_squared.append(prefix_squared[-1] + value * value)

    word_count = RAM_SIZE // 2
    sums = [0] * word_count
    squared_sums = [0] * word_count
    cross_sums = [0] * word_count
    change_counts = [0] * word_count
    last_frames = [start] * word_count
    current_words: list[int] | None = None
    ram = bytearray(baseline)

    def finish_segment(word_index: int, segment_end: int) -> None:
        segment_start = last_frames[word_index]
        if segment_end < segment_start:
            return
        duration = segment_end - segment_start + 1
        value = current_words[word_index]  # type: ignore[index]
        target_sum = prefix[segment_end] - prefix[segment_start - 1]
        sums[word_index] += value * duration
        squared_sums[word_index] += value * value * duration
        cross_sums[word_index] += value * target_sum

    for frame in iter_frame_diffs(diffs_path):
        if frame.number < start:
            apply_frame(ram, frame)
            continue
        if frame.number > end:
            break
        if current_words is None:
            current_words = [
                int.from_bytes(ram[offset : offset + 2], "big")
                for offset in range(0, RAM_SIZE, 2)
            ]

        changed_words = {offset // 2 for offset, _ in frame.changes}
        for word_index in changed_words:
            finish_segment(word_index, frame.number - 1)
        apply_frame(ram, frame)
        for word_index in changed_words:
            offset = word_index * 2
            current_words[word_index] = int.from_bytes(ram[offset : offset + 2], "big")
            last_frames[word_index] = frame.number
            change_counts[word_index] += 1

    if current_words is None:
        raise ValueError("selected correlation interval has no frames")
    for word_index in range(word_count):
        finish_segment(word_index, end)

    frame_count = end - start + 1
    target_sum = prefix[end] - prefix[start - 1]
    target_squared_sum = prefix_squared[end] - prefix_squared[start - 1]
    target_variance = frame_count * target_squared_sum - target_sum * target_sum
    if target_variance == 0:
        raise ValueError("target word has no variance in the selected interval")

    results: list[WordCorrelation] = []
    for word_index in range(word_count):
        variance = frame_count * squared_sums[word_index] - sums[word_index] ** 2
        if variance <= 0:
            continue
        numerator = frame_count * cross_sums[word_index] - sums[word_index] * target_sum
        correlation = numerator / math.sqrt(variance * target_variance)
        results.append(WordCorrelation(word_index * 2, correlation, change_counts[word_index]))
    results.sort(key=lambda result: abs(result.correlation), reverse=True)
    return (start, end), results
