from __future__ import annotations

import struct
from pathlib import Path

import pytest

from rr2rom.capture import (
    MAGIC,
    RAM_SIZE,
    apply_frame,
    correlate_words,
    iter_frame_diffs,
    summarize_capture,
)


def write_capture(path: Path) -> None:
    path.write_bytes(
        MAGIC
        + struct.pack(">IIHBHB", 1, 2, 0x0012, 0x34, 0xFFFF, 0x56)
        + struct.pack(">II", 2, 0)
        + struct.pack(">IIHB", 4, 1, 0x0012, 0x78)
    )


def test_capture_can_be_reconstructed_and_summarized(tmp_path: Path) -> None:
    baseline_path = tmp_path / "run.baseline.bin"
    diffs_path = tmp_path / "run.diffs.bin"
    baseline_path.write_bytes(bytes(RAM_SIZE))
    write_capture(diffs_path)

    ram = bytearray(baseline_path.read_bytes())
    frames = list(iter_frame_diffs(diffs_path))
    for frame in frames:
        apply_frame(ram, frame)

    assert ram[0x12] == 0x78
    assert ram[0xFFFF] == 0x56
    summary = summarize_capture(baseline_path, diffs_path)
    assert summary.frames == 3
    assert summary.changed_bytes == 3
    assert summary.first_frame == 1
    assert summary.last_frame == 4


def test_truncated_capture_is_rejected(tmp_path: Path) -> None:
    path = tmp_path / "bad.diffs.bin"
    path.write_bytes(MAGIC + struct.pack(">II", 1, 1) + b"\x00")

    with pytest.raises(ValueError, match="truncated change list"):
        list(iter_frame_diffs(path))


def test_word_correlation_finds_matching_and_inverse_values(tmp_path: Path) -> None:
    baseline_path = tmp_path / "correlation.baseline.bin"
    diffs_path = tmp_path / "correlation.diffs.bin"
    baseline_path.write_bytes(bytes(RAM_SIZE))
    records = [MAGIC]
    for frame, value in enumerate((1, 2, 3, 4), 1):
        changes = ((0x10, 0), (0x11, value), (0x20, 0), (0x21, 5 - value))
        records.append(struct.pack(">II", frame, len(changes)))
        records.extend(struct.pack(">HB", offset, byte) for offset, byte in changes)
    diffs_path.write_bytes(b"".join(records))

    interval, results = correlate_words(baseline_path, diffs_path, 0x10)

    correlations = {result.offset: result.correlation for result in results}
    assert interval == (1, 4)
    assert correlations[0x10] == pytest.approx(1.0)
    assert correlations[0x20] == pytest.approx(-1.0)
