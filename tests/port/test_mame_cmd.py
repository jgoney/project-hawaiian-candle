from __future__ import annotations

import pytest

from rr2port.mame_cmd import (
    WINDOWS,
    build_handoff_stop_cmd,
    build_picture_cmd,
    build_stop_cmd,
    build_trace_cmd,
)


def test_race_cmd_saves_one_ram_image_per_handoff():
    text = build_trace_cmd(WINDOWS["race"], "out")
    assert [f"handoff-{n:02d}.bin" in text for n in range(21)] == [True] * 21
    assert "ff0000,10000" in text


def test_boot_cmd_starts_trace_before_first_instruction():
    lines = build_trace_cmd(WINDOWS["boot"], "out").splitlines()
    trace_line = next(line for line in lines if line.startswith("trace "))
    assert lines.index(trace_line) < lines.index("g")


def test_counts_are_written_in_hex_because_the_debugger_reads_numbers_as_hex():
    race = build_trace_cmd(WINDOWS["race"], "out")
    assert "temp0<0x1f3," in race  # handoffs before 499 are only counted
    assert "temp0==0x1f4," in race  # window starts at handoff 500
    assert "temp0==0x208," in race  # and ends 20 handoffs later
    boot = build_trace_cmd(WINDOWS["boot"], "out")
    assert "temp1==0x1d," in boot  # 30th VBlank entry


def test_trace_windows_take_no_picture():
    # Taking a snapshot while tracing made MAME hang on exit; pictures are
    # recorded by a separate run.
    assert "snap" not in build_trace_cmd(WINDOWS["boot"], "out")
    assert "snap" not in build_trace_cmd(WINDOWS["race"], "out")


def test_picture_cmd_snaps_at_each_requested_vblank_and_then_quits():
    lines = build_picture_cmd([60, 120]).splitlines()
    breakpoints = [line for line in lines if line.startswith("bp ")]
    assert breakpoints == [
        "bp 0x11b8,temp0!=0x3b && temp0!=0x77,{temp0++; g}",
        "bp 0x11b8,temp0==0x3b,{temp0++; snap boot-v0060; g}",
        "bp 0x11b8,temp0==0x77,{temp0++; snap boot-v0120; quit}",
    ]
    assert lines[-1] == "g"


def test_picture_cmd_needs_at_least_one_count():
    with pytest.raises(ValueError):
        build_picture_cmd([])


def test_stop_cmd_halts_at_the_requested_vblank_entry_without_resuming():
    lines = build_stop_cmd(660).splitlines()
    breakpoints = [line for line in lines if line.startswith("bp ")]
    assert breakpoints == [
        "bp 0x11b8,temp0!=0x293,{temp0++; g}",
        "bp 0x11b8,temp0==0x293,{temp0++}",
    ]
    assert lines[-1] == "g"


def test_handoff_stop_cmd_stops_at_every_handoff():
    lines = build_handoff_stop_cmd().splitlines()
    breakpoints = [line for line in lines if line.startswith("bp ")]
    assert breakpoints == [
        'bp 0xdf48,1,{logerror "RR2HANDOFF %d %04X ",totalcycles,sr; '
        'logerror "%08X %08X %08X %08X %08X %08X %08X %08X ",d0,d1,d2,d3,d4,d5,d6,d7; '
        'logerror "%08X %08X %08X %08X %08X %08X %08X %08X\\n",a0,a1,a2,a3,a4,a5,a6,sp}',
        'bp 0x1154,1,{logerror "RR2PAD 1 %02X\\n",d0&0xff; g}',
        'bp 0x11b6,1,{logerror "RR2PAD 2 %02X\\n",d0&0xff; g}',
    ]
    assert lines[-1] == "g"


def test_race_windows_cover_the_start_and_the_first_passes_that_differed():
    # race-start: where native timing first parted from MAME's; race-early: the
    # stretch after it, holding the first passes that still differed.
    assert (WINDOWS["race-start"].start, WINDOWS["race-start"].end_count) == (2, 12)
    assert (WINDOWS["race-early"].start, WINDOWS["race-early"].end_count) == (14, 30)
    assert WINDOWS["race-early"].playback == "full-throttle"
    # race-66: two of the ten passes that still differed after that.
    assert (WINDOWS["race-66"].start, WINDOWS["race-66"].end_count) == (66, 14)
    # The last two passes with a difference, each with the pass before it.
    assert (WINDOWS["race-822"].start, WINDOWS["race-822"].end_count) == (822, 2)
    assert (WINDOWS["race-953"].start, WINDOWS["race-953"].end_count) == (953, 2)
