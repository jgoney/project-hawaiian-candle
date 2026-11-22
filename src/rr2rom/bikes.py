"""RR206 bike tuning-table parser."""

from __future__ import annotations

import struct
from dataclasses import dataclass

BIKE_NAMES = (
    "SHURIKEN 400",
    "PANDA 500",
    "SHURIKEN TT 250",
    "PANDA 900",
    "BANZAI 7.11",
    "BANZAI 600 N",
    "BANZAI 750 N",
    "SHURIKEN 1000 N",
    "BANZAI 7.11 N",
    "DIABLO 1000 N",
    "PANDA 600",
    "BANZAI 600",
    "BANZAI 750",
    "SHURIKEN 1000",
    "DIABLO 1000",
    "WILD THING 2000",
)

TUNING_POINTER_TABLE = 0x278BA
SECONDARY_TABLE = 0x277AE
TUNING_WORDS = 19
SECONDARY_WORDS = 8


@dataclass(frozen=True)
class BikeTuning:
    index: int
    name: str
    tuning_address: int
    max_gear_index: int
    base_delta_by_gear: tuple[int, ...]
    upper_speed_thresholds: tuple[int, ...]
    lower_speed_thresholds: tuple[int, ...]
    secondary_parameters: tuple[int, ...]


def parse_bike_tuning(data: bytes) -> tuple[BikeTuning, ...]:
    """Parse RR206's 16 bike entries and their big-endian tuning records."""
    pointer_end = TUNING_POINTER_TABLE + 4 * len(BIKE_NAMES)
    secondary_end = SECONDARY_TABLE + 2 * SECONDARY_WORDS * len(BIKE_NAMES)
    if len(data) < max(pointer_end, secondary_end):
        raise ValueError("input is too short for RR206 bike tables")

    pointers = struct.unpack_from(f">{len(BIKE_NAMES)}I", data, TUNING_POINTER_TABLE)
    bikes: list[BikeTuning] = []
    for index, (name, address) in enumerate(zip(BIKE_NAMES, pointers, strict=True)):
        if address + 2 * TUNING_WORDS > len(data):
            raise ValueError(f"bike {index} tuning pointer 0x{address:X} is outside the ROM")
        words = struct.unpack_from(f">{TUNING_WORDS}H", data, address)
        secondary = struct.unpack_from(
            f">{SECONDARY_WORDS}H", data, SECONDARY_TABLE + index * SECONDARY_WORDS * 2
        )
        bikes.append(
            BikeTuning(
                index=index,
                name=name,
                tuning_address=address,
                max_gear_index=words[0],
                base_delta_by_gear=words[1:7],
                upper_speed_thresholds=words[7:13],
                lower_speed_thresholds=words[13:19],
                secondary_parameters=secondary,
            )
        )
    return tuple(bikes)

