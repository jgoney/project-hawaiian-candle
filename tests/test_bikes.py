from __future__ import annotations

import struct

import pytest

from rr2rom.bikes import (
    BIKE_NAMES,
    SECONDARY_TABLE,
    TUNING_POINTER_TABLE,
    parse_bike_tuning,
)


def make_table_fixture() -> bytes:
    data = bytearray(0x28000)
    for index in range(len(BIKE_NAMES)):
        address = 0x27000 + index * 0x26
        struct.pack_into(">I", data, TUNING_POINTER_TABLE + index * 4, address)
        words = (5, *range(index, index + 6), *range(100, 106), *range(200, 206))
        struct.pack_into(">19H", data, address, *words)
        struct.pack_into(">8H", data, SECONDARY_TABLE + index * 16, *([index] * 8))
    return bytes(data)


def test_bike_table_layout() -> None:
    bikes = parse_bike_tuning(make_table_fixture())

    assert len(bikes) == 16
    assert bikes[0].name == "SHURIKEN 400"
    assert bikes[15].name == "WILD THING 2000"
    assert bikes[3].max_gear_index == 5
    assert bikes[3].base_delta_by_gear == (3, 4, 5, 6, 7, 8)
    assert bikes[3].upper_speed_thresholds == (100, 101, 102, 103, 104, 105)
    assert bikes[3].lower_speed_thresholds == (200, 201, 202, 203, 204, 205)
    assert bikes[3].secondary_parameters == (3,) * 8


def test_out_of_range_tuning_pointer_is_rejected() -> None:
    data = bytearray(make_table_fixture())
    struct.pack_into(">I", data, TUNING_POINTER_TABLE, len(data))

    with pytest.raises(ValueError, match="outside the ROM"):
        parse_bike_tuning(bytes(data))
