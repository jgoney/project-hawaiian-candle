"""The Godot native extension, run in headless Godot."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest
from harness import REPO

from rr2port.picture import compare_pictures

GODOT = Path(os.environ.get("GODOT_BIN", "/Applications/Godot.app/Contents/MacOS/Godot"))
LIBRARY = REPO / "godot" / "bin" / "librr2.dylib"
ROM = REPO / "Road Rash II (USA, Europe) (RR206).gen"
BOOT_RUN = REPO / "generated" / "port" / "build" / "rr-boot-run"


def run_godot_test(script: str, env: dict[str, str]) -> subprocess.CompletedProcess:
    return subprocess.run(
        [
            str(GODOT),
            "--headless",
            "--path",
            str(REPO / "godot"),
            "--script",
            f"res://tests/{script}",
        ],
        capture_output=True,
        text=True,
        env={**os.environ, **env},
        timeout=300,
    )


@pytest.mark.skipif(not GODOT.exists() or not LIBRARY.exists(), reason="Godot or library missing")
def test_console_class_without_a_rom():
    done = run_godot_test("test_console.gd", {"RR2_ROM": ""})
    assert done.returncode == 0, done.stdout + done.stderr
    assert "FAIL" not in done.stdout and "PASS class RoadRashConsole exists" in done.stdout


@pytest.mark.skipif(
    not (GODOT.exists() and LIBRARY.exists() and ROM.exists() and BOOT_RUN.exists()),
    reason="Godot, library, ROM or rr-boot-run missing",
)
def test_console_in_godot_draws_the_same_picture_as_the_command_line_runner(tmp_path):
    godot_frame = tmp_path / "godot-0600.png"
    done = run_godot_test(
        "test_console.gd", {"RR2_ROM": str(ROM), "RR2_FRAME_OUT": str(godot_frame)}
    )
    assert done.returncode == 0, done.stdout + done.stderr
    assert "FAIL" not in done.stdout
    native_frame = tmp_path / "native-0600.ppm"
    subprocess.run(
        [str(BOOT_RUN), "--rom", str(ROM), "--snap-frame", f"600:{native_frame}"], check=True
    )
    assert compare_pictures(godot_frame, native_frame).same


@pytest.mark.skipif(not GODOT.exists(), reason="Godot missing")
def test_pad_input_mapping():
    done = run_godot_test("test_pad_input.gd", {})
    assert done.returncode == 0, done.stdout + done.stderr
    assert "FAIL" not in done.stdout and "PASS" in done.stdout


@pytest.mark.skipif(not GODOT.exists() or not LIBRARY.exists(), reason="Godot or library missing")
def test_console_scene():
    env = {"RR2_ROM": str(ROM) if ROM.exists() else ""}
    done = run_godot_test("test_console_scene.gd", env)
    assert done.returncode == 0, done.stdout + done.stderr
    assert "FAIL" not in done.stdout and "PASS" in done.stdout
