"""Emit C for decoded 68000 instructions.

Each instruction becomes ``static void i_<pc>(RrMachine *m)``. The function
performs the instruction exactly (result, condition codes, register side
effects, bus accesses in the 68000's order), sets the next PC and charges
cycles. Helpers live in ``port/core/rr_ops.h``.
"""

from __future__ import annotations

from collections.abc import Mapping

from rr2port.cycles import ea_time, static_cycles
from rr2port.decode import Insn, Operand

_BITS = {1: 8, 2: 16, 4: 32}
_CAST = {1: "(uint8_t)", 2: "(uint16_t)", 4: ""}
_SHIFT_KIND = {
    "asl": "RR_ASL", "asr": "RR_ASR", "lsl": "RR_LSL", "lsr": "RR_LSR",
    "roxl": "RR_ROXL", "roxr": "RR_ROXR", "rol": "RR_ROL", "ror": "RR_ROR",
}  # fmt: skip
_BINARY = {"add": "rr_add", "sub": "rr_sub", "addi": "rr_add", "subi": "rr_sub",
           "addq": "rr_add", "subq": "rr_sub"}  # fmt: skip
_LOGIC = {"and": "&", "or": "|", "eor": "^", "andi": "&", "ori": "|", "eori": "^"}


class Unimplemented(Exception):
    """The instruction decodes but has no emitter yet."""


def _u(value: int) -> str:
    return f"0x{value & 0xFFFFFFFF:X}u"


class _Body:
    """Accumulates the C statements for one instruction."""

    def __init__(self, insn: Insn) -> None:
        self.insn = insn
        self.lines: list[str] = []
        self.addresses: dict[int, str] = {}
        self.cycles_done = False
        self.pc_done = False

    def add(self, line: str) -> None:
        self.lines.append(line)

    # --- operands -------------------------------------------------------

    @staticmethod
    def _step(op: Operand, size: int) -> int:
        return 2 if size == 1 and op.reg == 7 else size

    @staticmethod
    def _index_value(op: Operand) -> str:
        reg = f"c->a[{op.xreg - 8}]" if op.xreg >= 8 else f"c->d[{op.xreg}]"
        return reg if op.xlong else f"(uint32_t)(int16_t){reg}"

    def address(self, k: int, size: int) -> str:
        """Compute operand k's address once, applying (An)+ / -(An) side effects."""
        if k in self.addresses:
            return self.addresses[k]
        op = self.insn.operands[k]
        name = f"ea{k}"
        if op.kind == "ind":
            expr = f"c->a[{op.reg}]"
        elif op.kind == "postinc":
            self.add(f"uint32_t {name} = c->a[{op.reg}];")
            self.add(f"c->a[{op.reg}] += {self._step(op, size)};")
            self.addresses[k] = name
            return name
        elif op.kind == "predec":
            self.add(f"c->a[{op.reg}] -= {self._step(op, size)};")
            expr = f"c->a[{op.reg}]"
        elif op.kind == "disp":
            expr = f"c->a[{op.reg}] + {_u(op.value)}"
        elif op.kind == "index":
            expr = f"c->a[{op.reg}] + {_u(op.value)} + {self._index_value(op)}"
        elif op.kind in ("absw", "absl", "pcdisp"):
            expr = _u(op.value)
        elif op.kind == "pcindex":
            expr = f"{_u(op.value)} + {self._index_value(op)}"
        else:
            raise Unimplemented(f"operand {op.kind} has no address")
        self.add(f"uint32_t {name} = {expr};")
        self.addresses[k] = name
        return name

    def read(self, k: int, size: int) -> str:
        """Return an expression for operand k's value, zero-extended to 32 bits."""
        op = self.insn.operands[k]
        if op.kind == "dreg":
            return f"({_CAST[size]}c->d[{op.reg}])"
        if op.kind == "areg":
            return f"({_CAST[size]}c->a[{op.reg}])"
        if op.kind == "imm":
            return _u(op.value)
        addr = self.address(k, size)
        name = f"v{k}"
        self.add(f"uint32_t {name} = rr_read{_BITS[size]}(m, {addr});")
        return name

    def write(self, k: int, size: int, expr: str) -> None:
        op = self.insn.operands[k]
        if op.kind == "dreg":
            if size == 4:
                self.add(f"c->d[{op.reg}] = {expr};")
            else:
                self.add(f"rr_setd(c, {op.reg}, {size}, {expr});")
        elif op.kind == "areg":
            self.add(f"c->a[{op.reg}] = {expr};")
        else:
            addr = self.address(k, size)
            self.add(f"rr_write{_BITS[size]}(m, {addr}, {_CAST[size]}({expr}));")

    def sign_extend(self, expr: str, size: int) -> str:
        if size == 2:
            return f"(uint32_t)(int16_t)({expr})"
        if size == 1:
            return f"(uint32_t)(int8_t)({expr})"
        return expr

    # --- bookkeeping ----------------------------------------------------

    def cycles(self, expr: str | int) -> None:
        self.add(f"c->cycles += {expr};")
        self.cycles_done = True

    def goto(self, expr: str) -> None:
        self.add(f"c->pc = {expr};")
        self.pc_done = True

    @property
    def next_pc(self) -> str:
        return _u(self.insn.pc + self.insn.size)

    def finish(self) -> list[str]:
        if not self.pc_done:
            self.goto(self.next_pc)
        if not self.cycles_done:
            cost = static_cycles(self.insn)
            if cost is None:
                raise Unimplemented(f"no cycle cost for {self.insn.mnemonic}")
            self.cycles(cost)
        return self.lines


def _emit_move(b: _Body) -> None:
    insn = b.insn
    src, dst = insn.operands
    size = insn.opsize
    if src.kind == "sr":
        if dst.is_memory:
            b.add(f"(void)rr_read16(m, {b.address(1, 2)});")
        b.write(1, 2, "c->sr")
        return
    value = b.read(0, size)
    if dst.kind == "ccr":
        b.add(f"rr_set_ccr(c, {value});")
        return
    if dst.kind == "sr":
        b.add(f"rr_set_sr(m, {value});")
        return
    b.add(f"uint32_t r = {value};")
    b.add(f"rr_nz(c, r, {size});")
    b.write(1, size, "r")


def _emit_binary(b: _Body, helper: str) -> None:
    size = b.insn.opsize
    s = b.read(0, size)
    d = b.read(1, size)
    b.add(f"uint32_t r = {helper}(c, {s}, {d}, {size});")
    b.write(1, size, "r")


def _emit_logic(b: _Body, operator: str) -> None:
    insn = b.insn
    size = insn.opsize
    dst = insn.operands[1]
    if dst.kind == "ccr":
        b.add(f"rr_set_ccr(c, (c->sr & 0xFF) {operator} {_u(insn.operands[0].value)});")
        return
    if dst.kind == "sr":
        b.add(f"rr_set_sr(m, c->sr {operator} {_u(insn.operands[0].value)});")
        return
    s = b.read(0, size)
    d = b.read(1, size)
    b.add(f"uint32_t r = {s} {operator} {d};")
    b.add(f"rr_nz(c, r, {size});")
    b.write(1, size, "r")


def _emit_quick(b: _Body) -> None:
    insn = b.insn
    dst = insn.operands[1]
    if dst.kind == "areg":
        operator = "+=" if insn.mnemonic == "addq" else "-="
        b.add(f"c->a[{dst.reg}] {operator} {insn.operands[0].value};")
        return
    _emit_binary(b, _BINARY[insn.mnemonic])


def _emit_address_arith(b: _Body) -> None:
    insn = b.insn
    size = insn.opsize
    value = b.sign_extend(b.read(0, size), size)
    reg = insn.operands[1].reg
    if insn.mnemonic == "adda":
        b.add(f"c->a[{reg}] += {value};")
    elif insn.mnemonic == "suba":
        b.add(f"c->a[{reg}] -= {value};")
    elif insn.mnemonic == "cmpa":
        b.add(f"rr_cmp(c, {value}, c->a[{reg}], 4);")
    else:
        b.add(f"c->a[{reg}] = {value};")


def _emit_unary(b: _Body) -> None:
    insn = b.insn
    size = insn.opsize
    name = insn.mnemonic
    if name == "clr":
        if insn.operands[0].is_memory:
            b.add(f"(void)rr_read{_BITS[size]}(m, {b.address(0, size)});")
        b.write(0, size, "0")
        b.add("c->sr = (uint16_t)((c->sr & ~0xF) | RR_Z);")
        return
    value = b.read(0, size)
    if name == "tst":
        b.add(f"rr_nz(c, {value}, {size});")
        return
    if name == "not":
        b.add(f"uint32_t r = ~{value} & rr_mask({size});")
        b.add(f"rr_nz(c, r, {size});")
    elif name == "neg":
        b.add(f"uint32_t r = rr_sub(c, {value}, 0, {size});")
    else:
        b.add(f"uint32_t r = rr_subx(c, {value}, 0, {size});")
    b.write(0, size, "r")


def _emit_bit(b: _Body) -> None:
    insn = b.insn
    bit_op, target = insn.operands
    name = insn.mnemonic
    register = target.kind == "dreg"
    modulo = 31 if register else 7
    if bit_op.kind == "imm":
        b.add(f"unsigned bit = {bit_op.value & modulo};")
    else:
        b.add(f"unsigned bit = c->d[{bit_op.reg}] & {modulo};")
    if register:
        b.add(f"uint32_t v = c->d[{target.reg}];")
    else:
        b.add(f"uint32_t v = {b.read(1, 1)};")
    b.add("c->sr = (uint16_t)((c->sr & ~RR_Z) | ((v >> bit) & 1 ? 0 : RR_Z));")
    if name == "btst":
        return
    operator = {"bchg": "^=", "bclr": "&= ~", "bset": "|="}[name]
    b.add(f"v {operator}(1u << bit);")
    if register:
        b.add(f"c->d[{target.reg}] = v;")
        if static_cycles(insn) is None:
            base = 8 if name == "bclr" else 6
            b.cycles(f"{base} + (bit > 15 ? 2 : 0)")
    else:
        b.write(1, 1, "v")


def _emit_shift(b: _Body) -> None:
    insn = b.insn
    kind = _SHIFT_KIND[insn.mnemonic]
    size = insn.opsize
    if len(insn.operands) == 1:
        value = b.read(0, 2)
        b.add(f"uint32_t r = rr_shift(c, {kind}, {value}, 1, 2);")
        b.write(0, 2, "r")
        return
    count, target = insn.operands
    if count.kind == "imm":
        b.add(f"unsigned count = {count.value};")
    else:
        b.add(f"unsigned count = c->d[{count.reg}] & 63;")
    b.add(f"uint32_t r = rr_shift(c, {kind}, c->d[{target.reg}], count, {size});")
    b.write(1, size, "r")
    b.cycles(f"{8 if size == 4 else 6} + 2 * count")


def _emit_movem(b: _Body) -> None:
    insn = b.insn
    size = insn.opsize
    first, second = insn.operands
    bits = _BITS[size]

    def register(index: int) -> str:
        return f"c->a[{index - 8}]" if index >= 8 else f"c->d[{index}]"

    if first.kind == "reglist":
        mask = first.value
        if second.kind == "predec":
            b.add(f"uint32_t addr = c->a[{second.reg}];")
            for index in range(15, -1, -1):
                if mask >> index & 1:
                    b.add(f"addr -= {size};")
                    b.add(f"rr_write{bits}(m, addr, {_CAST[size]}{register(index)});")
            b.add(f"c->a[{second.reg}] = addr;")
            return
        b.add(f"uint32_t addr = {b.address(1, size)};")
        for index in range(16):
            if mask >> index & 1:
                b.add(f"rr_write{bits}(m, addr, {_CAST[size]}{register(index)});")
                b.add(f"addr += {size};")
        return
    mask = second.value
    if first.kind == "postinc":
        b.add(f"uint32_t addr = c->a[{first.reg}];")
    else:
        b.add(f"uint32_t addr = {b.address(0, size)};")
    for index in range(16):
        if mask >> index & 1:
            value = b.sign_extend(f"rr_read{bits}(m, addr)", size)
            b.add(f"{register(index)} = {value};")
            b.add(f"addr += {size};")
    if first.kind == "postinc":
        b.add(f"c->a[{first.reg}] = addr;")


def _emit_flow(b: _Body) -> None:
    insn = b.insn
    name = insn.mnemonic
    if name == "bra":
        b.goto(_u(insn.targets[0]))
    elif name == "bsr":
        b.add(f"rr_push32(m, {b.next_pc});")
        b.goto(_u(insn.targets[0]))
    elif name in ("jmp", "jsr"):
        target = b.address(0, 0)
        if name == "jsr":
            b.add(f"rr_push32(m, {b.next_pc});")
        b.goto(target)
    elif name == "rts":
        b.goto("rr_pop32(m)")
    elif name == "rte":
        b.add("uint16_t sr = rr_pop16(m);")
        b.add("uint32_t pc = rr_pop32(m);")
        b.add("rr_set_sr(m, sr);")
        b.goto("pc")
    elif name.startswith("db"):
        reg = insn.operands[0].reg
        b.add(f"if (rr_cond(c, {insn.cond})) {{")
        b.add(f"    c->pc = {b.next_pc}; c->cycles += 12;")
        b.add("} else {")
        b.add(f"    uint16_t n = (uint16_t)(c->d[{reg}] - 1);")
        b.add(f"    rr_setd(c, {reg}, 2, n);")
        b.add(f"    if (n != 0xFFFF) {{ c->pc = {_u(insn.targets[0])}; c->cycles += 10; }}")
        b.add(f"    else {{ c->pc = {b.next_pc}; c->cycles += 14; }}")
        b.add("}")
        b.pc_done = b.cycles_done = True
    else:  # bcc
        not_taken = 8 if insn.opsize == 1 else 12
        b.add(f"if (rr_cond(c, {insn.cond})) {{ c->pc = {_u(insn.targets[0])}; c->cycles += 10; }}")
        b.add(f"else {{ c->pc = {b.next_pc}; c->cycles += {not_taken}; }}")
        b.pc_done = b.cycles_done = True


def _emit_scc(b: _Body) -> None:
    insn = b.insn
    target = insn.operands[0]
    b.add(f"int taken = rr_cond(c, {insn.cond});")
    if target.is_memory:
        b.add(f"(void)rr_read8(m, {b.address(0, 1)});")
    b.write(0, 1, "taken ? 0xFFu : 0u")
    if target.kind == "dreg":
        b.cycles("taken ? 6 : 4")


def _emit_muldiv(b: _Body) -> None:
    insn = b.insn
    value = b.read(0, 2)
    reg = insn.operands[1].reg
    name = insn.mnemonic
    if name in ("mulu", "muls"):
        b.add(f"rr_{name}(c, {reg}, {value});")
    else:
        b.add(f"rr_{name}(m, {reg}, {value});")
    b.cycles(ea_time(insn.operands[0], 2))


def _emit_misc(b: _Body) -> None:
    insn = b.insn
    name = insn.mnemonic
    ops = insn.operands
    if name == "nop":
        return
    if name == "moveq":
        b.add(f"c->d[{ops[1].reg}] = {_u(ops[0].value)};")
        b.add(f"rr_nz(c, c->d[{ops[1].reg}], 4);")
    elif name == "lea":
        b.add(f"c->a[{ops[1].reg}] = {b.address(0, 0)};")
    elif name == "pea":
        b.add(f"rr_push32(m, {b.address(0, 0)});")
    elif name == "swap":
        reg = ops[0].reg
        b.add(f"c->d[{reg}] = c->d[{reg}] << 16 | c->d[{reg}] >> 16;")
        b.add(f"rr_nz(c, c->d[{reg}], 4);")
    elif name == "ext":
        reg = ops[0].reg
        if insn.opsize == 2:
            b.add(f"rr_setd(c, {reg}, 2, (uint32_t)(int8_t)c->d[{reg}]);")
        else:
            b.add(f"c->d[{reg}] = (uint32_t)(int16_t)c->d[{reg}];")
        b.add(f"rr_nz(c, c->d[{reg}], {insn.opsize});")
    elif name == "exg":
        left = f"c->{'a' if ops[0].kind == 'areg' else 'd'}[{ops[0].reg}]"
        right = f"c->{'a' if ops[1].kind == 'areg' else 'd'}[{ops[1].reg}]"
        b.add(f"uint32_t t = {left};")
        b.add(f"{left} = {right};")
        b.add(f"{right} = t;")
    elif name == "link":
        reg = ops[0].reg
        b.add(f"rr_push32(m, c->a[{reg}]);")
        b.add(f"c->a[{reg}] = c->a[7];")
        b.add(f"c->a[7] += (uint32_t)(int16_t){_u(ops[1].value)};")
    elif name == "unlk":
        reg = ops[0].reg
        b.add(f"c->a[7] = c->a[{reg}];")
        b.add(f"c->a[{reg}] = rr_pop32(m);")
    elif name == "moveusp":
        if ops[0].kind == "usp":
            b.add(f"c->a[{ops[1].reg}] = c->usp;")
        else:
            b.add(f"c->usp = c->a[{ops[0].reg}];")
    elif name in ("cmp", "cmpi", "cmpm"):
        size = insn.opsize
        s = b.read(0, size)
        d = b.read(1, size)
        b.add(f"rr_cmp(c, {s}, {d}, {size});")
    elif name == "abcd":
        s = b.read(0, 1)
        d = b.read(1, 1)
        b.add(f"uint32_t r = rr_abcd(c, {s}, {d});")
        b.write(1, 1, "r")
    elif name in ("addx", "subx"):
        size = insn.opsize
        s = b.read(0, size)
        d = b.read(1, size)
        b.add(f"uint32_t r = rr_{name}(c, {s}, {d}, {size});")
        b.write(1, size, "r")
    else:
        raise Unimplemented(name)


def _emit_body(insn: Insn) -> list[str]:
    b = _Body(insn)
    name = insn.mnemonic
    if name == "move":
        _emit_move(b)
    elif name in ("movea", "adda", "suba", "cmpa"):
        _emit_address_arith(b)
    elif name in ("addq", "subq"):
        _emit_quick(b)
    elif name in _BINARY:
        _emit_binary(b, _BINARY[name])
    elif name in _LOGIC:
        _emit_logic(b, _LOGIC[name])
    elif name in ("clr", "tst", "not", "neg", "negx"):
        _emit_unary(b)
    elif name in ("btst", "bchg", "bclr", "bset"):
        _emit_bit(b)
    elif name in _SHIFT_KIND:
        _emit_shift(b)
    elif name == "movem":
        _emit_movem(b)
    elif name in ("mulu", "muls", "divu", "divs"):
        _emit_muldiv(b)
    elif name in ("bra", "bsr", "jmp", "jsr", "rts", "rte") or (
        insn.cond is not None and name[0] in "bd"
    ):
        _emit_flow(b)
    elif insn.cond is not None:
        _emit_scc(b)
    else:
        _emit_misc(b)
    return b.finish()


def emit_insn(insn: Insn) -> str:
    """Return the C function for one instruction."""
    header = f"static void i_{insn.pc:06X}(RrMachine *m) {{"
    try:
        body = ["RrCpu *c = &m->cpu;", *_emit_body(insn)]
    except Unimplemented:
        body = [f'rr_fatal(m, "unimplemented instruction {insn.mnemonic} at {insn.pc:06X}");']
    return "\n".join([header, *(f"    {line}" for line in body), "}"])


def emit_program(insns: Mapping[int, Insn], rom_crc32: int) -> str:
    """Return a complete C source file for a set of instructions."""
    ordered = sorted(insns)
    limit = (ordered[-1] + 2) if ordered else 0
    parts = [
        "/* Generated by rr2-translate from the user's ROM. Do not edit or commit. */",
        '#include "rr_ops.h"',
        "",
        f"const uint32_t rr_code_rom_crc32 = 0x{rom_crc32:08X}u;",
        "",
    ]
    parts += [emit_insn(insns[pc]) for pc in ordered]
    parts += ["", f"static const RrInsnFn rr_table[{limit // 2 + 1}] = {{"]
    parts += [f"    [0x{pc >> 1:X}] = i_{pc:06X}," for pc in ordered]
    parts += [
        "};",
        "",
        "RrInsnFn rr_lookup(uint32_t pc) {",
        f"    if ((pc & 1) || pc >= 0x{limit:X}u) return 0;",
        "    return rr_table[pc >> 1];",
        "}",
        "",
    ]
    return "\n".join(parts)
