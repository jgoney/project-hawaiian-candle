"""Curated RR206 text-resource dump."""

from __future__ import annotations

import argparse
from pathlib import Path

from .rom import inspect_rom_bytes
from .text import fixed_ascii_records, printable_runs, unique_text

RR206_SHA256 = "c7a609019b1f052bcee6204a3a09ea77a5150a748de90b51382fedf4df207848"


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="Dump curated text resources from RR206")
    result.add_argument("rom", type=Path)
    result.add_argument(
        "--allow-unknown",
        action="store_true",
        help="extract from an unrecognized image using RR206 offsets",
    )
    return result


def _clean_display_text(text: str) -> str:
    printable_prefix = "".join(character if " " <= character <= "~" else "\0" for character in text)
    return " ".join(printable_prefix.split("\0", 1)[0].split())


def main() -> int:
    args = parser().parse_args()
    data = args.rom.read_bytes()
    report = inspect_rom_bytes(data)
    if report.hashes.sha256 != RR206_SHA256 and not args.allow_unknown:
        raise SystemExit("error: image is not the known-good RR206 dump; use --allow-unknown")

    bikes = fixed_ascii_records(
        data, first_text=0x672C, stride=0x18, text_length=0x10, count=15
    )
    bike_copy = fixed_ascii_records(
        data, first_text=0x68AC, stride=0x6C, text_length=0x6C, count=16
    )
    bike_stats = fixed_ascii_records(
        data, first_text=0x6FAC, stride=0x26, text_length=0x26, count=16
    )
    rider_records = fixed_ascii_records(
        data, first_text=0x10B38, stride=0x10, text_length=10, count=75
    )
    police_records = fixed_ascii_records(
        data, first_text=0x10FF8, stride=0x50, text_length=10, count=5
    )
    dialogue = printable_runs(data, start=0x204C8, end=0x25B00, minimum_length=4)

    print("# RR206 text-resource dump")
    print()
    print(f"Source SHA-256: `{report.hashes.sha256}`")
    print()
    print("## Bikes")
    print()
    for record in bikes:
        print(f"- `0x{record.offset:06X}` — {record.text}")
    print("- `0x006F00` — WILD THING 2000 (secret/extra description entry)")
    print()
    print("## Bike-shop descriptions and stats")
    print()
    for copy, stats in zip(bike_copy, bike_stats, strict=True):
        print(f"- `0x{copy.offset:06X}` — {_clean_display_text(copy.text)}")
        print(f"  - {_clean_display_text(stats.text)}")
    print()
    print("## Unique named riders")
    print()
    for name in unique_text(rider_records):
        print(f"- {name[:1].upper()}{name[1:]}")
    print()
    print("## Named police characters")
    print()
    for record in police_records:
        print(f"- `0x{record.offset:06X}` — {record.text}")
    print()
    print("## Other compact resource lists")
    print()
    print("- Tracks: Gold Rush, Molokai Express, Smokin' Mountain, Hoodoo's Revenge, Maple Run")
    print("- Regions: Alaska, Hawaii, Tennessee, Arizona, Vermont")
    print("- Weapons: None, Billy Club, Chain")
    print()
    print(f"## Dialogue and longer messages ({len(dialogue)} printable runs)")
    print()
    for run in dialogue:
        print(f"- `0x{run.offset:06X}` — {_clean_display_text(run.text)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
