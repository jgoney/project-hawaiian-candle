"""The trace conformance checker, exercised on a tiny synthetic window."""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest
from harness import REPO, ROM_SIZE, build_snippet

CHECKER = REPO / "port" / "tools" / "rr_trace_check.c"
# 0: moveq #1,d0 ; 2: addq.w #1,d0 ; 4: bra.s 4
CODE = bytes.fromhex("7001524060FE")
SP = 0x00FFFF00


def line(pc: int, d0: int, cycles: int, sr: int = 0x2700, sp: int = SP) -> str:
    regs = [d0] + [0] * 14 + [sp]
    return f"T {pc:06X} {sr:04X} " + " ".join(f"{r:08X}" for r in regs) + f" {cycles}\n"


def trace_text(lines) -> str:
    return "".join(lines) + f"E {len(lines)}\n"


GOOD_TRACE = [line(0, 0, 0), line(2, 1, 4), line(4, 2, 8), line(4, 2, 18), line(4, 2, 28)]


@pytest.fixture
def window(tmp_path):
    directory = tmp_path / "window"
    directory.mkdir()
    (directory / "trace.txt").write_text(trace_text(GOOD_TRACE))
    (directory / "hw.txt").write_text("")
    (directory / "ram-start.bin").write_bytes(bytes(0x10000))
    (directory / "ram-end.bin").write_bytes(bytes(0x10000))
    return directory


def check(tmp_path: Path, window: Path, *extra: str, rom: Path | None = None):
    binary, rom_path = build_snippet(CODE, tmp_path, main=CHECKER, defines=["RR_ANY_ROM"])
    return subprocess.run(
        [str(binary), "--rom", str(rom or rom_path), "--window", str(window), *extra],
        capture_output=True,
        text=True,
    )


def test_matching_window_reports_ok(tmp_path, window):
    done = check(tmp_path, window)
    assert done.returncode == 0, done.stdout + done.stderr
    assert done.stdout.startswith("OK insns=4 interrupts=0 handoffs=0 hw=0 ")


def test_register_divergence_is_reported_with_index(tmp_path, window):
    lines = list(GOOD_TRACE)
    lines[2] = line(4, 3, 8)
    (window / "trace.txt").write_text(trace_text(lines))
    done = check(tmp_path, window)
    assert done.returncode == 1
    assert "DIVERGE insn=2" in done.stdout and "field=d0" in done.stdout
    assert "want=00000003 got=00000002" in done.stdout


def test_truncated_trace_is_an_error_not_a_pass(tmp_path, window):
    text = "".join(GOOD_TRACE)
    (window / "trace.txt").write_text(text[: len(text) - 60])
    done = check(tmp_path, window)
    assert done.returncode == 2
    assert "line 5" in done.stderr


def test_unconsumed_hardware_log_fails(tmp_path, window):
    (window / "hw.txt").write_text("R C00004 2 3400\n")
    done = check(tmp_path, window)
    assert done.returncode == 1
    assert "hardware" in done.stdout


def test_ram_end_difference_fails(tmp_path, window):
    ram = bytearray(0x10000)
    ram[0x1234] = 0x55
    (window / "ram-end.bin").write_bytes(ram)
    done = check(tmp_path, window)
    assert done.returncode == 1
    assert "RAMDIFF" in done.stdout and "first_offset=1234" in done.stdout


def test_wrong_rom_is_refused(tmp_path, window):
    other = tmp_path / "other.rom"
    other.write_bytes(b"\x4e\x71" * (ROM_SIZE // 2))
    done = check(tmp_path, window, rom=other)
    assert done.returncode == 2
    assert "CRC" in done.stderr


def test_cycles_out_lists_cost_per_instruction(tmp_path, window):
    out = tmp_path / "cycles.txt"
    done = check(tmp_path, window, "--cycles-out", str(out))
    assert done.returncode == 0
    assert out.read_text().splitlines() == [
        "0 000000 4 0",
        "1 000002 4 0",
        "2 000004 10 0",
        "3 000004 10 0",
    ]


def ok_line(done) -> str:
    assert done.returncode == 0, done.stdout + done.stderr
    return done.stdout.split(" seconds=")[0]


# Vector 30 (level 6) -> 0x100. 0x80: nop ; 0x82: nop ; 0x84: bra.s 0x84 ; 0x100: rte
INTERRUPT_IMAGE = (
    bytes(0x78) + bytes.fromhex("00000100") + bytes(4)
    + bytes.fromhex("4E71" "4E71" "60FE").ljust(0x80, b"\xff") + bytes.fromhex("4E73")
)  # fmt: skip


@pytest.fixture
def interrupt_window(tmp_path):
    directory = tmp_path / "irq-window"
    directory.mkdir()
    trace = [
        line(0x80, 0, 0, sr=0x2000),
        line(0x82, 0, 4, sr=0x2000),
        line(0x100, 0, 52, sr=0x2600, sp=SP - 6),
        line(0x84, 0, 72, sr=0x2000),
        line(0x84, 0, 82, sr=0x2000),
    ]
    (directory / "trace.txt").write_text(trace_text(trace))
    (directory / "hw.txt").write_text("")
    (directory / "ram-start.bin").write_bytes(bytes(0x10000))
    ram = bytearray(0x10000)
    ram[0xFEFA:0xFF00] = bytes.fromhex("200000000084")
    (directory / "ram-end.bin").write_bytes(ram)
    return directory


def check_interrupt(tmp_path, window, *extra):
    binary, rom_path = build_snippet(
        INTERRUPT_IMAGE, tmp_path, entries=(0x80, 0x100), main=CHECKER, defines=["RR_ANY_ROM"]
    )
    return subprocess.run(
        [str(binary), "--rom", str(rom_path), "--window", str(window), *extra],
        capture_output=True,
        text=True,
    )


def test_interrupt_is_injected_where_the_trace_enters_the_handler(tmp_path, interrupt_window):
    done = check_interrupt(tmp_path, interrupt_window)
    assert ok_line(done) == "OK insns=4 interrupts=1 handoffs=0 hw=0"


def test_slicing_does_not_change_result(tmp_path, window):
    assert ok_line(check(tmp_path, window, "--slice", "1")) == ok_line(check(tmp_path, window))


def test_save_restore_mid_run_does_not_change_result(tmp_path, window):
    done = check(tmp_path, window, "--save-restore-at", "2")
    assert ok_line(done) == ok_line(check(tmp_path, window))
    assert "restored at insn=2" in done.stderr


def test_save_restore_inside_interrupt_handler(tmp_path, interrupt_window):
    done = check_interrupt(tmp_path, interrupt_window, "--save-restore-at", "2")
    assert ok_line(done) == "OK insns=4 interrupts=1 handoffs=0 hw=0"


def test_save_restore_point_past_the_end_is_an_error(tmp_path, window):
    done = check(tmp_path, window, "--save-restore-at", "99")
    assert done.returncode == 2
    assert "never reached" in done.stderr


def test_trace_cut_at_a_line_boundary_is_an_error_not_a_pass(tmp_path, window):
    (window / "trace.txt").write_text("".join(GOOD_TRACE[:2]))
    done = check(tmp_path, window)
    assert done.returncode == 2
    assert "end line" in done.stderr


def test_end_line_with_the_wrong_count_is_an_error(tmp_path, window):
    (window / "trace.txt").write_text("".join(GOOD_TRACE) + "E 9\n")
    done = check(tmp_path, window)
    assert done.returncode == 2
    assert "E 9" in done.stderr or "9" in done.stderr


# 0: nop ; 2: bra.s 0  -- placed so that 0xDF48 is the loop head is not possible in a
# small ROM, so the handoff tests use the real handoff address through HANDOFF_PC.
HANDOFF_CODE = bytes.fromhex("4E7160FC")


def handoff_window(tmp_path, files):
    directory = tmp_path / "handoff-window"
    directory.mkdir()
    trace = [line(0, 0, 0), line(2, 0, 4), line(0, 0, 14), line(2, 0, 18), line(0, 0, 28)]
    (directory / "trace.txt").write_text(trace_text(trace))
    (directory / "hw.txt").write_text("")
    for name in ("ram-start.bin", "ram-end.bin", *files):
        (directory / name).write_bytes(bytes(0x10000))
    return directory


def check_handoff(tmp_path, directory):
    binary, rom_path = build_snippet(
        HANDOFF_CODE, tmp_path, main=CHECKER, defines=["RR_ANY_ROM", "HANDOFF_PC=0u"]
    )
    return subprocess.run(
        [str(binary), "--rom", str(rom_path), "--window", str(directory)],
        capture_output=True,
        text=True,
    )


def test_every_handoff_file_is_compared(tmp_path):
    names = ["handoff-00.bin", "handoff-01.bin", "handoff-02.bin"]
    done = check_handoff(tmp_path, handoff_window(tmp_path, names))
    assert ok_line(done) == "OK insns=4 interrupts=0 handoffs=3 hw=0"


def test_missing_handoff_file_fails_instead_of_skipping_the_rest(tmp_path):
    done = check_handoff(tmp_path, handoff_window(tmp_path, ["handoff-00.bin", "handoff-02.bin"]))
    assert done.returncode == 1
    assert "HANDOFFS" in done.stdout and "handoff-02.bin" in done.stdout


# 0: move.b $a10001.l,d0 ; 6: move.w #$8f02,$c00004.l ; e: bra.s e
SHADOW_CODE = bytes.fromhex("103900A1000133FC8F0200C0000460FE")


def shadow_window(tmp_path, version: int):
    directory = tmp_path / f"shadow-window-{version:02X}"
    directory.mkdir()
    trace = [
        line(0, 0, 0),
        line(6, version, 16, sr=0x2708),
        line(0xE, version, 36, sr=0x2708),
        line(0xE, version, 46, sr=0x2708),
    ]
    (directory / "trace.txt").write_text(trace_text(trace))
    (directory / "hw.txt").write_text(f"R A10001 1 {version:02X}\nW C00004 2 8F02\n")
    (directory / "ram-start.bin").write_bytes(bytes(0x10000))
    (directory / "ram-end.bin").write_bytes(bytes(0x10000))
    return directory


def check_shadow(tmp_path, directory, *extra):
    binary, rom_path = build_snippet(SHADOW_CODE, tmp_path, main=CHECKER, defines=["RR_ANY_ROM"])
    return subprocess.run(
        [str(binary), "--rom", str(rom_path), "--window", str(directory), *extra],
        capture_output=True,
        text=True,
    )


def test_shadow_hardware_agreeing_with_the_recording(tmp_path):
    done = check_shadow(tmp_path, shadow_window(tmp_path, 0xA0), "--shadow-hw")
    assert done.returncode == 0, done.stdout + done.stderr
    assert "SHADOW addr=A10001 reads=1 differ=0" in done.stdout
    assert done.stdout.splitlines()[-1].startswith("OK insns=3 ")


def test_shadow_hardware_disagreement_is_reported_but_not_fatal(tmp_path):
    done = check_shadow(tmp_path, shadow_window(tmp_path, 0x80), "--shadow-hw")
    assert done.returncode == 0, done.stdout + done.stderr
    assert "SHADOW addr=A10001 reads=1 differ=1" in done.stdout


def test_without_shadow_no_shadow_lines_are_printed(tmp_path):
    done = check_shadow(tmp_path, shadow_window(tmp_path, 0xA0))
    assert done.returncode == 0 and "SHADOW" not in done.stdout


def test_shadow_cannot_be_combined_with_save_restore(tmp_path):
    done = check_shadow(
        tmp_path, shadow_window(tmp_path, 0xA0), "--shadow-hw", "--save-restore-at", "1"
    )
    assert done.returncode == 2 and "--shadow-hw" in done.stderr


# 0: move.w #$0100,$a11100.l ; 8: move.b $a11100.l,d0 ; e: bra.s e
BUSREQ_CODE = bytes.fromhex("33FC010000A11100103900A1110060FE")


def test_shadow_compares_only_the_defined_bit_of_the_bus_request_register(tmp_path):
    directory = tmp_path / "busreq-window"
    directory.mkdir()
    # MAME returns open-bus junk in bits 1-7; only bit 0 (0 = bus granted) is defined.
    trace = [line(0, 0, 0), line(8, 0, 20), line(0xE, 0x3A, 36), line(0xE, 0x3A, 46)]
    (directory / "trace.txt").write_text(trace_text(trace))
    (directory / "hw.txt").write_text("W A11100 2 100\nR A11100 1 3A\n")
    (directory / "ram-start.bin").write_bytes(bytes(0x10000))
    (directory / "ram-end.bin").write_bytes(bytes(0x10000))
    binary, rom_path = build_snippet(BUSREQ_CODE, tmp_path, main=CHECKER, defines=["RR_ANY_ROM"])
    done = subprocess.run(
        [str(binary), "--rom", str(rom_path), "--window", str(directory), "--shadow-hw"],
        capture_output=True,
        text=True,
    )
    assert done.returncode == 0, done.stdout + done.stderr
    assert "SHADOW addr=A11100 reads=1 differ=0" in done.stdout


# 0: move.w $c00008.l,d0 ; 6: bra.s 6
HV_CODE = bytes.fromhex("303900C0000860FE")


def test_shadow_reports_the_high_byte_of_word_reads_separately(tmp_path):
    directory = tmp_path / "hv-window"
    directory.mkdir()
    # At the very start of a frame the model's counter is 0x0000. The recording
    # says 0x00FF: the word differs, the high (line) byte agrees.
    trace = [line(0, 0, 0), line(6, 0x00FF, 16), line(6, 0x00FF, 26)]
    (directory / "trace.txt").write_text(trace_text(trace))
    (directory / "hw.txt").write_text("R C00008 2 00FF\n")
    (directory / "ram-start.bin").write_bytes(bytes(0x10000))
    (directory / "ram-end.bin").write_bytes(bytes(0x10000))
    binary, rom_path = build_snippet(HV_CODE, tmp_path, main=CHECKER, defines=["RR_ANY_ROM"])
    done = subprocess.run(
        [str(binary), "--rom", str(rom_path), "--window", str(directory), "--shadow-hw"],
        capture_output=True,
        text=True,
    )
    assert done.returncode == 0, done.stdout + done.stderr
    assert "SHADOW addr=C00008 reads=1 differ=1 high_differ=0" in done.stdout
