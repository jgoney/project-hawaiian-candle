"""Record where indirect jumps and calls go, using MAME, and merge the results.

The MAME debugger reads bare numbers as hexadecimal, so every number in a
generated script carries an explicit ``0x`` prefix.
"""

from __future__ import annotations

import tomllib
from collections.abc import Mapping
from pathlib import Path

from rr2port.decode import Insn, Operand
from rr2port.discover import AnnotationError, require_code
from rr2port.trace import TraceFormatError

LOG_PREFIX = "RR2IND "
SECTION = "[jump_tables]"


def _address_register(reg: int) -> str:
    return "sp" if reg == 7 else f"a{reg}"


def _index(op: Operand) -> str:
    register = _address_register(op.xreg - 8) if op.xreg >= 8 else f"d{op.xreg}"
    if op.xlong:
        return register
    return f"((({register}&0xffff)^0x8000)-0x8000)"


def target_expression(insn: Insn) -> str:
    """MAME debugger expression for the address a jmp or jsr is about to go to."""
    op = insn.operands[0]
    if op.kind == "ind":
        return _address_register(op.reg)
    if op.kind == "disp":
        sign = "-" if op.value < 0 else "+"
        return f"({_address_register(op.reg)}{sign}{abs(op.value):#x})"
    if op.kind == "index":
        sign = "-" if op.value < 0 else "+"
        return f"({_address_register(op.reg)}{sign}{abs(op.value):#x}+{_index(op)})"
    if op.kind == "pcindex":
        return f"({op.value:#x}+{_index(op)})"
    raise ValueError(f"{insn.pc:06X}: {insn.mnemonic} operand {op.kind} is not indirect")


def build_indirect_cmd(sites: Mapping[int, Insn]) -> str:
    """Debugger script that logs ``RR2IND <site> <target>`` each time a site runs."""
    lines = ["focus maincpu"]
    for site in sorted(sites):
        expression = target_expression(sites[site])
        lines.append(
            f'bp {site:#x},1,{{logerror "RR2IND %06X %06X\\n",{site:#x},'
            f"({expression})&0xffffff; g}}"
        )
    lines.append("g")
    return "\n".join(lines) + "\n"


def read_indirect_log(error_log: Path) -> dict[int, set[int]]:
    """Collect the distinct targets per site from MAME's error log."""
    observed: dict[int, set[int]] = {}
    with error_log.open(errors="replace") as handle:
        for line_number, line in enumerate(handle, 1):
            if not line.startswith(LOG_PREFIX):
                continue
            fields = line.split()
            try:
                if len(fields) != 3:
                    raise ValueError("expected 'RR2IND site target'")
                site, target = int(fields[1], 16), int(fields[2], 16)
            except ValueError as error:
                raise TraceFormatError(line_number, str(error)) from None
            observed.setdefault(site, set()).add(target)
    return observed


def merge_jump_tables(annotation_text: str, observed: Mapping[int, set[int]], rom: bytes) -> str:
    """Return the annotation text with ``observed`` merged into ``[jump_tables]``.

    Everything above the section is kept byte for byte. The section must be the
    last one in the file, because it is regenerated from its header to the end.
    """
    lines = annotation_text.splitlines(keepends=True)
    headers = [index for index, line in enumerate(lines) if line.strip() == SECTION]
    if len(headers) != 1:
        raise AnnotationError(f"annotation file needs exactly one {SECTION} section")
    start = headers[0]
    if any(line.lstrip().startswith("[") for line in lines[start + 1 :]):
        raise AnnotationError(f"{SECTION} must be the last section of the annotation file")
    try:
        existing = tomllib.loads(annotation_text).get("jump_tables", {})
        merged = {int(key, 16): set(targets) for key, targets in existing.items()}
    except (tomllib.TOMLDecodeError, ValueError) as error:
        raise AnnotationError(f"cannot read existing {SECTION}: {error}") from None
    for site, targets in observed.items():
        for target in targets:
            require_code(rom, f"jump table for site {site:06X}", target)
        merged.setdefault(site, set()).update(targets)
    body = [
        f'"{site:06X}" = [{", ".join(f"0x{target:06X}" for target in sorted(merged[site]))}]\n'
        for site in sorted(merged)
    ]
    return "".join(lines[: start + 1]) + "".join(body)
