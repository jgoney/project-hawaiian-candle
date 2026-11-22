"""Command-line interface for ROM inspection."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from .rom import RomFormatError, inspect_rom


def _hex(value: int, width: int = 8) -> str:
    return f"0x{value:0{width}X}"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Inspect a linear Sega Genesis ROM")
    parser.add_argument("rom", type=Path)
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    parser.add_argument("--all-vectors", action="store_true", help="show all 64 vectors")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        report = inspect_rom(args.rom)
    except (OSError, RomFormatError) as error:
        raise SystemExit(f"error: {error}") from error

    if args.json:
        print(json.dumps(report.to_dict(), indent=2))
        return 0

    header = report.header
    print(f"File: {args.rom}")
    print(f"Size: {report.size} bytes ({_hex(report.size)})")
    print(f"CRC-32: {report.hashes.crc32}")
    print(f"SHA-1: {report.hashes.sha1}")
    print(f"SHA-256: {report.hashes.sha256}")
    print(f"System: {header.system_type}")
    print(f"Copyright: {header.copyright}")
    print(f"Domestic title: {header.domestic_title}")
    print(f"International title: {header.international_title}")
    print(f"Serial/revision: {header.serial_and_revision}")
    print(f"Regions: {header.regions}")
    print(f"ROM range: {_hex(header.rom_start)}-{_hex(header.rom_end)}")
    print(f"RAM range: {_hex(header.ram_start)}-{_hex(header.ram_end)}")
    print(
        f"Header checksum: {_hex(header.checksum, 4)}; "
        f"calculated: {_hex(report.calculated_checksum, 4)}; "
        f"match: {report.validation.checksum_matches}"
    )
    selected = report.vectors if args.all_vectors else (
        report.vectors[0],
        report.vectors[1],
        report.vectors[28],
        report.vectors[30],
    )
    print("Vectors:")
    for vector in selected:
        print(
            f"  {vector.index:2d} @ {_hex(vector.offset, 4)} "
            f"{vector.name}: {_hex(vector.address)}"
        )
    if report.validation.warnings:
        print("Validation warnings:")
        for warning in report.validation.warnings:
            print(f"  - {warning}")
    else:
        print("Validation warnings: none")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
