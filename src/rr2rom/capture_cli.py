"""Summarize a MAME RR2 RAM differential capture."""

from __future__ import annotations

import argparse
from pathlib import Path

from .capture import correlate_words, summarize_capture


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate and summarize an RR2 RAM capture")
    parser.add_argument("prefix", type=Path, help="capture path without .baseline.bin/.diffs.bin")
    parser.add_argument(
        "--correlate-word",
        type=lambda value: int(value, 0),
        metavar="OFFSET",
        help="rank aligned 16-bit RAM words against this work-RAM offset",
    )
    parser.add_argument("--top", type=int, default=20, help="correlation rows to display")
    parser.add_argument(
        "--min-changes",
        type=int,
        default=0,
        help="exclude words changed in fewer frame records",
    )
    parser.add_argument(
        "--full-range",
        action="store_true",
        help="include leading/trailing frames where the target is zero",
    )
    args = parser.parse_args()

    summary = summarize_capture(
        args.prefix.with_suffix(".baseline.bin"),
        args.prefix.with_suffix(".diffs.bin"),
    )
    print(f"frames: {summary.frames}")
    print(f"changed byte records: {summary.changed_bytes}")
    print(f"frame range: {summary.first_frame}..{summary.last_frame}")
    print(f"baseline SHA-256: {summary.baseline_sha256}")
    if args.correlate_word is not None:
        interval, correlations = correlate_words(
            args.prefix.with_suffix(".baseline.bin"),
            args.prefix.with_suffix(".diffs.bin"),
            args.correlate_word,
            trim_to_nonzero_target=not args.full_range,
        )
        print(f"correlation frame range: {interval[0]}..{interval[1]}")
        print("rank  address   offset  correlation  changed_frames")
        filtered = [result for result in correlations if result.changes >= args.min_changes]
        for rank, result in enumerate(filtered[: args.top], 1):
            print(
                f"{rank:>4}  {0xFF0000 + result.offset:06X}  {result.offset:04X}  "
                f"{result.correlation:+.6f}  {result.changes:>14}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
