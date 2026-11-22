"""Load and compare pictures: MAME's PNG snapshots against the port's PPM frames."""

from __future__ import annotations

import re
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path


@dataclass
class PictureDiff:
    same: bool
    size_want: tuple[int, int]
    size_got: tuple[int, int]
    differing_pixels: int = 0
    first: tuple[int, int] | None = None  # (x, y) of the first differing pixel


def _load_ppm(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    # P6, then width, height and maximum value separated by whitespace or comments,
    # then exactly one whitespace byte before the pixels.
    header = re.match(
        rb"P6(?:\s+|#[^\n]*\n)+(\d+)(?:\s+|#[^\n]*\n)+(\d+)(?:\s+|#[^\n]*\n)+(\d+)\s", data
    )
    if header is None:
        raise ValueError(f"{path}: not a binary PPM (P6)")
    width, height, maximum = (int(group) for group in header.groups())
    if maximum != 255:
        raise ValueError(f"{path}: only 8-bit PPM is supported")
    pixels = data[header.end() :]
    if len(pixels) != width * height * 3:
        raise ValueError(
            f"{path}: expected {width * height * 3} pixel bytes for {width}x{height}, "
            f"found {len(pixels)}"
        )
    return width, height, pixels


def load_rgb(path: Path) -> tuple[int, int, bytes]:
    """Return (width, height, RGB bytes) for a PNG or binary PPM file."""
    if path.suffix.lower() == ".ppm":
        return _load_ppm(path)
    from PIL import Image  # development dependency; only needed for PNG

    with Image.open(path) as image:
        rgb = image.convert("RGB")
        return rgb.width, rgb.height, rgb.tobytes()


def compare_pictures(want: Path, got: Path) -> PictureDiff:
    """Compare two pictures pixel for pixel. A missing reference is an error."""
    if not want.exists():
        raise FileNotFoundError(f"no reference picture: {want}")
    want_w, want_h, want_pixels = load_rgb(want)
    got_w, got_h, got_pixels = load_rgb(got)
    diff = PictureDiff(same=False, size_want=(want_w, want_h), size_got=(got_w, got_h))
    if (want_w, want_h) != (got_w, got_h):
        return diff
    for index in range(want_w * want_h):
        offset = index * 3
        if want_pixels[offset : offset + 3] != got_pixels[offset : offset + 3]:
            diff.differing_pixels += 1
            if diff.first is None:
                diff.first = (index % want_w, index // want_w)
    diff.same = diff.differing_pixels == 0
    return diff


def main(argv: Sequence[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if len(args) != 2:
        print("usage: python -m rr2port.picture WANT GOT", file=sys.stderr)
        return 2
    try:
        diff = compare_pictures(Path(args[0]), Path(args[1]))
    except (FileNotFoundError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    want = f"{diff.size_want[0]}x{diff.size_want[1]}"
    got = f"{diff.size_got[0]}x{diff.size_got[1]}"
    if diff.size_want != diff.size_got:
        print(f"sizes differ: reference is {want}, got {got}")
        return 1
    if not diff.same:
        total = diff.size_want[0] * diff.size_want[1]
        print(f"{diff.differing_pixels} of {total} pixels differ; first at {diff.first}")
        return 1
    print(f"identical ({want})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
