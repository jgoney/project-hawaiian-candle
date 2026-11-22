"""Find the code in a ROM by following control flow from known entry points."""

from __future__ import annotations

import hashlib
import tomllib
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from rr2port.decode import DecodeError, Insn, decode
from rr2port.trace import read_trace

RR206_SHA256 = "c7a609019b1f052bcee6204a3a09ea77a5150a748de90b51382fedf4df207848"


class WrongRomError(ValueError):
    """Raised when a ROM image is not the known-good RR206 dump."""

    def __init__(self, expected_sha256: str, actual_sha256: str) -> None:
        super().__init__(
            f"not the RR206 ROM: expected SHA-256 {expected_sha256}, found {actual_sha256}"
        )
        self.expected_sha256 = expected_sha256
        self.actual_sha256 = actual_sha256


class AnnotationError(ValueError):
    """Raised for a malformed annotation file or an impossible code target."""


@dataclass(frozen=True)
class PointerTable:
    """A table of code pointers in the ROM.

    kind    "long" (32-bit absolute addresses; zero entries are null and
            skipped), "long_offset" (32-bit signed offsets added to ``base``)
            or "word_offset" (16-bit signed offsets added to ``base``)
    stride  bytes from one entry to the next when the pointers sit inside
            larger records; 0 means the entries are contiguous
    sites   the indirect instructions that use the table
    note    how the entry count was established
    """

    address: int
    count: int
    kind: str
    base: int = 0
    stride: int = 0
    sites: tuple[int, ...] = ()
    note: str = ""


@dataclass
class Annotations:
    entries: list[int] = field(default_factory=list)
    jump_tables: dict[int, list[int]] = field(default_factory=dict)
    callback_slots: list[int] = field(default_factory=list)
    callback_registrars: list[int] = field(default_factory=list)
    pointer_tables: list[PointerTable] = field(default_factory=list)


_TOP_LEVEL_KEYS = {
    "entries", "callback_slots", "callback_registrars", "pointer_tables", "jump_tables",
}  # fmt: skip
_TABLE_KEYS = {"address", "count", "kind", "base", "stride", "sites", "note"}
_TABLE_REQUIRED = ("address", "count", "kind")
_TABLE_KINDS = {"long": 4, "long_offset": 4, "word_offset": 2}


def verify_rr206(rom: bytes) -> None:
    actual = hashlib.sha256(rom).hexdigest()
    if actual != RR206_SHA256:
        raise WrongRomError(RR206_SHA256, actual)


def load_annotations(path: Path) -> Annotations:
    """Load the code map. Unknown or missing keys are errors, never ignored."""
    with path.open("rb") as handle:
        data = tomllib.load(handle)
    for key in data:
        if key not in _TOP_LEVEL_KEYS:
            raise AnnotationError(f"{path}: unknown key '{key}'")
    tables = []
    for index, raw in enumerate(data.get("pointer_tables", [])):
        for key in raw:
            if key not in _TABLE_KEYS:
                raise AnnotationError(f"{path}: pointer_tables[{index}] has unknown key '{key}'")
        for key in _TABLE_REQUIRED:
            if key not in raw:
                raise AnnotationError(f"{path}: pointer_tables[{index}] is missing '{key}'")
        if raw["kind"] not in _TABLE_KINDS:
            raise AnnotationError(
                f"{path}: pointer_tables[{index}] has unknown kind '{raw['kind']}'"
            )
        if raw.get("stride", 0) not in (0, *range(_TABLE_KINDS[raw["kind"]], 0x10000)):
            raise AnnotationError(
                f"{path}: pointer_tables[{index}] stride is smaller than one entry"
            )
        tables.append(PointerTable(**{**raw, "sites": tuple(raw.get("sites", ()))}))
    jump_tables: dict[int, list[int]] = {}
    for key, targets in data.get("jump_tables", {}).items():
        try:
            site = int(key, 16)
        except ValueError:
            raise AnnotationError(f"{path}: jump_tables key '{key}' is not a hex address") from None
        jump_tables[site] = list(targets)
    return Annotations(
        entries=list(data.get("entries", [])),
        jump_tables=jump_tables,
        callback_slots=list(data.get("callback_slots", [])),
        callback_registrars=list(data.get("callback_registrars", [])),
        pointer_tables=tables,
    )


def pointer_table_targets(rom: bytes, table: PointerTable) -> list[int]:
    """Read the code addresses a pointer table holds."""
    width = _TABLE_KINDS[table.kind]
    stride = table.stride or width
    end = table.address + (table.count - 1) * stride + width if table.count else table.address
    if table.address < 0 or end > len(rom):
        raise AnnotationError(
            f"pointer table at {table.address:06X} with {table.count} entries runs past the ROM"
        )
    targets = []
    for index in range(table.count):
        offset = table.address + index * stride
        raw = int.from_bytes(rom[offset : offset + width], "big")
        if table.kind == "long":
            if raw != 0:
                targets.append(raw & 0xFFFFFF)
            continue
        sign_bit = 1 << (8 * width - 1)
        signed = raw - (sign_bit << 1) if raw & sign_bit else raw
        targets.append((table.base + signed) & 0xFFFFFF)
    return targets


def trace_entries(trace_path: Path) -> set[int]:
    """Every distinct instruction address executed in a trace window."""
    return {step.pc for step in read_trace(trace_path)}


def require_code(rom: bytes, source: str, target: int) -> None:
    """Reject a code target that cannot be an instruction."""
    reason = None
    if target & 1:
        reason = "is odd"
    elif target < 0 or target >= len(rom):
        reason = "is outside the ROM"
    else:
        try:
            decode(rom, target)
        except DecodeError as error:
            reason = f"does not decode ({error.reason})"
    if reason is not None:
        raise AnnotationError(f"{source}: target {target:06X} {reason}")


def _callback_target(insn: Insn, slots: frozenset[int]) -> int | None:
    """The code address a ``move.l #imm,<slot>`` installs, if it is one."""
    if insn.mnemonic != "move" or insn.opsize != 4 or len(insn.operands) != 2:
        return None
    source, destination = insn.operands
    if source.kind != "imm" or destination.kind not in ("absw", "absl"):
        return None
    if destination.value & 0xFFFFFF not in slots or source.value == 0:
        return None
    return source.value & 0xFFFFFF


def discover(
    rom: bytes,
    entries: Iterable[int],
    jump_tables: Mapping[int, Sequence[int]] | None = None,
    *,
    callback_slots: Iterable[int] = (),
    callback_registrars: Iterable[int] = (),
    pointer_tables: Iterable[PointerTable] = (),
) -> dict[int, Insn]:
    """Decode every instruction reachable from ``entries``.

    Code reached indirectly comes from three kinds of annotation:

    - ``jump_tables``: targets observed at run time for an indirect jump or call;
    - ``callback_slots``: RAM addresses holding code pointers, so the immediate
      of any discovered ``move.l #imm,<slot>`` is code;
    - ``callback_registrars``: routines that take a code pointer in A0, so the
      immediate of a ``movea.l #imm,a0`` placed directly before a call to one
      is code;
    - ``pointer_tables``: tables of code pointers in the ROM.

    A path ends at a return, an indirect jump without a jump-table annotation,
    a STOP, or bytes that do not decode. Undecodable addresses reached by
    falling through are left out, so reaching one at run time fails loudly.
    A target supplied by an annotation must be a possible instruction;
    otherwise :class:`AnnotationError` names where it came from.
    """
    tables = jump_tables or {}
    slots = frozenset(slot & 0xFFFFFF for slot in callback_slots)
    registrars = frozenset(address & 0xFFFFFF for address in callback_registrars)
    found: dict[int, Insn] = {}
    pending = list(entries)
    for table in pointer_tables:
        for target in pointer_table_targets(rom, table):
            require_code(rom, f"pointer table at {table.address:06X}", target)
            pending.append(target)
    # Follow control flow to exhaustion, then look for newly registered
    # callbacks, and repeat until that finds nothing more.
    while pending:
        while pending:
            pc = pending.pop()
            if pc in found or pc < 0 or pc >= len(rom):
                continue
            try:
                insn = decode(rom, pc)
            except DecodeError:
                continue
            found[pc] = insn
            pending.extend(insn.targets)
            for target in tables.get(pc, ()):
                require_code(rom, f"jump table for site {pc:06X}", target)
                pending.append(target)
            callback = _callback_target(insn, slots)
            if callback is not None:
                require_code(rom, f"callback installed at {pc:06X}", callback)
                pending.append(callback)
            if insn.flow in ("next", "cond", "call"):
                pending.append(pc + insn.size)
        pending.extend(_registered_callbacks(rom, found, registrars))
    _check_jump_table_sites(found, tables)
    return found


def _check_jump_table_sites(found: Mapping[int, Insn], tables: Mapping[int, Sequence[int]]) -> None:
    """Every jump table must belong to a discovered indirect jump and have targets.

    A mistyped or stale site would otherwise be ignored without a word.
    """
    for site, targets in tables.items():
        insn = found.get(site)
        if insn is None:
            raise AnnotationError(
                f"jump table for site {site:06X}: no discovered instruction at that address"
            )
        if insn.mnemonic not in ("jmp", "jsr") or insn.targets:
            raise AnnotationError(
                f"jump table for site {site:06X}: {insn.mnemonic} there is not an indirect jump"
            )
        if not targets:
            raise AnnotationError(f"jump table for site {site:06X} lists no targets")


def _registered_callbacks(
    rom: bytes, found: Mapping[int, Insn], registrars: frozenset[int]
) -> list[int]:
    """Code pointers passed in A0 to a callback registrar, not yet discovered."""
    if not registrars:
        return []
    ends = {insn.pc + insn.size: insn for insn in found.values()}
    new = []
    for insn in found.values():
        if insn.flow != "call" or not registrars.intersection(insn.targets):
            continue
        loader = ends.get(insn.pc)
        if loader is None or loader.mnemonic != "movea" or loader.opsize != 4:
            continue
        source, destination = loader.operands
        if source.kind != "imm" or destination.reg != 0:
            continue
        target = source.value & 0xFFFFFF
        if target not in found:
            require_code(rom, f"callback registered at {insn.pc:06X}", target)
            new.append(target)
    return new


def indirect_sites(insns: Mapping[int, Insn]) -> list[int]:
    """Addresses of jumps and calls whose target is not known statically."""
    return sorted(
        pc for pc, insn in insns.items() if insn.mnemonic in ("jmp", "jsr") and not insn.targets
    )
