from __future__ import annotations

from rr2port.cli import code_report_main
from rr2port.code_report import build_code_report, format_code_report
from rr2port.discover import Annotations, PointerTable

# 0: jmp (a0) ; 2: jsr (a1) ; 4: rts ; 6: rts ; 8: dc.l 6
ROM = bytes.fromhex("4ED04E914E754E7500000006")


def annotations(**changes) -> Annotations:
    base = Annotations(entries=[0, 2])
    for key, value in changes.items():
        setattr(base, key, value)
    return base


def test_report_counts_code_and_flags_unresolved_sites():
    report = build_code_report(ROM, annotations(jump_tables={0: [6]}), {})
    assert (report.instructions, report.code_bytes) == (4, 8)
    assert [
        (row.site, row.operand, row.observed_targets, row.resolved) for row in report.sites
    ] == [
        (0, "jmp (a0)", 1, True),
        (2, "jsr (a1)", 0, False),
    ]


def test_pointer_table_naming_a_site_resolves_it():
    table = PointerTable(address=8, count=1, kind="long", sites=(2,))
    report = build_code_report(ROM, annotations(jump_tables={0: [6]}, pointer_tables=[table]), {})
    row = report.sites[1]
    assert (row.table_targets, row.resolved) == (1, True)


def test_traced_addresses_that_were_not_discovered_are_listed():
    report = build_code_report(ROM, annotations(), {"boot": {0, 4, 6}})
    assert report.missing == {"boot": [6]}


def test_format_states_the_unresolved_count():
    text = format_code_report(build_code_report(ROM, annotations(), {"boot": {6}}))
    assert "unresolved indirect sites: 2" in text
    assert "boot: 1 traced addresses not discovered" in text
    assert "000002  jsr (a1)" in text


def test_cli_exit_status_reflects_unresolved_sites(tmp_path, monkeypatch, capsys):
    rom = tmp_path / "rom.bin"
    rom.write_bytes(ROM)
    monkeypatch.setattr("rr2port.cli.verify_rr206", lambda data: None)
    path = tmp_path / "a.toml"
    path.write_text('entries = [0, 2]\n[jump_tables]\n"0" = [6]\n')
    assert code_report_main([str(rom), "--annotations", str(path)]) == 1
    path.write_text('entries = [0, 2]\n[jump_tables]\n"0" = [6]\n"2" = [6]\n')
    assert code_report_main([str(rom), "--annotations", str(path)]) == 0
    assert "unresolved indirect sites: 0" in capsys.readouterr().out


def test_cli_rejects_bad_annotations(tmp_path, monkeypatch, capsys):
    rom = tmp_path / "rom.bin"
    rom.write_bytes(ROM)
    monkeypatch.setattr("rr2port.cli.verify_rr206", lambda data: None)
    path = tmp_path / "a.toml"
    path.write_text("callback_slot = [1]\n")
    assert code_report_main([str(rom), "--annotations", str(path)]) == 2
    assert "callback_slot" in capsys.readouterr().err


def test_one_pointer_table_can_resolve_several_sites():
    table = PointerTable(address=8, count=1, kind="long", sites=(0, 2))
    report = build_code_report(ROM, annotations(pointer_tables=[table]), {})
    assert [row.resolved for row in report.sites] == [True, True]
