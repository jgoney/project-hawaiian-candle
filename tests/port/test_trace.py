from __future__ import annotations

import pytest

from rr2port.trace import (
    Interrupt,
    TraceFormatError,
    TraceStep,
    find_interrupts,
    read_hw_log,
    read_trace,
    split_hw_log,
    split_raw_trace,
)

LINE = "T 000200 2700 " + " ".join(["00000000"] * 15) + " 00FFFFF6 123"


def step(*, pc: int, sr: int, sp: int) -> TraceStep:
    return TraceStep(pc=pc, sr=sr, d=(0,) * 8, a=(0,) * 7 + (sp,), cycles=0)


def test_read_trace_parses_fields(tmp_path):
    p = tmp_path / "trace.txt"
    p.write_text(LINE + "\nE 1\n")
    (parsed,) = read_trace(p)
    assert (parsed.pc, parsed.sr, parsed.a[7], parsed.cycles) == (0x200, 0x2700, 0x00FFFFF6, 123)


def test_read_trace_reports_line_number_of_truncated_line(tmp_path):
    p = tmp_path / "trace.txt"
    p.write_text(LINE + "\n" + LINE[:40] + "\n")
    with pytest.raises(TraceFormatError) as e:
        list(read_trace(p))
    assert e.value.line_number == 2


def test_find_interrupts_detects_vblank_entry():
    before = step(pc=0x1000, sr=0x2000, sp=0xFFFF00)
    after = step(pc=0x11B8, sr=0x2600, sp=0xFFFEFA)
    assert find_interrupts([before, after]) == [Interrupt(index=1, level=6)]


def test_find_interrupts_ignores_plain_jump_to_handler():
    steps = [step(pc=0x1000, sr=0x2000, sp=0xFFFF00), step(pc=0x11B8, sr=0x2000, sp=0xFFFF00)]
    assert find_interrupts(steps) == []


def test_read_hw_log_parses_reads_and_writes(tmp_path):
    p = tmp_path / "hw.txt"
    p.write_text("R C00004 2 3400\nW A11100 2 0100\n")
    log = read_hw_log(p)
    assert [(h.kind, h.addr, h.size, h.value) for h in log] == [
        ("R", 0xC00004, 2, 0x3400),
        ("W", 0xA11100, 2, 0x0100),
    ]


def test_read_hw_log_rejects_unknown_kind(tmp_path):
    p = tmp_path / "hw.txt"
    p.write_text("X 0C0004 2 0000\n")
    with pytest.raises(TraceFormatError) as e:
        read_hw_log(p)
    assert e.value.line_number == 1


RAW_REGS = " ".join(["00000000"] * 15) + " 00FFFFF6"


def raw_pair(pc: int, cycles: int) -> str:
    return f"T 2700 {RAW_REGS} {cycles}\n{pc:06X}: nop\n"


def test_split_raw_trace_keeps_lines_between_markers_and_takes_pc_from_disassembly(tmp_path):
    raw = tmp_path / "raw.txt"
    raw.write_text(
        raw_pair(0x100, 10)
        + raw_pair(0x206, 20)
        + "RR2START\n"
        + raw_pair(0x208, 30)
        + raw_pair(0x20E, 40)
        + "RR2END\n"
        + raw_pair(0x210, 50)
    )
    out = tmp_path / "trace.txt"
    assert split_raw_trace(raw, out) == 3
    steps = list(read_trace(out))
    assert [s.pc for s in steps] == [0x206, 0x208, 0x20E]
    assert steps[0].cycles == 20 and steps[0].a[7] == 0x00FFFFF6


def test_split_raw_trace_without_end_marker_is_an_error(tmp_path):
    raw = tmp_path / "raw.txt"
    raw.write_text(raw_pair(0x206, 20) + "RR2START\n" + raw_pair(0x208, 30))
    with pytest.raises(TraceFormatError):
        split_raw_trace(raw, tmp_path / "trace.txt")


def test_split_hw_log_converts_bit_sizes_and_ignores_other_rows(tmp_path):
    log = tmp_path / "error.log"
    log.write_text("noise\nRR2HW R A10001 8 A0\nRR2HW W C00004 10 8004\n")
    out = tmp_path / "hw.txt"
    assert split_hw_log(log, out) == 2
    assert out.read_text() == "R A10001 1 A0\nW C00004 2 8004\n"


def test_find_interrupts_detects_entry_right_after_the_mask_is_lowered():
    # move #$2000,sr at mask 7, then the pending level-6 interrupt is taken at once.
    steps = [step(pc=0x33E, sr=0x2700, sp=0xFFFF00), step(pc=0x11B8, sr=0x2600, sp=0xFFFEFA)]
    assert find_interrupts(steps) == [Interrupt(index=1, level=6)]


def test_find_interrupts_detects_back_to_back_entry_after_rte():
    # rte at the end of the HBlank handler, then the next HBlank is taken at once:
    # the mask is 4 on both lines and the stack pointer is unchanged.
    steps = [step(pc=0x15D8, sr=0x2409, sp=0xFFFEFA), step(pc=0x14BE, sr=0x2409, sp=0xFFFEFA)]
    assert find_interrupts(steps) == [Interrupt(index=1, level=4)]


def test_read_trace_requires_the_end_line(tmp_path):
    p = tmp_path / "trace.txt"
    p.write_text(LINE + "\n" + LINE + "\n")  # cut at a line boundary
    with pytest.raises(TraceFormatError) as e:
        list(read_trace(p))
    assert e.value.line_number == 3 and "end line" in e.value.reason


def test_read_trace_rejects_an_end_line_with_the_wrong_count(tmp_path):
    p = tmp_path / "trace.txt"
    p.write_text(LINE + "\nE 2\n")
    with pytest.raises(TraceFormatError) as e:
        list(read_trace(p))
    assert e.value.line_number == 2


def test_split_raw_trace_writes_the_end_line(tmp_path):
    raw = tmp_path / "raw.txt"
    raw.write_text(raw_pair(0x206, 20) + "RR2START\n" + raw_pair(0x208, 30) + "RR2END\n")
    out = tmp_path / "trace.txt"
    assert split_raw_trace(raw, out) == 2
    assert out.read_text().splitlines()[-1] == "E 2"
