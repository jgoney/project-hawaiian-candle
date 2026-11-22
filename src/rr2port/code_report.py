"""Report how much code discovery finds and which indirect jumps are unresolved."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass, field

from rr2port.decode import Insn, Operand
from rr2port.discover import Annotations, discover, indirect_sites, pointer_table_targets


@dataclass
class SiteRow:
    site: int
    operand: str
    table_targets: int
    observed_targets: int
    resolved: bool


@dataclass
class CodeReport:
    instructions: int
    code_bytes: int
    sites: list[SiteRow] = field(default_factory=list)
    missing: dict[str, list[int]] = field(default_factory=dict)

    @property
    def unresolved(self) -> int:
        return sum(1 for row in self.sites if not row.resolved)

    @property
    def complete(self) -> bool:
        return self.unresolved == 0 and not any(self.missing.values())


def _register(index: int) -> str:
    return f"a{index - 8}" if index >= 8 else f"d{index}"


def format_operand(op: Operand) -> str:
    """Assembler-style text for the operand kinds a jump or call can have."""
    index = f"{_register(op.xreg)}.{'l' if op.xlong else 'w'}"
    if op.kind == "ind":
        return f"(a{op.reg})"
    if op.kind == "disp":
        return f"{op.value}(a{op.reg})"
    if op.kind == "index":
        return f"{op.value}(a{op.reg},{index})"
    if op.kind == "pcindex":
        return f"${op.value:X}(pc,{index})"
    return op.kind


def describe(insn: Insn) -> str:
    return f"{insn.mnemonic} {format_operand(insn.operands[0])}"


def build_code_report(
    rom: bytes, annotations: Annotations, windows: Mapping[str, set[int]]
) -> CodeReport:
    insns = discover(
        rom,
        annotations.entries,
        annotations.jump_tables,
        callback_slots=annotations.callback_slots,
        callback_registrars=annotations.callback_registrars,
        pointer_tables=annotations.pointer_tables,
    )
    report = CodeReport(
        instructions=len(insns), code_bytes=sum(insn.size for insn in insns.values())
    )
    table_counts: dict[int, int] = {}
    for table in annotations.pointer_tables:
        count = len(pointer_table_targets(rom, table))
        for site in table.sites:
            table_counts[site] = table_counts.get(site, 0) + count
    for site in indirect_sites(insns):
        observed = len(annotations.jump_tables.get(site, ()))
        tables = table_counts.get(site, 0)
        report.sites.append(
            SiteRow(
                site=site,
                operand=describe(insns[site]),
                table_targets=tables,
                observed_targets=observed,
                resolved=site in annotations.jump_tables or site in table_counts,
            )
        )
    for name, traced in windows.items():
        report.missing[name] = sorted(pc for pc in traced if pc not in insns)
    return report


def format_code_report(report: CodeReport) -> str:
    lines = [
        f"instructions: {report.instructions}",
        f"code bytes: {report.code_bytes}",
        f"indirect sites: {len(report.sites)}",
        f"unresolved indirect sites: {report.unresolved}",
        "",
        f"{'site':<8}{'instruction':<22}{'table':>6}{'observed':>10}  status",
    ]
    for row in report.sites:
        status = "resolved" if row.resolved else "UNRESOLVED"
        lines.append(
            f"{row.site:06X}  {row.operand:<22}{row.table_targets:>6}{row.observed_targets:>10}"
            f"  {status}"
        )
    for name, missing in report.missing.items():
        lines.append("")
        lines.append(f"{name}: {len(missing)} traced addresses not discovered")
        if missing:
            shown = ", ".join(f"{pc:06X}" for pc in missing[:12])
            lines.append(f"  {shown}{' ...' if len(missing) > 12 else ''}")
    return "\n".join(lines) + "\n"
