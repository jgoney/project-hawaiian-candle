"""Compare the translator's cycle costs and interrupt timing with a MAME trace."""

from __future__ import annotations

from collections import Counter
from collections.abc import Callable, Iterable, Mapping
from dataclasses import dataclass, field

from rr2port.trace import DEFAULT_VECTORS, TraceStep

INTERRUPT_ENTRY_CYCLES = 44


@dataclass
class InterruptRow:
    index: int
    level: int
    mame_cycles: int
    mame_spacing: int | None  # MAME cycles since the previous interrupt of this level
    our_spacing: int | None  # our cycles over the same span
    entry_cycles: int  # MAME cycles from the interrupted instruction's start to the handler


@dataclass
class Report:
    cycles_available: bool = True
    compared: int = 0
    agreeing: int = 0
    per_mnemonic: dict[str, list[int]] = field(default_factory=dict)  # name -> [compared, wrong]
    worst: list[tuple[int, int, int, str, int, int]] = field(default_factory=list)
    interrupts: list[InterruptRow] = field(default_factory=list)


def build_report(
    steps: Iterable[TraceStep],
    ours: Iterable[tuple[int, ...]],
    mnemonic_of: Callable[[int], str],
    vectors: Mapping[int, int] = DEFAULT_VECTORS,
) -> Report:
    """Walk a trace and our per-instruction costs together.

    Each cost row is (index, pc, cycles) or (index, pc, cycles, entry), where
    entry is what our build charged for an interrupt entry after the
    instruction. Without it the textbook 44 cycles is assumed.
    """
    report = Report()
    level_for_target = {target: level for level, target in vectors.items()}
    our_total = 0
    last_seen: dict[int, tuple[int, int]] = {}  # level -> (mame cycles, our total)
    any_cycles = False
    iterator = iter(steps)
    previous = next(iterator, None)
    our_iter = iter(ours)
    for index, current in enumerate(iterator, 1):
        if previous is None:
            break
        ours_row = next(our_iter, None)
        if ours_row is None:
            break
        _, our_pc, our_cost, *rest = ours_row
        our_entry = rest[0] if rest else INTERRUPT_ENTRY_CYCLES
        if our_pc != previous.pc & 0xFFFFFF:
            raise ValueError(f"cost file is out of step with the trace at instruction {index - 1}")
        any_cycles = any_cycles or current.cycles != 0 or previous.cycles != 0
        delta = current.cycles - previous.cycles
        our_total += our_cost
        level = level_for_target.get(current.pc)
        entered = level is not None and (current.sr >> 8) & 7 == level
        if entered:
            our_total += our_entry
            seen = last_seen.get(level)
            report.interrupts.append(
                InterruptRow(
                    index=index,
                    level=level,
                    mame_cycles=current.cycles,
                    mame_spacing=None if seen is None else current.cycles - seen[0],
                    our_spacing=None if seen is None else our_total - seen[1],
                    entry_cycles=delta - our_cost,
                )
            )
            last_seen[level] = (current.cycles, our_total)
        else:
            name = mnemonic_of(previous.pc & 0xFFFFFF)
            counts = report.per_mnemonic.setdefault(name, [0, 0])
            counts[0] += 1
            report.compared += 1
            if delta == our_cost:
                report.agreeing += 1
            else:
                counts[1] += 1
                report.worst.append(
                    (abs(delta - our_cost), index - 1, previous.pc, name, delta, our_cost)
                )
                if len(report.worst) > 2000:
                    report.worst = sorted(report.worst, reverse=True)[:10]
        previous = current
    report.worst = sorted(report.worst, reverse=True)[:10]
    report.cycles_available = any_cycles
    return report


def _spacing_summary(values: list[int]) -> str:
    if not values:
        return "no data"
    mode, mode_count = Counter(values).most_common(1)[0]
    return f"min {min(values)}, max {max(values)}, mode {mode} ({mode_count} of {len(values)})"


def format_report(report: Report) -> str:
    lines: list[str] = []
    if not report.cycles_available:
        lines.append("MAME cycle data unavailable (totalcycles is 0 throughout).")
    else:
        lines.append("Cycle agreement (instructions not followed by an interrupt)")
        lines.append(f"{'mnemonic':<10}{'compared':>12}{'differ':>10}")
        for name, (compared, wrong) in sorted(
            report.per_mnemonic.items(), key=lambda item: (-item[1][1], item[0])
        ):
            lines.append(f"{name:<10}{compared:>12}{wrong:>10}")
        if report.worst:
            lines.append("")
            lines.append("Largest disagreements (instruction, pc, mnemonic, MAME, ours)")
            for _, index, pc, name, mame, ours in report.worst:
                lines.append(f"  {index:>9} {pc:06X} {name:<8} {mame:>6} {ours:>6}")
    lines.append("")
    lines.append("Interrupt placement")
    lines.append(f"{'insn':>10} {'lvl':>3} {'MAME cycles':>14} {'MAME gap':>10} {'our gap':>10}")
    shown = report.interrupts if len(report.interrupts) <= 40 else report.interrupts[:40]
    for row in shown:
        gap = "-" if row.mame_spacing is None else str(row.mame_spacing)
        our_gap = "-" if row.our_spacing is None else str(row.our_spacing)
        lines.append(
            f"{row.index:>10} {row.level:>3} {row.mame_cycles:>14} {gap:>10} {our_gap:>10}"
        )
    if len(report.interrupts) > len(shown):
        lines.append(f"  ... {len(report.interrupts) - len(shown)} more")
    lines.append("")
    lines.append("Summary")
    for level in sorted({row.level for row in report.interrupts}):
        rows = [row for row in report.interrupts if row.level == level]
        mame = [row.mame_spacing for row in rows if row.mame_spacing is not None]
        ours = [row.our_spacing for row in rows if row.our_spacing is not None]
        drift = [o - m for o, m in zip(ours, mame, strict=True)]
        lines.append(f"  level {level}: {len(rows)} interrupts")
        if report.cycles_available:
            lines.append(f"    MAME spacing: {_spacing_summary(mame)}")
            lines.append(f"    our spacing:  {_spacing_summary(ours)}")
            lines.append(f"    ours - MAME:  {_spacing_summary(drift)}")
            entry = [row.entry_cycles for row in rows]
            lines.append(f"    entry cycles: {_spacing_summary(entry)}")
    if report.cycles_available and report.compared:
        share = 100.0 * report.agreeing / report.compared
        lines.append(
            f"  instruction costs agreeing: {report.agreeing} of {report.compared} ({share:.3f}%)"
        )
    return "\n".join(lines) + "\n"
