"""MC68000 instruction timing (standard execution-time tables).

``static_cycles`` returns the full cost of an instruction when it does not
depend on data, or ``None`` when it does. Data-dependent costs (branches,
DBcc, Scc on a register, register shifts, bit operations on a register,
multiply and divide) are charged by the emitted code or its helpers, using
``ea_time`` for the operand-fetch part.
"""

from __future__ import annotations

from rr2port.decode import Insn, Operand

# (byte/word, long) effective-address calculation times.
_EA = {
    "dreg": (0, 0), "areg": (0, 0), "ind": (4, 8), "postinc": (4, 8), "predec": (6, 10),
    "disp": (8, 12), "index": (10, 14), "absw": (8, 12), "absl": (12, 16),
    "pcdisp": (8, 12), "pcindex": (10, 14), "imm": (4, 8),
}  # fmt: skip
_MOVE_DEST = {
    "dreg": (0, 0), "ind": (4, 8), "postinc": (4, 8), "predec": (4, 8),
    "disp": (8, 12), "index": (10, 14), "absw": (8, 12), "absl": (12, 16),
}  # fmt: skip
_JMP = {"ind": 8, "disp": 10, "index": 14, "absw": 10, "absl": 12, "pcdisp": 10, "pcindex": 14}
_JSR = {"ind": 16, "disp": 18, "index": 22, "absw": 18, "absl": 20, "pcdisp": 18, "pcindex": 22}
_LEA = {"ind": 4, "disp": 8, "index": 12, "absw": 8, "absl": 12, "pcdisp": 8, "pcindex": 12}
_PEA = {"ind": 12, "disp": 16, "index": 20, "absw": 16, "absl": 20, "pcdisp": 16, "pcindex": 20}
_MOVEM_TO_MEM = {"ind": 8, "predec": 8, "disp": 12, "index": 14, "absw": 12, "absl": 16}
_MOVEM_TO_REG = {
    "ind": 12, "postinc": 12, "disp": 16, "index": 18, "absw": 16, "absl": 20,
    "pcdisp": 16, "pcindex": 18,
}  # fmt: skip
_FIXED = {
    "moveq": 4, "ext": 4, "swap": 4, "exg": 6, "nop": 4, "rts": 16, "rte": 20, "rtr": 20,
    "bsr": 18, "bra": 10, "link": 16, "unlk": 12, "moveusp": 4,
}  # fmt: skip
_SHIFT_NAMES = ("asl", "asr", "lsl", "lsr", "roxl", "roxr", "rol", "ror")


def ea_time(op: Operand, size: int) -> int:
    """Cycles to compute an effective address and fetch the operand."""
    if op.kind not in _EA:
        return 0
    return _EA[op.kind][1 if size == 4 else 0]


def _is_fast_source(op: Operand) -> bool:
    return op.kind in ("dreg", "areg", "imm")


def static_cycles(insn: Insn) -> int | None:
    name, size, ops = insn.mnemonic, insn.opsize, insn.operands
    long = size == 4
    if name in _FIXED:
        return _FIXED[name]
    if name == "move":
        src, dst = ops
        if src.kind == "sr":
            return 6 if dst.kind == "dreg" else 8 + ea_time(dst, 2)
        if dst.kind in ("sr", "ccr"):
            return 12 + ea_time(src, 2)
        return 4 + ea_time(src, size) + _MOVE_DEST[dst.kind][1 if long else 0]
    if name == "movea":
        return 4 + ea_time(ops[0], size)
    if name == "lea":
        return _LEA[ops[0].kind]
    if name == "pea":
        return _PEA[ops[0].kind]
    if name == "jmp":
        return _JMP[ops[0].kind]
    if name == "jsr":
        return _JSR[ops[0].kind]
    if name in ("clr", "neg", "not", "negx"):
        if ops[0].kind == "dreg":
            return 6 if long else 4
        return (12 if long else 8) + ea_time(ops[0], size)
    if name == "tst":
        return 4 + ea_time(ops[0], size)
    if name in ("add", "sub", "and", "or"):
        src, dst = ops
        if dst.kind == "dreg":
            if long:
                return (8 if _is_fast_source(src) else 6) + ea_time(src, size)
            return 4 + ea_time(src, size)
        return (12 if long else 8) + ea_time(dst, size)
    if name == "eor":
        if ops[1].kind == "dreg":
            return 8 if long else 4
        return (12 if long else 8) + ea_time(ops[1], size)
    if name in ("adda", "suba"):
        if long:
            return (8 if _is_fast_source(ops[0]) else 6) + ea_time(ops[0], size)
        return 8 + ea_time(ops[0], size)
    if name == "cmp":
        return (6 if long else 4) + ea_time(ops[0], size)
    if name == "cmpa":
        return 6 + ea_time(ops[0], size)
    if name == "cmpm":
        return 20 if long else 12
    if name in ("addi", "subi", "andi", "ori", "eori"):
        dst = ops[1]
        if dst.kind in ("ccr", "sr"):
            return 20
        if dst.kind == "dreg":
            return 16 if long else 8
        return (20 if long else 12) + ea_time(dst, size)
    if name == "cmpi":
        dst = ops[1]
        if dst.kind == "dreg":
            return 14 if long else 8
        return (12 if long else 8) + ea_time(dst, size)
    if name in ("addq", "subq"):
        dst = ops[1]
        if dst.kind == "areg":
            return 8
        if dst.kind == "dreg":
            return 8 if long else 4
        return (12 if long else 8) + ea_time(dst, size)
    if name == "abcd":
        return 6 if ops[0].kind == "dreg" else 18
    if name in ("addx", "subx"):
        if ops[0].kind == "dreg":
            return 8 if long else 4
        return 30 if long else 18
    if name in ("btst", "bchg", "bclr", "bset"):
        bit, target = ops
        static_bit = bit.kind == "imm"
        if target.kind == "dreg":
            if name == "btst":
                return 10 if static_bit else 6
            if not static_bit:
                return None
            high = (bit.value & 31) > 15
            base = 12 if name == "bclr" else 10
            return base + (2 if high else 0)
        if name == "btst":
            return (8 if static_bit else 4) + ea_time(target, 1)
        return (12 if static_bit else 8) + ea_time(target, 1)
    if name == "movem":
        first, second = ops
        if first.kind == "reglist":
            count = bin(first.value).count("1")
            return _MOVEM_TO_MEM[second.kind] + count * (8 if long else 4)
        count = bin(second.value).count("1")
        return _MOVEM_TO_REG[first.kind] + count * (8 if long else 4)
    if name in _SHIFT_NAMES and len(ops) == 1:
        return 8 + ea_time(ops[0], 2)
    if name.startswith("s") and insn.cond is not None and ops and ops[0].kind != "dreg":
        return 8 + ea_time(ops[0], 1)
    return None
