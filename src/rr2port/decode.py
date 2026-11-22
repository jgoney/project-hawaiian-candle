"""MC68000 instruction decoder for the port translator.

Only the plain 68000 instruction set is accepted. 68010+ encodings, line-A,
line-F and the ILLEGAL opcode raise :class:`DecodeError`, so code discovery
stops instead of walking into data.
"""

from __future__ import annotations

from dataclasses import dataclass

CONDITIONS = (
    "t", "f", "hi", "ls", "cc", "cs", "ne", "eq",
    "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le",
)  # fmt: skip
_SIZES = {0: 1, 1: 2, 2: 4}
_BIT_OPS = ("btst", "bchg", "bclr", "bset")
_IMM_OPS = {0: "ori", 1: "andi", 2: "subi", 3: "addi", 5: "eori", 6: "cmpi"}
_SHIFTS = ("as", "ls", "rox", "ro")


class DecodeError(ValueError):
    """Raised when the bytes at ``pc`` are not a valid 68000 instruction."""

    def __init__(self, pc: int, reason: str) -> None:
        super().__init__(f"{pc:06X}: {reason}")
        self.pc = pc
        self.reason = reason


@dataclass(frozen=True)
class Operand:
    """One instruction operand.

    kind is one of: dreg, areg, ind, postinc, predec, disp, index, absw, absl,
    pcdisp, pcindex, imm, reglist, ccr, sr, usp.

    reg    register number for register and address-register-based modes
    value  displacement (disp, index), absolute address (absw, absl), resolved
           base address (pcdisp, pcindex), immediate value, or register mask
           with bit n = D<n> and bit 8+n = A<n> (reglist)
    xreg   index register 0-15 (8+ = address register) for index, pcindex
    xlong  True when the index register is used as a long
    """

    kind: str
    reg: int = 0
    value: int = 0
    xreg: int = 0
    xlong: bool = False

    @property
    def is_memory(self) -> bool:
        return self.kind in (
            "ind", "postinc", "predec", "disp", "index", "absw", "absl", "pcdisp", "pcindex",
        )  # fmt: skip


@dataclass(frozen=True)
class Insn:
    pc: int
    size: int
    mnemonic: str
    opsize: int
    operands: tuple[Operand, ...]
    flow: str
    targets: tuple[int, ...]
    cond: int | None = None


class _Reader:
    def __init__(self, rom: bytes, pc: int) -> None:
        self.rom = rom
        self.start = pc
        self.pos = pc

    def word(self) -> int:
        if self.pos + 2 > len(self.rom):
            raise DecodeError(self.start, "instruction runs past end of ROM")
        value = self.rom[self.pos] << 8 | self.rom[self.pos + 1]
        self.pos += 2
        return value

    def long(self) -> int:
        high = self.word()
        return high << 16 | self.word()


def _signed(value: int, bits: int) -> int:
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def _index(reader: _Reader, kind: str, reg: int, base: int) -> Operand:
    ext = reader.word()
    if ext & 0x0100:
        raise DecodeError(reader.start, "68020 full extension word")
    disp = _signed(ext & 0xFF, 8)
    xreg = (ext >> 12) & 0xF
    xlong = bool(ext & 0x0800)
    if kind == "pcindex":
        return Operand(kind, value=base + disp, xreg=xreg, xlong=xlong)
    return Operand(kind, reg=reg, value=disp, xreg=xreg, xlong=xlong)


def _ea(reader: _Reader, mode: int, reg: int, size: int) -> Operand:
    if mode == 0:
        return Operand("dreg", reg=reg)
    if mode == 1:
        return Operand("areg", reg=reg)
    if mode == 2:
        return Operand("ind", reg=reg)
    if mode == 3:
        return Operand("postinc", reg=reg)
    if mode == 4:
        return Operand("predec", reg=reg)
    if mode == 5:
        return Operand("disp", reg=reg, value=_signed(reader.word(), 16))
    if mode == 6:
        return _index(reader, "index", reg, 0)
    if reg == 0:
        return Operand("absw", value=_signed(reader.word(), 16) & 0xFFFFFFFF)
    if reg == 1:
        return Operand("absl", value=reader.long())
    if reg == 2:
        base = reader.pos
        return Operand("pcdisp", value=base + _signed(reader.word(), 16))
    if reg == 3:
        return _index(reader, "pcindex", 0, reader.pos)
    if reg == 4:
        if size == 1:
            return Operand("imm", value=reader.word() & 0xFF)
        if size == 2:
            return Operand("imm", value=reader.word())
        if size == 4:
            return Operand("imm", value=reader.long())
        raise DecodeError(reader.start, "immediate operand without a size")
    raise DecodeError(reader.start, "invalid addressing mode")


def _imm(reader: _Reader, size: int) -> Operand:
    return _ea(reader, 7, 4, size)


class _Decoded:
    def __init__(self, mnemonic, opsize=0, operands=(), flow="next", targets=(), cond=None):
        self.mnemonic = mnemonic
        self.opsize = opsize
        self.operands = tuple(operands)
        self.flow = flow
        self.targets = tuple(targets)
        self.cond = cond


def _group0(r: _Reader, op: int) -> _Decoded:
    mode, reg = (op >> 3) & 7, op & 7
    if op & 0x0100:
        if mode == 1:
            size = 4 if op & 0x0040 else 2
            mem = Operand("disp", reg=reg, value=_signed(r.word(), 16))
            data = Operand("dreg", reg=(op >> 9) & 7)
            ops = (data, mem) if op & 0x0080 else (mem, data)
            return _Decoded("movep", size, ops)
        name = _BIT_OPS[(op >> 6) & 3]
        bit = Operand("dreg", reg=(op >> 9) & 7)
        target = _ea(r, mode, reg, 1)
        return _Decoded(name, 4 if target.kind == "dreg" else 1, (bit, target))
    kind = (op >> 9) & 7
    if kind == 4:
        name = _BIT_OPS[(op >> 6) & 3]
        bit = Operand("imm", value=r.word() & 0xFF)
        target = _ea(r, mode, reg, 1)
        return _Decoded(name, 4 if target.kind == "dreg" else 1, (bit, target))
    if kind == 7:
        raise DecodeError(r.start, "illegal group-0 opcode")
    size_bits = (op >> 6) & 3
    if size_bits == 3:
        raise DecodeError(r.start, "illegal size")
    size = _SIZES[size_bits]
    name = _IMM_OPS[kind]
    if (op & 0x3F) == 0x3C:
        if name not in ("ori", "andi", "eori") or size == 4:
            raise DecodeError(r.start, "illegal immediate to status register")
        return _Decoded(name, size, (_imm(r, size), Operand("ccr" if size == 1 else "sr")))
    source = _imm(r, size)
    return _Decoded(name, size, (source, _ea(r, mode, reg, size)))


def _move(r: _Reader, op: int) -> _Decoded:
    size = {1: 1, 3: 2, 2: 4}[op >> 12]
    source = _ea(r, (op >> 3) & 7, op & 7, size)
    dest = _ea(r, (op >> 6) & 7, (op >> 9) & 7, size)
    if dest.kind == "areg":
        if size == 1:
            raise DecodeError(r.start, "movea.b")
        return _Decoded("movea", size, (source, dest))
    if dest.kind in ("imm", "pcdisp", "pcindex"):
        raise DecodeError(r.start, "illegal move destination")
    return _Decoded("move", size, (source, dest))


def _jump_like(r: _Reader, op: int, name: str) -> _Decoded:
    target = _ea(r, (op >> 3) & 7, op & 7, 0)
    if not target.is_memory or target.kind in ("postinc", "predec"):
        raise DecodeError(r.start, f"illegal {name} operand")
    static = target.kind in ("absw", "absl", "pcdisp")
    targets = (target.value & 0xFFFFFF,) if static else ()
    if name == "jsr":
        return _Decoded(name, 0, (target,), "call", targets)
    return _Decoded(name, 0, (target,), "jump" if static else "indirect", targets)


def _group4(r: _Reader, op: int) -> _Decoded:
    mode, reg = (op >> 3) & 7, op & 7
    if op == 0x4AFC:
        raise DecodeError(r.start, "ILLEGAL opcode")
    if (op & 0xFFF0) == 0x4E40:
        return _Decoded("trap", 0, (Operand("imm", value=op & 0xF),))
    if (op & 0xFFF8) == 0x4E50:
        return _Decoded("link", 0, (Operand("areg", reg=reg), Operand("imm", value=r.word())))
    if (op & 0xFFF8) == 0x4E58:
        return _Decoded("unlk", 0, (Operand("areg", reg=reg),))
    if (op & 0xFFF8) == 0x4E60:
        return _Decoded("moveusp", 4, (Operand("areg", reg=reg), Operand("usp")))
    if (op & 0xFFF8) == 0x4E68:
        return _Decoded("moveusp", 4, (Operand("usp"), Operand("areg", reg=reg)))
    simple = {
        0x4E70: ("reset", "next"), 0x4E71: ("nop", "next"), 0x4E73: ("rte", "ret"),
        0x4E75: ("rts", "ret"), 0x4E76: ("trapv", "next"), 0x4E77: ("rtr", "ret"),
    }  # fmt: skip
    if op in simple:
        name, flow = simple[op]
        return _Decoded(name, 0, (), flow)
    if op == 0x4E72:
        return _Decoded("stop", 0, (Operand("imm", value=r.word()),), "stop")
    if (op & 0xFFC0) == 0x4E80:
        return _jump_like(r, op, "jsr")
    if (op & 0xFFC0) == 0x4EC0:
        return _jump_like(r, op, "jmp")
    if (op & 0xFF00) == 0x4E00:
        raise DecodeError(r.start, "illegal 4Exx opcode")
    if (op & 0x01C0) == 0x01C0:
        source = _ea(r, mode, reg, 0)
        if not source.is_memory or source.kind in ("postinc", "predec"):
            raise DecodeError(r.start, "illegal lea operand")
        return _Decoded("lea", 4, (source, Operand("areg", reg=(op >> 9) & 7)))
    if (op & 0x01C0) == 0x0180:
        return _Decoded("chk", 2, (_ea(r, mode, reg, 2), Operand("dreg", reg=(op >> 9) & 7)))
    if op & 0x0100:
        raise DecodeError(r.start, "illegal group-4 opcode")
    size_bits = (op >> 6) & 3
    selector = (op >> 9) & 7
    if selector in (0, 1, 2, 3):
        if size_bits == 3:
            if selector == 0:
                return _Decoded("move", 2, (Operand("sr"), _ea(r, mode, reg, 2)))
            if selector == 2:
                return _Decoded("move", 2, (_ea(r, mode, reg, 2), Operand("ccr")))
            if selector == 3:
                return _Decoded("move", 2, (_ea(r, mode, reg, 2), Operand("sr")))
            raise DecodeError(r.start, "move from ccr is 68010+")
        name = ("negx", "clr", "neg", "not")[selector]
        size = _SIZES[size_bits]
        return _Decoded(name, size, (_ea(r, mode, reg, size),))
    if selector == 4:
        if size_bits == 0:
            return _Decoded("nbcd", 1, (_ea(r, mode, reg, 1),))
        if size_bits == 1:
            if mode == 0:
                return _Decoded("swap", 4, (Operand("dreg", reg=reg),))
            source = _ea(r, mode, reg, 0)
            if not source.is_memory or source.kind in ("postinc", "predec"):
                raise DecodeError(r.start, "illegal pea operand")
            return _Decoded("pea", 4, (source,))
        size = 2 if size_bits == 2 else 4
        if mode == 0:
            return _Decoded("ext", size, (Operand("dreg", reg=reg),))
        mask = r.word()
        if mode == 4:
            mask = int(f"{mask:016b}"[::-1], 2)
        return _Decoded("movem", size, (Operand("reglist", value=mask), _ea(r, mode, reg, size)))
    if selector == 5:
        if size_bits == 3:
            return _Decoded("tas", 1, (_ea(r, mode, reg, 1),))
        size = _SIZES[size_bits]
        return _Decoded("tst", size, (_ea(r, mode, reg, size),))
    if selector == 6 and size_bits >= 2:
        size = 2 if size_bits == 2 else 4
        mask = r.word()
        return _Decoded("movem", size, (_ea(r, mode, reg, size), Operand("reglist", value=mask)))
    raise DecodeError(r.start, "illegal group-4 opcode")


def _group5(r: _Reader, op: int) -> _Decoded:
    mode, reg = (op >> 3) & 7, op & 7
    size_bits = (op >> 6) & 3
    if size_bits == 3:
        cond = (op >> 8) & 0xF
        if mode == 1:
            base = r.pos
            target = (base + _signed(r.word(), 16)) & 0xFFFFFF
            return _Decoded(
                "db" + CONDITIONS[cond], 2, (Operand("dreg", reg=reg),), "cond", (target,), cond
            )
        return _Decoded("s" + CONDITIONS[cond], 1, (_ea(r, mode, reg, 1),), cond=cond)
    size = _SIZES[size_bits]
    count = (op >> 9) & 7 or 8
    name = "subq" if op & 0x0100 else "addq"
    target = _ea(r, mode, reg, size)
    if target.kind == "areg" and size == 1:
        raise DecodeError(r.start, "byte addq/subq to address register")
    return _Decoded(name, size, (Operand("imm", value=count), target))


def _group6(r: _Reader, op: int) -> _Decoded:
    cond = (op >> 8) & 0xF
    base = r.pos
    disp = op & 0xFF
    if disp == 0:
        disp = _signed(r.word(), 16)
        size = 2
    elif disp == 0xFF:
        raise DecodeError(r.start, "68020 long branch")
    else:
        disp = _signed(disp, 8)
        size = 1
    target = (base + disp) & 0xFFFFFF
    if cond == 0:
        return _Decoded("bra", size, (), "jump", (target,))
    if cond == 1:
        return _Decoded("bsr", size, (), "call", (target,))
    return _Decoded("b" + CONDITIONS[cond], size, (), "cond", (target,), cond)


def _arith(r: _Reader, op: int, name: str) -> _Decoded:
    """Shared decoding for OR/AND/ADD/SUB and their A/X variants."""
    mode, reg = (op >> 3) & 7, op & 7
    data = Operand("dreg", reg=(op >> 9) & 7)
    opmode = (op >> 6) & 7
    if opmode in (3, 7):
        if name in ("or", "and"):
            raise AssertionError("handled by caller")
        size = 2 if opmode == 3 else 4
        return _Decoded(
            name + "a", size, (_ea(r, mode, reg, size), Operand("areg", reg=(op >> 9) & 7))
        )
    size = _SIZES[opmode & 3]
    if opmode & 4:
        if mode in (0, 1):
            if name in ("or", "and"):
                raise DecodeError(r.start, f"illegal {name} destination")
            kind = "predec" if mode == 1 else "dreg"
            return _Decoded(
                name + "x", size, (Operand(kind, reg=reg), Operand(kind, reg=(op >> 9) & 7))
            )
        return _Decoded(name, size, (data, _ea(r, mode, reg, size)))
    return _Decoded(name, size, (_ea(r, mode, reg, size), data))


def _group8(r: _Reader, op: int) -> _Decoded:
    mode, reg = (op >> 3) & 7, op & 7
    opmode = (op >> 6) & 7
    data = Operand("dreg", reg=(op >> 9) & 7)
    if opmode == 3:
        return _Decoded("divu", 2, (_ea(r, mode, reg, 2), data))
    if opmode == 7:
        return _Decoded("divs", 2, (_ea(r, mode, reg, 2), data))
    if opmode == 4 and mode in (0, 1):
        kind = "predec" if mode == 1 else "dreg"
        return _Decoded("sbcd", 1, (Operand(kind, reg=reg), Operand(kind, reg=(op >> 9) & 7)))
    return _arith(r, op, "or")


def _groupb(r: _Reader, op: int) -> _Decoded:
    mode, reg = (op >> 3) & 7, op & 7
    opmode = (op >> 6) & 7
    high = (op >> 9) & 7
    if opmode in (3, 7):
        size = 2 if opmode == 3 else 4
        return _Decoded("cmpa", size, (_ea(r, mode, reg, size), Operand("areg", reg=high)))
    size = _SIZES[opmode & 3]
    if not opmode & 4:
        return _Decoded("cmp", size, (_ea(r, mode, reg, size), Operand("dreg", reg=high)))
    if mode == 1:
        return _Decoded("cmpm", size, (Operand("postinc", reg=reg), Operand("postinc", reg=high)))
    return _Decoded("eor", size, (Operand("dreg", reg=high), _ea(r, mode, reg, size)))


def _groupc(r: _Reader, op: int) -> _Decoded:
    mode, reg = (op >> 3) & 7, op & 7
    opmode = (op >> 6) & 7
    high = (op >> 9) & 7
    data = Operand("dreg", reg=high)
    if opmode == 3:
        return _Decoded("mulu", 2, (_ea(r, mode, reg, 2), data))
    if opmode == 7:
        return _Decoded("muls", 2, (_ea(r, mode, reg, 2), data))
    if opmode == 4 and mode in (0, 1):
        kind = "predec" if mode == 1 else "dreg"
        return _Decoded("abcd", 1, (Operand(kind, reg=reg), Operand(kind, reg=high)))
    if opmode == 5 and mode == 0:
        return _Decoded("exg", 4, (Operand("dreg", reg=high), Operand("dreg", reg=reg)))
    if opmode == 5 and mode == 1:
        return _Decoded("exg", 4, (Operand("areg", reg=high), Operand("areg", reg=reg)))
    if opmode == 6 and mode == 1:
        return _Decoded("exg", 4, (Operand("dreg", reg=high), Operand("areg", reg=reg)))
    return _arith(r, op, "and")


def _groupe(r: _Reader, op: int) -> _Decoded:
    direction = "l" if op & 0x0100 else "r"
    size_bits = (op >> 6) & 3
    if size_bits == 3:
        if op & 0x0800:
            raise DecodeError(r.start, "68020 bit field")
        name = _SHIFTS[(op >> 9) & 3] + direction
        return _Decoded(name, 2, (_ea(r, (op >> 3) & 7, op & 7, 2),))
    name = _SHIFTS[(op >> 3) & 3] + direction
    count_field = (op >> 9) & 7
    by_register = bool(op & 0x0020)
    count = (
        Operand("dreg", reg=count_field) if by_register else Operand("imm", value=count_field or 8)
    )
    return _Decoded(name, _SIZES[size_bits], (count, Operand("dreg", reg=op & 7)))


_ALL = frozenset(
    ("dreg", "areg", "ind", "postinc", "predec", "disp", "index", "absw", "absl",
     "pcdisp", "pcindex", "imm")
)  # fmt: skip
_DATA = _ALL - {"areg"}
_MEMORY_ALTERABLE = frozenset(("ind", "postinc", "predec", "disp", "index", "absw", "absl"))
_DATA_ALTERABLE = _MEMORY_ALTERABLE | {"dreg"}
_ALTERABLE = _DATA_ALTERABLE | {"areg"}
_CONTROL = frozenset(("ind", "disp", "index", "absw", "absl", "pcdisp", "pcindex"))
_SHIFT_MNEMONICS = frozenset(s + d for s in _SHIFTS for d in "lr")


def _validate(pc: int, d: _Decoded) -> None:
    """Reject addressing modes the 68000 does not allow for this instruction.

    Without this, discovery that strays into data keeps decoding plausible
    looking but impossible instructions instead of stopping.
    """
    name, ops, size = d.mnemonic, d.operands, d.opsize

    def need(index: int, allowed: frozenset[str]) -> None:
        kind = ops[index].kind
        if kind not in allowed:
            raise DecodeError(pc, f"{name} does not allow a {kind} operand")
        if kind == "areg" and size == 1:
            raise DecodeError(pc, f"byte {name} on an address register")

    if name in ("ori", "andi", "eori", "subi", "addi", "cmpi"):
        if ops[1].kind not in ("ccr", "sr"):
            need(1, _DATA_ALTERABLE)
    elif name == "btst":
        need(1, _DATA if ops[0].kind == "dreg" else _DATA - {"imm"})
    elif name in ("bchg", "bclr", "bset"):
        need(1, _DATA_ALTERABLE)
    elif name == "move":
        if ops[0].kind == "sr":
            need(1, _DATA_ALTERABLE)
        elif ops[1].kind in ("ccr", "sr"):
            need(0, _DATA)
        else:
            need(0, _ALL)
            need(1, _DATA_ALTERABLE)
    elif name in ("movea", "adda", "suba", "cmpa"):
        need(0, _ALL)
    elif name in ("negx", "clr", "neg", "not", "tst", "nbcd", "tas") or (
        d.cond is not None and name.startswith("s")
    ):
        need(0, _DATA_ALTERABLE)
    elif name == "movem":
        if ops[0].kind == "reglist":
            need(1, _CONTROL - {"pcdisp", "pcindex"} | {"predec"})
        else:
            need(0, _CONTROL | {"postinc"})
    elif name in ("chk", "divu", "divs", "mulu", "muls"):
        need(0, _DATA)
    elif name in ("addq", "subq"):
        need(1, _ALTERABLE)
    elif name in ("or", "and"):
        if ops[1].kind == "dreg":
            need(0, _DATA)
        else:
            need(1, _MEMORY_ALTERABLE)
    elif name in ("add", "sub"):
        if ops[1].kind == "dreg":
            need(0, _ALL)
        else:
            need(1, _MEMORY_ALTERABLE)
    elif name == "cmp":
        need(0, _ALL)
    elif name == "eor":
        need(1, _DATA_ALTERABLE)
    elif name in _SHIFT_MNEMONICS and len(ops) == 1:
        need(0, _MEMORY_ALTERABLE)


def decode(rom: bytes, pc: int) -> Insn:
    """Decode the instruction at ``pc``."""
    if pc & 1:
        raise DecodeError(pc, "odd address")
    r = _Reader(rom, pc)
    op = r.word()
    group = op >> 12
    if group == 0:
        d = _group0(r, op)
    elif group in (1, 2, 3):
        d = _move(r, op)
    elif group == 4:
        d = _group4(r, op)
    elif group == 5:
        d = _group5(r, op)
    elif group == 6:
        d = _group6(r, op)
    elif group == 7:
        if op & 0x0100:
            raise DecodeError(pc, "illegal moveq")
        value = _signed(op & 0xFF, 8) & 0xFFFFFFFF
        d = _Decoded("moveq", 4, (Operand("imm", value=value), Operand("dreg", reg=(op >> 9) & 7)))
    elif group == 8:
        d = _group8(r, op)
    elif group == 9:
        d = _arith(r, op, "sub")
    elif group == 0xB:
        d = _groupb(r, op)
    elif group == 0xC:
        d = _groupc(r, op)
    elif group == 0xD:
        d = _arith(r, op, "add")
    elif group == 0xE:
        d = _groupe(r, op)
    else:
        raise DecodeError(pc, f"line-{group:X} opcode")
    _validate(pc, d)
    return Insn(
        pc=pc,
        size=r.pos - pc,
        mnemonic=d.mnemonic,
        opsize=d.opsize,
        operands=d.operands,
        flow=d.flow,
        targets=d.targets,
        cond=d.cond,
    )
