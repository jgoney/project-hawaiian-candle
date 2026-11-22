"""Read-only Sega Genesis ROM inspection helpers."""

from .rom import GenesisHeader, RomInspection, Vector, inspect_rom, inspect_rom_bytes

__all__ = [
    "GenesisHeader",
    "RomInspection",
    "Vector",
    "inspect_rom",
    "inspect_rom_bytes",
]

