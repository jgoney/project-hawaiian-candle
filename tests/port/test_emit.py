"""Instruction semantics: translate a snippet, run it natively, check the result.

CCR bits: X=0x10, N=0x08, Z=0x04, V=0x02, C=0x01.
"""

from __future__ import annotations

from harness import run_snippet


def run(hex_code: str, tmp_path, **state):
    return run_snippet(bytes.fromhex(hex_code), tmp_path, **state)


def test_add_word_overflow(tmp_path):
    r = run("D041", tmp_path, d={0: 0x7FFF, 1: 1})
    assert (r.d[0], r.ccr, r.cycles, r.pc) == (0x8000, 0x0A, 4, 2)


def test_add_word_preserves_upper_half(tmp_path):
    r = run("D041", tmp_path, d={0: 0x1234FFFF, 1: 1})
    assert (r.d[0], r.ccr) == (0x12340000, 0x15)


def test_sub_byte_borrow(tmp_path):
    r = run("9001", tmp_path, d={0: 0, 1: 1})
    assert (r.d[0], r.ccr) == (0xFF, 0x19)


def test_moveq_negative_keeps_x(tmp_path):
    r = run("70FF", tmp_path, sr=0x2710)
    assert (r.d[0], r.ccr) == (0xFFFFFFFF, 0x18)


def test_lsl_word_shifts_out_carry(tmp_path):
    r = run("E348", tmp_path, d={0: 0x8000})
    assert (r.d[0], r.ccr) == (0, 0x15)


def test_mulu_word(tmp_path):
    r = run("C0C1", tmp_path, d={0: 0xFFFF, 1: 0xFFFF})
    assert (r.d[0], r.ccr) == (0xFFFE0001, 0x08)


def test_divu_word(tmp_path):
    r = run("80C1", tmp_path, d={0: 7, 1: 2})
    assert (r.d[0], r.ccr) == (0x00010003, 0x00)


def test_dbf_loops_then_falls_through(tmp_path):
    first = run("51C8FFFE", tmp_path, d={0: 1})
    assert (first.pc, first.d[0] & 0xFFFF) == (0, 0)
    second = run("51C8FFFE", tmp_path, d={0: 1}, steps=2)
    assert (second.pc, second.d[0] & 0xFFFF) == (4, 0xFFFF)


def test_movem_predecrement_order(tmp_path):
    r = run("48E7C000", tmp_path, d={0: 0x11111111, 1: 0x22222222}, a={7: 0xFFFF08})
    assert r.a[7] == 0xFFFF00
    assert r.ram(0xFFFF00, 8) == bytes.fromhex("1111111122222222")


def test_jsr_pushes_return_address_into_ram(tmp_path):
    r = run("4EB9000000084E714E75", tmp_path, a={7: 0xFFFF04})
    assert (r.pc, r.a[7]) == (8, 0xFFFF00)
    assert r.ram(0xFFFF00, 4) == bytes.fromhex("00000006")


def test_rts_returns(tmp_path):
    r = run("4E75", tmp_path, a={7: 0xFFFF00}, ram={0xFFFF00: bytes.fromhex("00000006")})
    assert (r.pc, r.a[7]) == (6, 0xFFFF04)


def test_move_word_postincrement_to_predecrement(tmp_path):
    # move.w (a0)+,-(a1)
    r = run("3318", tmp_path, a={0: 0xFF0000, 1: 0xFF0102}, ram={0xFF0000: b"\x80\x01"})
    assert (r.a[0], r.a[1], r.ccr) == (0xFF0002, 0xFF0100, 0x08)
    assert r.ram(0xFF0100, 2) == b"\x80\x01"


def test_byte_push_keeps_stack_pointer_even(tmp_path):
    # move.b d0,-(a7)
    r = run("1F00", tmp_path, d={0: 0xAB}, a={7: 0xFFFF02})
    assert r.a[7] == 0xFFFF00 and r.ram(0xFFFF00, 1) == b"\xab"


def test_movea_word_sign_extends(tmp_path):
    r = run("307CFFFE", tmp_path)  # movea.w #$fffe,a0
    assert r.a[0] == 0xFFFFFFFE


def test_btst_register_uses_bit_modulo_32(tmp_path):
    r = run("0300", tmp_path, d={0: 0x00010000, 1: 48})  # btst d1,d0
    assert r.ccr & 0x04 == 0


def test_unimplemented_instruction_is_fatal(tmp_path):
    r = run("4E722700", tmp_path)  # stop #$2700
    assert r.returncode == 2
    assert "RR2FATAL" in r.stderr and "unimplemented" in r.stderr


def test_abcd_adds_decimal_digits(tmp_path):
    r = run("C101", tmp_path, d={0: 0x19, 1: 0x03}, sr=0x2704)  # abcd d1,d0
    assert (r.d[0], r.ccr) == (0x22, 0x00)


def test_abcd_carry_out_keeps_zero_flag(tmp_path):
    r = run("C101", tmp_path, d={0: 0x99, 1: 0x01}, sr=0x2704)
    assert (r.d[0], r.ccr) == (0x00, 0x15)
