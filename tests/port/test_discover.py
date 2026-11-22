from __future__ import annotations

import pytest

from rr2port.discover import (
    AnnotationError,
    PointerTable,
    WrongRomError,
    discover,
    indirect_sites,
    load_annotations,
    pointer_table_targets,
    trace_entries,
    verify_rr206,
)


def test_discover_follows_branch_and_call_but_not_data():
    # 0: bsr.s +4 ; 2: rts ; 4: dc.w $FFFF (data) ; 6: moveq #0,d0 ; 8: rts
    rom = bytes.fromhex("61044E75FFFF70004E75")
    assert sorted(discover(rom, [0])) == [0, 2, 6, 8]


def test_discover_uses_jump_table_annotation():
    # 0: jmp (a0) ; 2: dc.w $FFFF ; 4: rts
    rom = bytes.fromhex("4ED0FFFF4E75")
    assert sorted(discover(rom, [0])) == [0]
    assert sorted(discover(rom, [0], {0: [4]})) == [0, 4]


def test_discover_stops_a_path_at_undecodable_bytes():
    # 0: nop ; 2: dc.w $FFFF
    rom = bytes.fromhex("4E71FFFF")
    assert sorted(discover(rom, [0])) == [0]


def test_discover_ignores_entries_outside_the_rom():
    assert discover(bytes.fromhex("4E75"), [0, 0x100000]).keys() == {0}


def test_load_annotations_reads_entries_and_jump_tables(tmp_path):
    path = tmp_path / "a.toml"
    path.write_text('entries = [0x200, 0x35A]\n[jump_tables]\n"0xCBDEE" = [0x1000, 0x2000]\n')
    annotations = load_annotations(path)
    assert annotations.entries == [0x200, 0x35A]
    assert annotations.jump_tables == {0xCBDEE: [0x1000, 0x2000]}


def test_trace_entries_collects_distinct_pcs(tmp_path):
    regs = " ".join(["00000000"] * 16)
    path = tmp_path / "trace.txt"
    path.write_text(
        f"T 000206 2700 {regs} 1\nT 000208 2700 {regs} 2\nT 000206 2700 {regs} 3\nE 3\n"
    )
    assert trace_entries(path) == {0x206, 0x208}


def test_verify_rr206_names_both_hashes():
    with pytest.raises(WrongRomError) as e:
        verify_rr206(b"\0" * 0x100000)
    assert "c7a60901" in str(e.value) and e.value.actual_sha256 in str(e.value)


def test_callback_slot_makes_moved_immediates_code():
    # 0: move.l #$10,$ff0300.l ; a: rts ; 10: rts
    rom = bytes.fromhex("23FC0000001000FF03004E75").ljust(0x10, b"\xff") + bytes.fromhex("4E75")
    assert sorted(discover(rom, [0])) == [0, 0xA]
    assert sorted(discover(rom, [0], callback_slots=[0xFF0300])) == [0, 0xA, 0x10]


def test_callback_slot_matches_absolute_short_form():
    def rom_with(destination: str) -> bytes:
        # 0: move.l #$10,(xxx).w ; 8: rts ; 10: rts
        return bytes.fromhex("21FC00000010" + destination + "4E75").ljust(
            0x10, b"\xff"
        ) + bytes.fromhex("4E75")

    # ($0300).w is $000300, not the slot $FF0300
    assert sorted(discover(rom_with("0300"), [0], callback_slots=[0xFF0300])) == [0, 8]
    # ($8300).w sign-extends to $FF8300
    assert sorted(discover(rom_with("8300"), [0], callback_slots=[0xFF8300])) == [0, 8, 0x10]


def test_callback_slot_ignores_a_zero_immediate():
    rom = bytes.fromhex("23FC0000000000FF03004E75")
    assert sorted(discover(rom, [0], callback_slots=[0xFF0300])) == [0, 0xA]


def test_long_pointer_table_targets():
    rom = bytes(8) + bytes.fromhex("0000000400000006")
    table = PointerTable(address=8, count=2, kind="long")
    assert pointer_table_targets(rom, table) == [4, 6]


def test_word_offset_pointer_table_targets():
    rom = bytes.fromhex("0004FFFE")
    assert pointer_table_targets(rom, PointerTable(0, 2, "word_offset", base=0x100)) == [
        0x104,
        0xFE,
    ]


def test_pointer_table_running_past_the_rom_is_an_error():
    with pytest.raises(AnnotationError) as e:
        pointer_table_targets(bytes(8), PointerTable(address=4, count=2, kind="long"))
    assert "000004" in str(e.value)


def test_pointer_table_entries_become_code():
    # 0: rts ; 2: rts ; 4: dc.l 2
    rom = bytes.fromhex("4E754E7500000002")
    found = discover(rom, [0], pointer_tables=[PointerTable(address=4, count=1, kind="long")])
    assert sorted(found) == [0, 2]


def test_odd_or_undecodable_indirect_target_is_an_error():
    rom = bytes.fromhex("4ED0FFFF")
    for target in (1, 2, 0x100000):
        with pytest.raises(AnnotationError) as e:
            discover(rom, [0], {0: [target]})
        assert f"{target:06X}" in str(e.value) and "000000" in str(e.value)


def test_indirect_sites_lists_only_unresolved_jumps():
    # 0: jmp (a0) ; 2: jsr $8.l ; 8: jsr 2(pc,d0.w) ; c: rts
    rom = bytes.fromhex("4ED04EB9000000084EBB00024E75")
    assert indirect_sites(discover(rom, [0, 2])) == [0, 8]


@pytest.mark.parametrize(
    "text, key",
    [
        ("callback_slot = [1]\n", "callback_slot"),
        ("[[pointer_table]]\naddress = 1\n", "pointer_table"),
        ("[[pointer_tables]]\naddress = 8\nkind = 'long'\n", "count"),
        ("[[pointer_tables]]\naddress = 8\ncount = 1\nkind = 'short'\n", "short"),
        ("[[pointer_tables]]\naddress = 8\ncount = 1\nkind = 'long'\nsize = 2\n", "size"),
        ('[jump_tables]\n"zz" = [2]\n', "zz"),
    ],
)
def test_load_annotations_rejects_unknown_or_incomplete_keys(tmp_path, text, key):
    path = tmp_path / "a.toml"
    path.write_text(text)
    with pytest.raises(AnnotationError) as e:
        load_annotations(path)
    assert key in str(e.value)


def test_load_annotations_reads_callback_slots_and_pointer_tables(tmp_path):
    path = tmp_path / "a.toml"
    path.write_text(
        "entries = [0x200]\ncallback_slots = [0xFF0300]\n"
        "[[pointer_tables]]\naddress = 0x52F8\ncount = 4\nkind = 'long'\nsites = [0xCBDEE]\n"
        "note = 'front end'\n[jump_tables]\n"
    )
    annotations = load_annotations(path)
    assert annotations.callback_slots == [0xFF0300]
    assert annotations.pointer_tables == [
        PointerTable(address=0x52F8, count=4, kind="long", sites=(0xCBDEE,), note="front end")
    ]


def test_long_offset_pointer_table_targets():
    rom = bytes.fromhex("00000010FFFFFFFE")
    table = PointerTable(0, 2, "long_offset", base=0x100)
    assert pointer_table_targets(rom, table) == [0x110, 0xFE]


def test_pointer_table_stride_skips_the_rest_of_each_record():
    rom = bytes.fromhex("00000004AAAAAAAA00000006BBBBBBBB")
    table = PointerTable(address=0, count=2, kind="long", stride=8)
    assert pointer_table_targets(rom, table) == [4, 6]


def test_null_entries_in_a_long_pointer_table_are_skipped():
    rom = bytes.fromhex("00000000000000084E75")
    assert pointer_table_targets(rom, PointerTable(0, 2, "long")) == [8]


def test_pointer_table_stride_smaller_than_an_entry_is_an_error(tmp_path):
    path = tmp_path / "a.toml"
    path.write_text("[[pointer_tables]]\naddress = 8\ncount = 1\nkind = 'long'\nstride = 2\n")
    with pytest.raises(AnnotationError) as e:
        load_annotations(path)
    assert "stride" in str(e.value)


# 0: movea.l #$10,a0 ; 6: jsr $c.l ; c: rts ; (pad) ; 10: rts
REGISTRAR_ROM = bytes.fromhex("207C000000104EB90000000C4E75").ljust(0x10, b"\xff") + bytes.fromhex(
    "4E75"
)


def test_callback_registrar_makes_the_pointer_loaded_into_a0_code():
    assert sorted(discover(REGISTRAR_ROM, [0])) == [0, 6, 0xC]
    assert sorted(discover(REGISTRAR_ROM, [0], callback_registrars=[0xC])) == [0, 6, 0xC, 0x10]


def test_callback_registrar_only_looks_at_the_instruction_just_before_the_call():
    # 0: movea.l #$10,a0 ; 6: nop ; 8: jsr $e.l ; e: rts ; 10: rts
    rom = bytes.fromhex("207C000000104E714EB90000000E4E754E75")
    assert sorted(discover(rom, [0], callback_registrars=[0xE])) == [0, 6, 8, 0xE]


def test_load_annotations_reads_callback_registrars(tmp_path):
    path = tmp_path / "a.toml"
    path.write_text("callback_registrars = [0x1224]\n")
    assert load_annotations(path).callback_registrars == [0x1224]


def test_callback_registrar_pass_runs_even_when_the_last_entry_does_not_decode():
    # 0: movea.l #$40,a0 ; 6: jsr $20.w ; a: rts ; 20: rts ; 40: rts ; 50: not code
    rom = bytearray(b"\xff" * 0x60)
    rom[0:12] = bytes.fromhex("207C000000404EB800204E75")
    rom[0x20:0x22] = bytes.fromhex("4E75")
    rom[0x40:0x42] = bytes.fromhex("4E75")
    for entries in ([0, 0x50], [0x50, 0]):
        found = discover(bytes(rom), entries, callback_registrars=[0x20])
        assert 0x40 in found, entries


def test_jump_table_for_an_address_that_is_not_an_indirect_site_is_an_error():
    rom = bytes.fromhex("4ED04E754E75")  # 0: jmp (a0) ; 2: rts ; 4: rts
    for site in (2, 3, 0x40):  # an rts, an odd address, never discovered
        with pytest.raises(AnnotationError) as e:
            discover(rom, [0], {0: [4], site: [4]})
        assert f"{site:06X}" in str(e.value)


def test_jump_table_with_no_targets_is_an_error():
    rom = bytes.fromhex("4ED04E75")
    with pytest.raises(AnnotationError) as e:
        discover(rom, [0], {0: []})
    assert "000000" in str(e.value)
