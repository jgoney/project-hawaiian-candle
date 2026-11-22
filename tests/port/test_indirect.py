from __future__ import annotations

import pytest

from rr2port.decode import decode
from rr2port.discover import AnnotationError
from rr2port.indirect import (
    build_indirect_cmd,
    merge_jump_tables,
    read_indirect_log,
    target_expression,
)
from rr2port.trace import TraceFormatError


def insn(hex_bytes: str, pc: int = 0):
    return decode(bytes(pc) + bytes.fromhex(hex_bytes), pc)


def test_target_expression_for_register_indirect():
    assert target_expression(insn("4ED0")) == "a0"  # jmp (a0)
    assert target_expression(insn("4E97")) == "sp"  # jsr (a7)


def test_target_expression_for_displacement():
    assert target_expression(insn("4EEB0010")) == "(a3+0x10)"  # jmp $10(a3)
    assert target_expression(insn("4EEBFFF0")) == "(a3-0x10)"  # jmp -$10(a3)


def test_target_expression_for_index():
    word = "(((d0&0xffff)^0x8000)-0x8000)"
    assert target_expression(insn("4EF00004")) == f"(a0+0x4+{word})"  # jmp 4(a0,d0.w)
    assert target_expression(insn("4EF09800")) == "(a0+0x0+a1)"  # jmp 0(a0,a1.l)
    assert target_expression(insn("4EFB0002", pc=0x1000)) == f"(0x1004+{word})"  # jmp 2(pc,d0.w)


def test_build_indirect_cmd_has_one_breakpoint_per_site():
    sites = {0x11DC: insn("4E90", pc=0x11DC), 0x2C416: insn("4ED0", pc=0x2C416)}
    lines = build_indirect_cmd(sites).splitlines()
    assert [line for line in lines if line.startswith("bp ")] == [
        'bp 0x11dc,1,{logerror "RR2IND %06X %06X\\n",0x11dc,(a0)&0xffffff; g}',
        'bp 0x2c416,1,{logerror "RR2IND %06X %06X\\n",0x2c416,(a0)&0xffffff; g}',
    ]
    assert lines[-1] == "g"


def test_read_indirect_log_collects_targets_per_site(tmp_path):
    log = tmp_path / "error.log"
    log.write_text("noise\nRR2IND 0011DC 00E038\nRR2IND 0011DC 00035A\nRR2IND 0011DC 00E038\n")
    assert read_indirect_log(log) == {0x11DC: {0xE038, 0x35A}}


def test_read_indirect_log_rejects_a_malformed_row(tmp_path):
    log = tmp_path / "error.log"
    log.write_text("RR2IND 0011DC\n")
    with pytest.raises(TraceFormatError) as e:
        read_indirect_log(log)
    assert e.value.line_number == 1


# 0: jmp (a0) ; 2: rts ; 4: rts ; 6: dc.w $FFFF
ROM = bytes.fromhex("4ED04E754E75FFFF")
HEAD = "# comment kept\nentries = [0]\n\n[jump_tables]\n"


def test_merge_keeps_existing_targets_and_adds_new_ones_sorted():
    text = HEAD + '"000000" = [0x000004]\n'
    merged = merge_jump_tables(text, {0: {2}}, ROM)
    assert merged == HEAD + '"000000" = [0x000002, 0x000004]\n'


def test_merge_leaves_everything_above_the_section_untouched():
    merged = merge_jump_tables(HEAD, {0: {4}}, ROM)
    assert merged.startswith(HEAD)
    assert merged == HEAD + '"000000" = [0x000004]\n'


def test_merge_requires_jump_tables_to_be_the_last_section():
    with pytest.raises(AnnotationError):
        merge_jump_tables("[jump_tables]\n[[pointer_tables]]\naddress = 1\n", {0: {2}}, ROM)
    with pytest.raises(AnnotationError):
        merge_jump_tables("entries = [0]\n", {0: {2}}, ROM)


@pytest.mark.parametrize("target", [3, 6, 0x100000])
def test_merge_rejects_impossible_targets(target):
    with pytest.raises(AnnotationError) as e:
        merge_jump_tables(HEAD, {0: {target}}, ROM)
    assert f"{target:06X}" in str(e.value) and "000000" in str(e.value)
