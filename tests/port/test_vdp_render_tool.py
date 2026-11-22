"""rr-vdp-render: render a dumped video chip state with the port's renderer."""

from __future__ import annotations

import subprocess

import pytest
from harness import CORE, HW, REPO, compiler

from rr2port.picture import load_rgb

TOOL = REPO / "port" / "tools" / "rr_vdp_render_tool.c"
MAGIC = b"RR2VDPSTATE1\n"


@pytest.fixture(scope="module")
def tool(tmp_path_factory):
    binary = tmp_path_factory.mktemp("vdp-render") / "rr-vdp-render"
    subprocess.run(
        [compiler(), "-std=c11", "-Wall", "-Wextra", "-Werror", "-O0", f"-I{CORE}", f"-I{HW}",
         "-o", str(binary), str(TOOL), str(HW / "rr_vdp.c"), str(HW / "rr_vdp_render.c"),
         str(CORE / "rr_mem.c"), str(CORE / "rr_run.c"), "-DRR_NO_GENERATED_CODE"],
        check=True,
    )  # fmt: skip
    return binary


def state(regs: dict[int, int], cram: dict[int, int]) -> bytes:
    reg_bytes = bytearray(24)
    for index, value in regs.items():
        reg_bytes[index] = value
    cram_words = [0] * 64
    for index, value in cram.items():
        cram_words[index] = value
    cram_bytes = b"".join(value.to_bytes(2, "big") for value in cram_words)
    return MAGIC + bytes(reg_bytes) + bytes(0x10000) + cram_bytes + bytes(80)


def test_renders_the_backdrop_of_a_state_file(tool, tmp_path):
    path = tmp_path / "state.bin"
    path.write_bytes(state({1: 0x44, 7: 0x05, 12: 0x81}, {5: 0x000E}))
    out = tmp_path / "out.ppm"
    done = subprocess.run(
        [str(tool), "--state", str(path), "-o", str(out)], capture_output=True, text=True
    )
    assert done.returncode == 0, done.stderr
    width, height, pixels = load_rgb(out)
    assert (width, height) == (320, 224) and pixels == bytes([255, 0, 0]) * (320 * 224)


def test_truncated_state_names_the_missing_part(tool, tmp_path):
    path = tmp_path / "state.bin"
    path.write_bytes(state({}, {})[: len(MAGIC) + 24 + 100])
    done = subprocess.run(
        [str(tool), "--state", str(path), "-o", str(tmp_path / "o.ppm")],
        capture_output=True,
        text=True,
    )
    assert done.returncode == 2 and "VRAM" in done.stderr


def test_wrong_magic_is_refused(tool, tmp_path):
    path = tmp_path / "state.bin"
    path.write_bytes(b"not a state file" + bytes(70000))
    done = subprocess.run(
        [str(tool), "--state", str(path), "-o", str(tmp_path / "o.ppm")],
        capture_output=True,
        text=True,
    )
    assert done.returncode == 2 and "RR2VDPSTATE1" in done.stderr
