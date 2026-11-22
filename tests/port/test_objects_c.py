"""The C repeat of the game's object drawing against the Python reference, on random scenes."""

import random
import struct
import subprocess

import pytest
from harness import HW, RENDER, REPO, compiler
from test_objects import (
    PICTURE_BUFFER,
    SPRITE_LIST,
    anchored_picture,
    plain_picture,
    race_ram,
    rom_with,
)

from rr2port.objects import COPY_SPACING, draw_objects_detail

TOOL_SOURCES = [REPO / "port" / "tools" / "rr_objects_draw_tool.c", RENDER / "rr_objects.c"]


@pytest.fixture(scope="module")
def tool(tmp_path_factory):
    binary = tmp_path_factory.mktemp("objects") / "rr-objects-draw"
    subprocess.run(
        [compiler(), "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
         f"-I{HW}", f"-I{RENDER}", f"-I{REPO / 'port' / 'core'}",
         "-o", str(binary), *map(str, TOOL_SOURCES)],
        check=True,
    )  # fmt: skip
    return binary


def random_scene(seed: int) -> tuple[bytes, bytes]:
    rng = random.Random(seed)
    pictures = {}
    anchored = {}
    for index in range(1, 9):
        width, height = rng.randint(2, 70), rng.randint(2, 90)
        if rng.random() < 0.5:
            pictures[index] = anchored_picture(
                rng.randint(0, width), rng.randint(0, min(height + 20, 127)), width, height
            )
            anchored[index] = True
        else:
            pictures[index] = plain_picture(width, height)
            anchored[index] = False
    rom = bytearray(rom_with(pictures))
    # Irregular spacing for copies: eight lists of shifts.
    for n in range(8):
        at = 0x17000 + 16 * n
        struct.pack_into(">I", rom, COPY_SPACING + 4 * n, at)
        for k in range(8):
            struct.pack_into(">h", rom, at + 2 * k, rng.choice([1, 2, 3, -1, -2, 4]))
    with_anchor = [i for i in pictures if anchored[i]]
    records = []
    for _ in range(rng.randint(1, 12)):
        index = rng.choice(list(pictures))
        flags = rng.choice([0x0000, 0x1000, 0x2000, 0x3000])
        if rng.random() < 0.3:
            flags |= 0x8000
        if anchored[index]:
            flags |= 0x0080
        second = 0
        if anchored[index] and with_anchor and rng.random() < 0.4:
            second = rng.choice(with_anchor)
        if rng.random() < 0.3:
            flags |= rng.randint(1, 7) << 8
            if rng.random() < 0.4:
                flags |= 0x0020
        records.append(
            {
                "x": rng.randint(-40, 360),
                "foot": rng.randint(0, 170),
                "size": rng.choice([0x4000, 0x2000, 0x8000, rng.randint(0x0400, 0xFFFF)]),
                "hidden": rng.randint(40, 0x98),
                "picture": index,
                "flags": flags,
                "offset": rng.randint(-60, 60),
                "second": second,
            }
        )
    ram = race_ram(records)
    for i in range(len(records)):
        struct.pack_into(">H", ram, 0xCEB8 + 22 * i + 0x14, rng.randint(0, 255))
    if rng.random() < 0.3:  # sprites and tiles already in the list (the game's other sprites)
        struct.pack_into(">H", ram, 0xDC34, rng.randint(1, 78))
        struct.pack_into(">H", ram, 0xDC30, rng.randint(0, 300))
    return bytes(rom), bytes(ram)


def run_tool(tool, tmp_path, rom: bytes, ram: bytes) -> tuple[bytes, list[list[str]]]:
    (tmp_path / "rom.bin").write_bytes(rom)
    (tmp_path / "ram.bin").write_bytes(ram)
    done = subprocess.run(
        [str(tool), *(str(tmp_path / name) for name in ("rom.bin", "ram.bin", "out.bin"))],
        capture_output=True, text=True, check=True,
    )  # fmt: skip
    return (tmp_path / "out.bin").read_bytes(), [line.split() for line in done.stdout.splitlines()]


def test_c_drawing_equals_python_on_random_scenes(tool, tmp_path):
    drew = entries = described = 0
    for seed in range(150):
        rom, ram = random_scene(seed)
        expected = draw_objects_detail(ram, rom)
        out, lines = run_tool(tool, tmp_path, rom, ram)
        for name, at, length in (
            ("sprite list", SPRITE_LIST, 80 * 8),
            ("picture buffer", PICTURE_BUFFER, 0x150 * 32),
            ("counters", 0xDC30, 6),
        ):
            assert out[at : at + length] == expected.ram[at : at + length], f"seed {seed}: {name}"
        assert out == expected.ram, f"seed {seed}: something else in RAM"
        fields = {line[0]: line[1:] for line in lines if line[0] in ("ok", "sprites")}
        assert fields["ok"] == ["1"]
        assert int(fields["sprites"][1]) == min(len(expected.sprites), 80), f"seed {seed}"
        drew += len(expected.sprites) > 0
        # An object entry shows a picture that lies in the ROM (or none, if
        # the picture is cut to nothing).
        pictures = [line for line in lines if line[0] == "picture"]
        for entry in (line for line in lines if line[0] == "entry"):
            assert -1 <= int(entry[2]) < len(pictures), f"seed {seed}: entry {entry[1]}"
            entries += 1
            described += int(entry[2]) >= 0
        for picture in pictures:
            offset, width, height, stride = (int(field) for field in picture[2:6])
            assert offset > 0 and offset + stride * height <= len(rom), f"seed {seed}"
            assert stride == (width + 1) // 2 + 1
    assert drew > 100  # the scenes do draw something
    assert described > 0.97 * entries


def test_tool_reports_a_missing_file(tool, tmp_path):
    done = subprocess.run(
        [str(tool), str(tmp_path / "none"), str(tmp_path / "none"), str(tmp_path / "out")],
        capture_output=True, text=True,
    )  # fmt: skip
    assert done.returncode == 2 and "cannot read" in done.stderr
