"""Display RR206 bike tuning records."""

from __future__ import annotations

import argparse
import json
from dataclasses import asdict
from pathlib import Path

from .bikes import parse_bike_tuning
from .rom import inspect_rom_bytes
from .text_cli import RR206_SHA256


def _hex_words(values: tuple[int, ...]) -> str:
    return " ".join(f"{value:04X}" for value in values)


def main() -> int:
    parser = argparse.ArgumentParser(description="Inspect RR206 per-bike tuning tables")
    parser.add_argument("rom", type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    data = args.rom.read_bytes()
    report = inspect_rom_bytes(data)
    if report.hashes.sha256 != RR206_SHA256:
        raise SystemExit("error: image is not the known-good RR206 dump")
    bikes = parse_bike_tuning(data)

    if args.json:
        print(json.dumps([asdict(bike) for bike in bikes], indent=2))
        return 0

    print(
        "idx  bike                record    gear deltas                       "
        "upper thresholds                  lower thresholds"
    )
    for bike in bikes:
        print(
            f"{bike.index:>2}   {bike.name:<19} {bike.tuning_address:06X}  "
            f"{_hex_words(bike.base_delta_by_gear):<29} "
            f"{_hex_words(bike.upper_speed_thresholds):<35} "
            f"{_hex_words(bike.lower_speed_thresholds)}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
