from __future__ import annotations

from rr2port.report import build_report, format_report
from rr2port.trace import TraceStep


def step(pc: int, cycles: int, sr: int = 0x2000) -> TraceStep:
    return TraceStep(pc=pc, sr=sr, d=(0,) * 8, a=(0,) * 8, cycles=cycles)


def names(pc: int) -> str:
    return {0x100: "add", 0x102: "add", 0x104: "nop"}.get(pc, "other")


def test_cycle_agreement_counts_mismatches():
    steps = [step(0x100, 0), step(0x102, 4), step(0x104, 12)]
    report = build_report(steps, [(0, 0x100, 4), (1, 0x102, 10)], names)
    assert report.per_mnemonic == {"add": [2, 1]}
    assert report.worst == [(2, 1, 0x102, "add", 8, 10)]
    assert report.compared == 2 and report.agreeing == 1


def test_interrupt_spacing():
    steps = [
        step(0x100, 0),
        step(0x11B8, 1000, sr=0x2600),
        step(0x100, 1100),
        step(0x11B8, 2000, sr=0x2600),
        step(0x100, 2100),
    ]
    ours = [(0, 0x100, 900), (1, 0x11B8, 100), (2, 0x100, 850), (3, 0x11B8, 100)]
    report = build_report(steps, ours, names)
    assert [(row.index, row.level, row.mame_spacing) for row in report.interrupts] == [
        (1, 6, None),
        (3, 6, 1000),
    ]
    # ours: 100 (handler) + 850 + 44 for the interrupt entry itself
    assert report.interrupts[1].our_spacing == 994


def test_instruction_before_an_interrupt_is_not_counted_as_a_cost_mismatch():
    steps = [step(0x100, 0), step(0x11B8, 60, sr=0x2600), step(0x100, 70)]
    report = build_report(steps, [(0, 0x100, 4), (1, 0x11B8, 10)], names)
    assert report.per_mnemonic == {"other": [1, 0]}
    assert report.interrupts[0].entry_cycles == 56


def test_report_handles_zero_totalcycles():
    steps = [step(0x100, 0), step(0x102, 0), step(0x104, 0)]
    report = build_report(steps, [(0, 0x100, 4), (1, 0x102, 4)], names)
    text = format_report(report)
    assert not report.cycles_available
    assert "MAME cycle data unavailable" in text
    assert "Cycle agreement" not in text


def test_interrupt_entry_cost_can_come_from_the_cost_file():
    steps = [
        step(0x100, 0),
        step(0x11B8, 1000, sr=0x2600),
        step(0x100, 1100),
        step(0x11B8, 2000, sr=0x2600),
        step(0x100, 2100),
    ]
    # A fourth column gives the cycles our build charged for the interrupt
    # entry that followed the instruction.
    ours = [(0, 0x100, 900, 55), (1, 0x11B8, 100, 0), (2, 0x100, 845, 55), (3, 0x11B8, 100, 0)]
    report = build_report(steps, ours, names)
    assert report.interrupts[1].our_spacing == 1000
