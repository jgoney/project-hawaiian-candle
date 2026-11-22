from __future__ import annotations

from rr2port.cli import translate_main


def test_translate_refuses_wrong_rom(tmp_path, capsys):
    rom = tmp_path / "zero.gen"
    rom.write_bytes(b"\0" * 0x100000)
    out = tmp_path / "generated" / "code.c"
    out.parent.mkdir()
    assert translate_main([str(rom), "-o", str(out)]) == 2
    message = capsys.readouterr().err
    assert "c7a60901" in message and "expected" in message and "found" in message
    assert not out.exists()


def test_translate_refuses_output_outside_generated(tmp_path, capsys):
    rom = tmp_path / "zero.gen"
    rom.write_bytes(b"\0" * 0x100000)
    out = tmp_path / "code.c"
    assert translate_main([str(rom), "-o", str(out)]) == 2
    assert "generated" in capsys.readouterr().err
    assert not out.exists()


def test_translate_reports_missing_rom(tmp_path, capsys):
    out = tmp_path / "generated" / "code.c"
    assert translate_main([str(tmp_path / "missing.gen"), "-o", str(out)]) == 2
    assert "missing.gen" in capsys.readouterr().err


def test_translate_has_no_trace_seed_option(tmp_path, capsys):
    out = tmp_path / "generated" / "code.c"
    try:
        translate_main(["rom.gen", "--trace-seed", "x", "-o", str(out)])
    except SystemExit as error:
        assert error.code == 2
    else:
        raise AssertionError("--trace-seed was accepted")
    assert "unrecognized arguments" in capsys.readouterr().err
