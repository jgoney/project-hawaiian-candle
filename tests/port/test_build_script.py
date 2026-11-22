from __future__ import annotations

import subprocess

from harness import REPO

SCRIPT = REPO / "scripts" / "build-godot-extension.sh"


def test_build_refuses_a_rom_that_is_not_rr206(tmp_path):
    rom = tmp_path / "zero.gen"
    rom.write_bytes(bytes(0x100000))
    before = sorted((REPO / "godot" / "bin").glob("*")) if (REPO / "godot" / "bin").exists() else []
    done = subprocess.run([str(SCRIPT), str(rom)], capture_output=True, text=True)
    assert done.returncode != 0
    assert "SHA-256 mismatch" in done.stderr
    after = sorted((REPO / "godot" / "bin").glob("*")) if (REPO / "godot" / "bin").exists() else []
    assert after == before


def test_build_reports_a_missing_rom(tmp_path):
    done = subprocess.run(
        [str(SCRIPT), str(tmp_path / "missing.gen")], capture_output=True, text=True
    )
    assert done.returncode != 0
    assert "missing.gen" in done.stderr


FM_LOG_SCRIPT = REPO / "scripts" / "run-mame-fm-log-rr206.sh"


def test_fm_log_script_needs_a_plain_capture_name():
    done = subprocess.run([str(FM_LOG_SCRIPT), "../etc"], capture_output=True, text=True)
    assert done.returncode != 0
    assert "usage" in done.stderr


def test_fm_log_script_reports_a_missing_rom(tmp_path):
    done = subprocess.run(
        [str(FM_LOG_SCRIPT), "no-such-capture", str(tmp_path / "missing.gen")],
        capture_output=True,
        text=True,
    )
    assert done.returncode != 0
    assert "missing.gen" in done.stderr or "mame is not installed" in done.stderr
