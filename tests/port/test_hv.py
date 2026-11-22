from __future__ import annotations

import pytest

from rr2port.hv import HvSample, fit_counters, read_hv_log
from rr2port.mame_cmd import build_hv_cmd
from rr2port.trace import TraceFormatError


def test_read_hv_log_parses_counter_reads_and_interrupts(tmp_path):
    log = tmp_path / "error.log"
    log.write_text(
        "noise\n"
        "RR2HV C00008 10 2A5C 184 42 1000\n"
        "RR2HV C00009 8 5C 184 42 1010\n"
        "RR2INT 6 300 224 2000\n"
    )
    samples, interrupts = read_hv_log(log)
    assert samples == [
        HvSample(addr=0xC00008, size=2, value=0x2A5C, beamx=184, beamy=42, cycles=1000),
        HvSample(addr=0xC00009, size=1, value=0x5C, beamx=184, beamy=42, cycles=1010),
    ]
    assert [(i.level, i.beamx, i.beamy, i.cycles) for i in interrupts] == [(6, 300, 224, 2000)]


def test_read_hv_log_rejects_a_short_row(tmp_path):
    log = tmp_path / "error.log"
    log.write_text("RR2HV C00008 10 2A5C 184\n")
    with pytest.raises(TraceFormatError) as e:
        read_hv_log(log)
    assert e.value.line_number == 1


def sample(value: int, beamx: int, beamy: int, *, addr=0xC00008, size=2) -> HvSample:
    return HvSample(addr=addr, size=size, value=value, beamx=beamx, beamy=beamy, cycles=0)


def test_fit_recovers_step_tables_from_word_and_byte_reads():
    samples = [
        sample(0x1020, 64, 16),
        sample(0x1021, 66, 16),
        sample(0x1120, 64, 17),
        sample(0x21, 66, 99, addr=0xC00009, size=1),  # H only
        sample(0x12, 0, 18, addr=0xC00008, size=1),  # V only
    ]
    fit = fit_counters(samples)
    assert fit.v_by_beamy == {16: 0x10, 17: 0x11, 18: 0x12}
    assert fit.h_by_beamx == {64: 0x20, 66: 0x21}
    assert fit.residual == []


def test_fit_reports_samples_the_tables_cannot_reproduce():
    odd = sample(0x1099, 64, 16)
    fit = fit_counters([sample(0x1020, 64, 16), sample(0x1020, 64, 16), odd])
    assert fit.h_by_beamx == {64: 0x20}  # the majority value
    assert fit.residual == [odd]


def test_hv_cmd_logs_counter_reads_and_interrupt_entries_in_the_window():
    lines = build_hv_cmd(500, 20).splitlines()
    watch = [line for line in lines if line.startswith("wp ")]
    assert len(watch) == 1 and watch[0].startswith("wp 0xc00008,0x2,r,")
    assert "beamx" in watch[0] and "beamy" in watch[0] and "totalcycles" in watch[0]
    assert sum(1 for line in lines if line.startswith("bp 0x14be,")) == 1
    assert sum(1 for line in lines if line.startswith("bp 0x11b8,")) == 1
    assert any("temp0==0x1f4" in line for line in lines)  # window starts at handoff 500
    assert any("temp0==0x208" in line and "quit" in line for line in lines)  # ends 20 later
    assert lines[-1] == "g"
