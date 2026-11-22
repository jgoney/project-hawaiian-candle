from __future__ import annotations

import pytest

from rr2port.decode import DecodeError, Operand, decode


def d(hex_bytes: str, pc: int = 0):
    return decode(bytes(pc) + bytes.fromhex(hex_bytes), pc)


def test_decode_add_word_register():
    i = d("D041")
    assert (i.mnemonic, i.opsize, i.size, i.flow) == ("add", 2, 2, "next")
    assert i.operands == (Operand("dreg", reg=1), Operand("dreg", reg=0))


def test_decode_jsr_absolute_long_is_call_with_target():
    i = d("4EB9000CBCC8")
    assert (i.flow, i.targets, i.size) == ("call", (0xCBCC8,), 6)


def test_decode_dbf_targets_itself():
    i = d("51C8FFFE")
    assert (i.mnemonic, i.flow, i.targets) == ("dbf", "cond", (0,))


def test_decode_rts_and_jmp_indirect():
    assert d("4E75").flow == "ret"
    assert d("4ED0").flow == "indirect"  # jmp (a0)


def test_decode_branch_targets_are_relative_to_extension_word():
    assert d("6104", pc=0x1000).targets == (0x1006,)  # bsr.s
    assert d("6E12", pc=0x1000).targets == (0x1014,)  # bgt.s
    assert d("6000FFFC", pc=0x1000).targets == (0x0FFE,)  # bra.w
    assert d("6E12", pc=0x1000).flow == "cond"
    assert d("6000FFFC", pc=0x1000).flow == "jump"


def test_decode_move_word_to_absolute_long():
    i = d("33C100FF1C08")
    assert (i.mnemonic, i.opsize, i.size) == ("move", 2, 6)
    assert i.operands == (Operand("dreg", reg=1), Operand("absl", value=0xFF1C08))


def test_decode_absolute_short_sign_extends():
    i = d("4A38FF00")  # tst.b $ff00.w
    assert i.operands == (Operand("absw", value=0xFFFFFF00),)


def test_decode_immediate_sizes():
    assert d("0C9000100000").operands[0] == Operand("imm", value=0x100000)  # cmpi.l
    assert d("0C9000100000").size == 6
    assert d("0C0000FF").operands[0] == Operand("imm", value=0xFF)  # cmpi.b uses low byte
    assert d("70FF").operands[0] == Operand("imm", value=0xFFFFFFFF)  # moveq sign-extends


def test_decode_displacement_and_index_modes():
    assert d("1140FFFE").operands[1] == Operand("disp", reg=0, value=-2)
    i = d("31BC12340004")  # move.w #$1234,4(a0,d0.w)
    assert i.operands[1] == Operand("index", reg=0, value=4, xreg=0, xlong=False)
    assert i.size == 6
    j = d("4EFB0002", pc=0x1000)  # jmp 2(pc,d0.w)
    assert j.operands[0] == Operand("pcindex", value=0x1004, xreg=0, xlong=False)
    assert j.flow == "indirect"
    k = d("41FA007C", pc=0x1000)  # lea $7c(pc),a0
    assert k.operands == (Operand("pcdisp", value=0x107E), Operand("areg", reg=0))


def test_decode_movem_normalises_predecrement_mask():
    push = d("48E7C000")  # movem.l d0-d1,-(a7)
    assert push.operands == (Operand("reglist", value=0x0003), Operand("predec", reg=7))
    pop = d("4CDF7FFF")  # movem.l (a7)+,d0-d7/a0-a6
    assert pop.operands == (Operand("postinc", reg=7), Operand("reglist", value=0x7FFF))
    assert (push.mnemonic, push.opsize, push.size) == ("movem", 4, 4)


def test_decode_status_register_forms():
    assert d("46FC2700").operands == (Operand("imm", value=0x2700), Operand("sr"))
    assert d("46FC2700").mnemonic == "move"
    assert d("027CF8FF").operands == (Operand("imm", value=0xF8FF), Operand("sr"))
    assert d("4E73").flow == "ret"  # rte


def test_decode_shift_forms():
    i = d("E348")  # lsl.w #1,d0
    assert (i.mnemonic, i.opsize, i.operands) == (
        "lsl",
        2,
        (Operand("imm", value=1), Operand("dreg", reg=0)),
    )
    j = d("E0A8")  # lsr.l d0,d0
    assert (j.mnemonic, j.opsize, j.operands) == (
        "lsr",
        4,
        (Operand("dreg", reg=0), Operand("dreg", reg=0)),
    )
    assert d("E1D0").operands == (Operand("ind", reg=0),)  # asl (a0)


def test_decode_condition_is_part_of_mnemonic_and_cond_field():
    assert (d("56C0").mnemonic, d("56C0").cond) == ("sne", 6)
    assert (d("6700FFFE").mnemonic, d("6700FFFE").cond) == ("beq", 7)


def test_decode_rejects_odd_pc():
    with pytest.raises(DecodeError) as e:
        decode(bytes.fromhex("004E71"), 1)
    assert e.value.pc == 1


def test_decode_rejects_illegal_opcode():
    for opcode in ("FFFF", "A000", "4AFC"):
        with pytest.raises(DecodeError):
            d(opcode)


def test_decode_rejects_68020_only_addressing():
    with pytest.raises(DecodeError):
        d("31BC12340104")  # full extension word format (bit 8 set)


def test_decode_rejects_truncated_instruction():
    with pytest.raises(DecodeError):
        d("4EB9000C")


@pytest.mark.parametrize(
    "encoding",
    [
        "00080001",  # ori.b #1,a0
        "48D8",  # movem.l regs,(a0)+
        "48C8",  # movem.l regs,a0 is ext only for a data register
        "48FA0000",  # movem.l regs,(d16,pc)
        "4CE00001",  # movem.l -(a0),regs
        "08080000",  # btst #0,a0
        "4248",  # clr.w a0
        "D008",  # add.b a0,d0
        "1008",  # move.b a0,d0
        "E1C0",  # memory shift on d0
        "917C0001",  # sub.w d0,#imm
        "4808",  # nbcd a0
        "4AC8",  # tas a0
        "4A7C0001",  # tst.w #imm
        "B13C0001",  # eor.b d0,#imm
        "81FC0000",  # divs #0 is valid, see below; this entry is replaced
        "50FA0000",  # st (d16,pc)
        "44C8",  # move a0,ccr
        "C1C8",  # muls a0,d0
    ],
)
def test_decode_rejects_invalid_addressing_modes(encoding):
    if encoding == "81FC0000":
        assert d(encoding).mnemonic == "divs"  # immediate source is legal
        return
    with pytest.raises(DecodeError):
        d(encoding + "0000")


def test_decode_accepts_the_legal_edge_modes():
    assert d("083A00000000").mnemonic == "btst"  # btst #0,(d16,pc)
    assert d("0B3C0001").mnemonic == "btst"  # btst d5,#imm
    assert d("B0C8").mnemonic == "cmpa"  # cmpa.w a0,a0
    assert d("5248").mnemonic == "addq"  # addq.w #1,a0
    assert d("3008").mnemonic == "move"  # move.w a0,d0
    assert d("4CD80001").mnemonic == "movem"  # movem.l (a0)+,d0
    assert d("48E08000").mnemonic == "movem"  # movem.l d0,-(a0)
