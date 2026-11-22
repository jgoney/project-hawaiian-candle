"""rr-race-replay on a tiny synthetic "race": a loop with a handoff, a counter and a pad read."""

from __future__ import annotations

import subprocess

import pytest
from harness import REPO, build_snippet

from rr2port.handoffs import write_handoffs

TOOL = REPO / "port" / "tools" / "rr_race_replay.c"
HANDOFF_PC = 0x214
PAD_OFFSET = 0x062A
PAD_READ_PC = 0x220

CODE = (
    "13FC004000A10009"  # 200: move.b #$40,$a10009   pad TH is an output
    "13FC004000A10003"  # 208: move.b #$40,$a10003   TH high
    "323C0001"  # 210: move.w #1,d1            the timestep
    "33C100FF1C08"  # 214: move.w d1,$ff1c08       <- handoff
    "523900FF0000"  # 21a: addq.b #1,$ff0000       pass counter
    "103900A10003"  # 220: move.b $a10003,d0       read the pad
    "4600"  # 226: not.b d0
    "0200003F"  # 228: andi.b #$3f,d0
    "13C000FF062A"  # 22c: move.b d0,$ff062a       the raw pad byte, active high
    "60DC"  # 232: bra.s $210
)
PADS = [0x10, 0x01, 0x00]  # what the original read during passes 0, 1, 2


# The same race with a video register write at the end of each pass.
CODE_WITH_VIDEO_WRITE = CODE[:-4] + (
    "33FC8F0200C00004"  # 232: move.w #$8f02,$c00004   register 15 = 2
    "60D4"  # 23a: bra.s $210
)


def image(code_hex: str = CODE) -> bytes:
    rom = bytearray(0x400)
    rom[0:4] = bytes.fromhex("00FFFF00")
    rom[4:8] = bytes.fromhex("00000200")
    code = bytes.fromhex(code_hex)
    rom[0x200 : 0x200 + len(code)] = code
    return bytes(rom)


def reference(pads=PADS) -> list[bytes]:
    """Work RAM at each handoff of the synthetic race, as the original would leave it."""
    images = []
    for k in range(len(pads) + 1):
        ram = bytearray(0x10000)
        ram[0] = k
        if k > 0:
            ram[0x1C09] = 1
            ram[PAD_OFFSET] = pads[k - 1]
        images.append(bytes(ram))
    return images


@pytest.fixture
def race(tmp_path):
    def run(images=None, timesteps=None, *extra, mutate=None, pads=None, cycles=None, code=CODE):
        images = reference() if images is None else images
        handoffs = tmp_path / "handoffs.bin"
        write_handoffs(handoffs, images)
        steps = tmp_path / "timesteps.txt"
        values = [1] * len(images) if timesteps is None else timesteps
        steps.write_text("".join(f"{k} {value}\n" for k, value in enumerate(values)))
        pad_file = tmp_path / "pads.txt"
        # One "H" line per handoff, with the reads of each pass after its handoff.
        per_pass = [[value] for value in PADS] if pads is None else pads
        pad_file.write_text(
            "".join("H\n" + "".join(f"1 {v:02X}\n" for v in reads) for reads in per_pass) + "H\n"
        )
        # Registers at each handoff: MAME's cycle count, SR, D0-D7, A0-A7. D1 is the timestep.
        reg_file = tmp_path / "regs.txt"
        reg_file.write_text(
            "".join(
                f"{1000 + 100 * k if cycles is None else cycles[k]} 2700 00000000 {value:08X} "
                + " ".join(["00000000"] * 13)
                + " 00FFFF00\n"
                for k, value in enumerate(values)
            )
        )
        if mutate is not None:
            mutate(handoffs)
        binary, rom_path = build_snippet(
            image(code),
            tmp_path,
            entries=(0x200,),
            main=TOOL,
            defines=[
                "RR_ANY_ROM",
                f"HANDOFF_PC={HANDOFF_PC:#x}u",
                f"PAD1_READ_PC={PAD_READ_PC:#x}u",
                "PAD2_READ_PC=0xFFFFFEu",
                "OBJECTS_NOTE_PC=0x21au",
                "OBJECTS_DONE_PC=0x220u",
                "DASHES_PC=0x21au",
                "DASHES_DONE_PC=0x220u",
            ],
        )
        return subprocess.run(
            [str(binary), "--rom", str(rom_path), "--handoffs", str(handoffs),
             "--timesteps", str(steps), "--pads", str(pad_file), "--regs", str(reg_file),
             *extra],
            capture_output=True,
            text=True,
        )  # fmt: skip

    run.tmp_path = tmp_path
    return run


def test_identical_race_matches(race):
    done = race()
    assert done.returncode == 0, done.stdout + done.stderr
    assert "HANDOFFS compared=4 of 4" in done.stdout
    assert "TIMESTEP same=4 different=0 forced=0" in done.stdout
    assert "RAM identical=4 differing=0 first_differing=none" in done.stdout
    assert done.stdout.rstrip().endswith("RESULT match")


def test_a_differing_byte_is_reported_with_the_handoff(race):
    images = reference()
    changed = bytearray(images[2])
    changed[0x1234] = 0x55
    images[2] = bytes(changed)
    done = race(images)
    assert done.returncode == 1
    assert "first_differing=2" in done.stdout and "RESULT differ" in done.stdout
    assert "1234" in done.stdout


def test_an_excluded_range_hides_the_byte_and_says_how_many(race):
    images = reference()
    changed = bytearray(images[2])
    changed[0x1234] = 0x55
    images[2] = bytes(changed)
    done = race(images, None, "--exclude", "1230:123F:test range")
    assert done.returncode == 0, done.stdout
    assert "EXCLUDED start=1230 end=123F bytes_hidden=1 reason=test range" in done.stdout


def test_a_different_native_timestep_fails_unless_forced_and_is_still_counted(race):
    images = reference()
    # The original stored 2 during pass 1; natively the code always produces 1.
    for k in (2, 3):
        changed = bytearray(images[k])
        changed[0x1C09] = 2 if k == 2 else 1
        images[k] = bytes(changed)
    timesteps = [1, 2, 1, 1]
    free = race(images, timesteps)
    assert free.returncode == 1 and "TIMESTEP same=3 different=1 forced=0" in free.stdout
    forced = race(images, timesteps, "--force-timestep")
    assert forced.returncode == 0, forced.stdout
    assert "TIMESTEP same=3 different=1 forced=1" in forced.stdout
    assert forced.stdout.rstrip().endswith("RESULT logic-match")


def test_each_pad_read_gets_the_value_the_original_read_in_that_pass(race):
    values = [0x20, 0x08, 0x15]
    done = race(reference(values), pads=[[v] for v in values])
    assert done.returncode == 0, done.stdout
    assert "RAM identical=4 differing=0" in done.stdout
    assert "PADS native_reads=3 recorded=3 repeated=0 dropped=0" in done.stdout


def test_a_pass_with_fewer_recorded_reads_repeats_the_last_value(race):
    # The original read the pad once in pass 0 and never again in the recording;
    # natively the code reads once per pass, so passes 1 and 2 reuse 0x10.
    done = race(reference([0x10, 0x10, 0x10]), pads=[[0x10], [], []])
    assert done.returncode == 0, done.stdout
    assert "PADS native_reads=3 recorded=1 repeated=2 dropped=0" in done.stdout


def test_extra_recorded_reads_in_a_pass_are_dropped_not_carried_over(race):
    # Pass 0 has two recorded reads but the code reads once: the second (0x3F)
    # must not leak into pass 1, which has its own value.
    done = race(reference([0x10, 0x01, 0x00]), pads=[[0x10, 0x3F], [0x01], [0x00]])
    assert done.returncode == 0, done.stdout
    assert "PADS native_reads=3 recorded=4 repeated=0 dropped=1" in done.stdout


def test_reference_longer_than_the_native_run_is_out_of_step(race):
    done = race(None, None, "--max-frames", "0")
    assert done.returncode == 1
    assert "reached 0 of 4" in done.stderr


def test_report_lists_each_handoff(race):
    report = race.tmp_path / "report.txt"
    done = race(None, None, "--report", str(report))
    assert done.returncode == 0
    lines = report.read_text().splitlines()
    assert len(lines) == 4
    assert lines[1].split()[0] == "1" and lines[1].split()[2:6] == ["1", "1", "0", "0"]


def test_truncated_handoff_file_is_an_input_error(race):
    done = race(mutate=lambda path: path.write_bytes(path.read_bytes()[:-1]))
    assert done.returncode == 2
    assert "handoff 3" in done.stderr


def test_ram_at_writes_native_memory_at_a_handoff(race):
    out = race.tmp_path / "ram-2.bin"
    done = race(None, None, "--ram-at", f"2:{out}")
    assert done.returncode == 0, done.stdout
    assert out.read_bytes() == reference()[2]


def test_profile_counts_instructions_between_two_handoffs(race):
    out = race.tmp_path / "profile.txt"
    done = race(None, None, "--profile", f"1:3:{out}")
    assert done.returncode == 0, done.stdout
    counts = {
        int(pc, 16): int(n) for pc, n, _ in (line.split() for line in out.read_text().splitlines())
    }
    # Two whole passes: every instruction of the loop ran twice.
    assert counts[0x214] == 2 and counts[0x220] == 2 and counts[0x232] == 2
    assert 0x200 not in counts


def test_resync_restarts_each_pass_from_the_reference_so_one_bad_pass_stays_one(race):
    # The reference's pass counter jumps from 1 to 10 at handoff 2. Natively it
    # would be 2, and without resync every later handoff differs as well.
    images = []
    for k, counter in enumerate([0, 1, 10, 11]):
        ram = bytearray(reference()[k])
        ram[0] = counter
        images.append(bytes(ram))
    free = race(images)
    assert "RAM identical=2 differing=2 first_differing=2" in free.stdout
    resynced = race(images, None, "--resync")
    assert "RAM identical=3 differing=1 first_differing=2" in resynced.stdout
    assert resynced.returncode == 1


def test_resync_needs_the_register_file_to_cover_every_handoff(race):
    done = race(
        None, None, "--resync", mutate=lambda path: (path.parent / "regs.txt").write_text("")
    )
    assert done.returncode == 2 and "regs" in done.stderr


def test_offsets_file_tallies_which_bytes_differed(race):
    images = reference()
    for k in (2, 3):
        changed = bytearray(images[k])
        changed[0x1234] = 0x55
        images[k] = bytes(changed)
    out = race.tmp_path / "offsets.txt"
    race(images, None, "--offsets", str(out))
    assert out.read_text().splitlines() == ["1234 2"]


def test_dead_stack_below_the_stack_pointer_can_be_excluded(race):
    # The registers file puts the stack pointer at 0xFFFF00. A difference just
    # below it is leftover stack; one at or above it is live and still counts.
    images = reference()
    below = bytearray(images[2])
    below[0xFEFF] = 0x77
    images[2] = bytes(below)
    done = race(images, None, "--exclude-stack-below", "FE00")
    assert done.returncode == 0, done.stdout
    assert "EXCLUDED start=FE00 end=stack-pointer bytes_hidden=1 reason=dead stack" in done.stdout
    live = bytearray(images[2])
    live[0xFF00] = 0x77
    images[2] = bytes(live)
    done = race(images, None, "--exclude-stack-below", "FE00")
    assert done.returncode == 1 and "first_differing=2" in done.stdout


def test_registers_are_compared_when_the_register_file_is_given(race):
    # The synthetic register file says every register but D1 and A7 is 0.
    # Natively D0 holds the last pad read (0x10, 0x01, then 0x00), so it
    # differs at handoffs 1 and 2 and agrees again at handoff 3.
    # The cycle count is compared too, so the file is given the native counts.
    cycles = [_native_cycles(race, k) for k in range(4)]
    done = race(None, None, "--compare-regs", cycles=cycles)
    assert "REGS identical=2 differing=2" in done.stdout
    report = race.tmp_path / "report.txt"
    race(None, None, "--compare-regs", "--report", str(report), cycles=cycles)
    assert [line.split()[5] for line in report.read_text().splitlines()] == ["0", "1", "1", "0"]


def _native_cycles(race, k: int) -> int:
    out = race(None, None, "--ram-at", f"{k}:{race.tmp_path / 'scratch.bin'}").stdout
    return int(next(line for line in out.splitlines() if line.startswith("REGS_AT")).split()[2])


def test_the_cycle_count_at_a_handoff_is_compared_with_the_registers(race):
    # The default synthetic file's cycle counts are not the native ones, so
    # every handoff differs, by one more item than the registers alone.
    done = race(None, None, "--compare-regs")
    assert "REGS identical=0 differing=4" in done.stdout
    report = race.tmp_path / "report.txt"
    race(None, None, "--compare-regs", "--report", str(report))
    assert [line.split()[5] for line in report.read_text().splitlines()] == ["1", "2", "2", "1"]


def _native_line(output: str) -> str:
    return (
        next(line for line in output.splitlines() if line.strip().startswith("native:"))
        .split("native:", 1)[1]
        .strip()
    )


def test_follow_stops_at_the_first_instruction_that_differs_from_a_recorded_trace(race):
    # A recorded trace that is right for one instruction and then wrong. The
    # right line is the tool's own account of the handoff instruction.
    trace = race.tmp_path / "trace.txt"
    trace.write_text("T 000000 2700 " + " ".join(["00000000"] * 16) + " 0\n")
    first = race(None, None, "--follow", f"1:{trace}")
    assert "FOLLOW differs at recorded instruction 0" in first.stdout
    # A run cut short by a difference is not a match.
    assert first.returncode == 1 and "RESULT differ" in first.stdout
    assert "RESULT match" not in first.stdout
    handoff = _native_line(first.stdout)
    assert handoff.startswith(f"T {HANDOFF_PC:06X} ")

    trace.write_text(handoff + "\nT 000000 2700 " + " ".join(["00000000"] * 16) + " 0\n")
    second = race(None, None, "--follow", f"1:{trace}")
    assert "FOLLOW differs at recorded instruction 1" in second.stdout
    assert "  " + handoff in second.stdout  # shown as the last matching line
    assert _native_line(second.stdout).startswith("T 00021A ")


def test_follow_reports_a_trace_that_matches_to_its_end(race):
    trace = race.tmp_path / "trace.txt"
    trace.write_text("T 000000 2700 " + " ".join(["00000000"] * 16) + " 0\n")
    handoff = _native_line(race(None, None, "--follow", f"1:{trace}").stdout)
    trace.write_text(handoff + "\nE 1\n")
    done = race(None, None, "--follow", f"1:{trace}")
    assert "FOLLOW matched all 1 recorded instructions" in done.stdout
    assert done.returncode == 0, done.stdout


def test_cpu_speed_is_reported_clamped_and_does_not_change_the_logic(race):
    normal = race(None, None)
    assert "CPU_SPEED" not in normal.stdout  # the original speed is not announced
    fast = race(None, None, "--cpu-speed", "800")
    assert "CPU_SPEED percent=800" in fast.stdout
    assert "RESULT match" in fast.stdout and fast.returncode == 0, fast.stdout
    assert "CPU_SPEED percent=6400" in race(None, None, "--cpu-speed", "99999").stdout
    bad = race(None, None, "--cpu-speed", "fast")
    assert bad.returncode == 2 and "--cpu-speed" in bad.stderr


def test_track_prints_a_ram_value_at_every_handoff(race):
    report = race.tmp_path / "report.txt"
    done = race(None, None, "--report", str(report), "--track", "FF0000:1:pass",
                "--track", "FF1C08:2:step")  # fmt: skip
    assert done.returncode == 0, done.stdout
    lines = report.read_text().splitlines()
    assert [line.split()[-2:] for line in lines] == [
        ["pass=00", "step=0000"],
        ["pass=01", "step=0001"],
        ["pass=02", "step=0001"],
        ["pass=03", "step=0001"],
    ]
    bad = race(None, None, "--track", "FF0000:3:odd")
    assert bad.returncode == 2 and "--track" in bad.stderr


def test_pad_log_records_every_native_read_with_its_frame(race):
    log = race.tmp_path / "pad-log.txt"
    done = race(None, None, "--pad-log", str(log))
    assert done.returncode == 0, done.stdout
    assert log.read_text().splitlines() == ["0 1 10", "0 1 01", "0 1 00"]


def test_inputs_by_frame_feed_the_value_logged_at_or_before_each_frame_and_run_on(race):
    # Frame 0 held 0x10, frame 2 held 0x01. The run is not compared with the
    # reference and does not stop at its last handoff.
    log = race.tmp_path / "pad-log.txt"
    log.write_text("0 1 10\n2 1 01\n")
    report = race.tmp_path / "report.txt"
    done = race(None, None, "--inputs-by-frame", str(log), "--max-frames", "3",
                "--report", str(report), "--track", f"FF{PAD_OFFSET:04X}:1:pad")  # fmt: skip
    assert done.returncode == 0, done.stdout
    rows = [line.split() for line in report.read_text().splitlines()]
    assert len(rows) > 100  # far past the reference's four handoffs
    by_frame = {}
    for row in rows:
        by_frame.setdefault(int(row[1]), set()).add(row[-1])
    assert by_frame[0] == {"pad=00", "pad=10"}  # 00 only before the first read
    assert by_frame[1] == {"pad=10"}  # no entry for frame 1: the latest one before it holds
    assert rows[-1][-1] == "pad=01"
    assert f"FREE_RUN handoffs={len(rows)} frames=3" in done.stdout
    assert "RESULT" not in done.stdout


def test_cpu_speed_can_start_at_a_handoff_so_what_comes_before_is_unchanged(race):
    # Speed 800% from handoff 2 on: it takes effect at the next frame boundary,
    # so frame 0 runs at the original speed and frame 1 does eight times as much.
    log = race.tmp_path / "pad-log.txt"
    log.write_text("0 1 00\n")
    report = race.tmp_path / "report.txt"
    speed = ["--cpu-speed", "800", "--speed-from-handoff", "2"]
    done = race(None, None, "--inputs-by-frame", str(log), "--max-frames", "2",
                "--report", str(report), *speed)  # fmt: skip
    assert done.returncode == 0, done.stdout
    assert "CPU_SPEED percent=800 from_handoff=2" in done.stdout
    frames = [int(line.split()[1]) for line in report.read_text().splitlines()]
    first, second = frames.count(0), frames.count(1)
    assert 7 * first < second < 9 * first, (first, second)
    alone = race(None, None, "--speed-from-handoff", "2")
    assert alone.returncode == 2 and "--cpu-speed" in alone.stderr


def test_picture_and_sources_are_written_for_the_asked_frame(race):
    tmp = race.tmp_path
    done = race(
        None, None,
        "--picture-at", f"0:{tmp / 'p.ppm'}", "--sources-at", f"0:{tmp / 's.bin'}",
        "--line-state-at", f"0:{tmp / 'l.txt'}", "--video-state-at", f"0:{tmp / 'v.bin'}",
    )  # fmt: skip
    assert done.returncode == 0, done.stdout + done.stderr
    assert "RESULT match" in done.stdout
    assert (tmp / "p.ppm").read_bytes().startswith(b"P6\n320 224\n255\n")
    assert (tmp / "p.ppm").stat().st_size == 15 + 320 * 224 * 3
    assert (tmp / "s.bin").stat().st_size == 320 * 224 * 4
    lines = (tmp / "l.txt").read_text().splitlines()
    assert len(lines) == 224 and len(lines[223].split()) == 1 + 24 + 4
    state = (tmp / "v.bin").read_bytes()
    assert state.startswith(b"RR2VDPSTATE1\n") and len(state) == 13 + 24 + 65536 + 128 + 80


def test_a_picture_for_a_frame_the_run_never_reaches_is_an_error(race):
    done = race(None, None, "--picture-at", f"100000:{race.tmp_path / 'p.ppm'}")
    assert done.returncode == 2
    assert "frame 100000" in done.stderr


def test_video_log_lists_each_write_with_the_code_that_made_it(race):
    log = race.tmp_path / "video.txt"
    done = race(None, None, "--video-log", f"1:3:{log}", code=CODE_WITH_VIDEO_WRITE)
    assert done.returncode == 0, done.stdout + done.stderr
    lines = [line.split() for line in log.read_text().splitlines()]
    # Passes 1 and 2 each end with one register write, made by the instruction at 0x232.
    assert [line[0] for line in lines] == ["1", "2"]
    handoff, frame, line, pc, level, kind, target, value, source = lines[0]
    assert (pc, level, kind, target, value, source) == ("000232", "7", "reg", "000F", "0002", "-")
    assert int(frame) >= 0 and 0 <= int(line) < 262


def test_logs_do_not_change_the_race(race):
    tmp = race.tmp_path
    plain = race(None, None, "--report", str(tmp / "plain.txt"), code=CODE_WITH_VIDEO_WRITE)
    logged = race(
        None, None, "--report", str(tmp / "logged.txt"), "--video-log", f"0:3:{tmp / 'v.txt'}",
        "--sources-at", f"0:{tmp / 's.bin'}", "--line-state-at", f"0:{tmp / 'l.txt'}",
        code=CODE_WITH_VIDEO_WRITE,
    )  # fmt: skip
    assert plain.returncode == 0 and logged.returncode == 0, logged.stdout + logged.stderr
    assert logged.stdout == plain.stdout
    assert (tmp / "logged.txt").read_text() == (tmp / "plain.txt").read_text()


def test_ram_writes_lists_writes_to_a_range_with_the_writing_instruction(race):
    log = race.tmp_path / "writes.txt"
    done = race(None, None, "--ram-writes", f"1:3:062A:062A:{log}")
    assert done.returncode == 0, done.stdout + done.stderr
    lines = [line.split() for line in log.read_text().splitlines()]
    # Passes 1 and 2 each store the pad byte once, from the instruction at 0x22C.
    assert [line[0] for line in lines] == ["1", "2"]
    handoff, frame, pc, address, size, value = lines[0]
    assert (pc, address, size, value) == ("00022C", "FF062A", "1", f"{PADS[1]:02X}")


def test_frame_files_are_refused_with_resync_because_frames_are_cut_short(race):
    # --resync restarts the clock at every handoff, so a "frame" is no longer a whole picture.
    for option in ("--picture-at", "--sources-at", "--line-state-at", "--video-state-at"):
        done = race(None, None, "--resync", option, f"0:{race.tmp_path / 'x'}")
        assert done.returncode == 2, option
        assert "--resync" in done.stderr and option in done.stderr


def test_ram_at_pc_takes_an_image_each_time_the_instruction_runs(race):
    from rr2port.handoffs import iter_handoffs

    out = race.tmp_path / "at-pc.bin"
    # 0x220 runs once a pass, just after the pass counter at offset 0 has been raised.
    done = race(None, None, "--ram-at-pc", f"220:1:3:{out}")
    assert done.returncode == 0, done.stdout + done.stderr
    images = list(iter_handoffs(out))
    assert [index for index, _ in images] == [0, 1]
    assert [ram[0] for _, ram in images] == [2, 3]  # passes 1 and 2, counter already raised
    index = [line.split() for line in (race.tmp_path / "at-pc.bin.index").read_text().splitlines()]
    assert [line[:2] for line in index] == [["0", "1"], ["1", "2"]]  # image number, pass


def test_ram_at_pc_is_refused_with_resync(race):
    done = race(None, None, "--resync", "--ram-at-pc", f"220:0:3:{race.tmp_path / 'x'}")
    assert done.returncode == 2
    assert "--resync" in done.stderr and "--ram-at-pc" in done.stderr


def test_scaled_check_reports_counts(race):
    out = race.tmp_path / "big.ppm"
    done = race(None, None, "--scaled-check", "4", "--scaled-at", f"0:{out}")
    assert done.returncode == 0, done.stdout + done.stderr
    line = next(line for line in done.stdout.splitlines() if line.startswith("SCALED "))
    fields = dict(field.split("=") for field in line.split()[1:])
    # The synthetic program is not a race: every frame is the plain enlargement.
    assert fields["scale"] == "4" and int(fields["frames"]) > 0
    assert fields["sampled_back_identical"] == fields["frames"]
    assert (fields["crisp_frames"], fields["race_frames"]) == ("0", "0")
    assert out.read_bytes().startswith(b"P6\n1280 896\n255\n")
    assert out.stat().st_size == 16 + 1280 * 896 * 3


def test_scaled_check_does_not_change_the_race(race):
    tmp = race.tmp_path
    plain = race(None, None, "--report", str(tmp / "plain.txt"))
    scaled = race(None, None, "--report", str(tmp / "scaled.txt"), "--scaled-check", "4")
    assert plain.returncode == 0 and scaled.returncode == 0, scaled.stdout + scaled.stderr
    kept = [line for line in scaled.stdout.splitlines() if not line.startswith("SCALED ")]
    assert kept == plain.stdout.splitlines()
    assert (tmp / "scaled.txt").read_text() == (tmp / "plain.txt").read_text()


def test_ground_check_prints_its_line_and_does_not_change_the_race(race):
    tmp = race.tmp_path
    plain = race(None, None, "--report", str(tmp / "plain.txt"), "--scaled-check", "2")
    report = str(tmp / "ground.txt")
    checked = race(None, None, "--report", report, "--scaled-check", "2", "--ground-check")
    assert plain.returncode == 0 and checked.returncode == 0, checked.stdout + checked.stderr
    line = next(line for line in checked.stdout.splitlines() if line.startswith("GROUND "))
    fields = dict(field.split("=") for field in line.split()[1:])
    assert list(fields) == [
        "ground_lines", "block_rows", "full_rows", "empty_rows", "other_ground_rows",
        "sky_lines", "sky_rows_passing", "mark_max", "period_max",
        "ground_lines_sky_backdrop", "sky_rows_passing_road_backdrop",
    ]  # fmt: skip
    # The synthetic program is not a race: no frame has a road, so no line is counted.
    assert set(fields.values()) == {"0"}

    def kept(out):
        return [x for x in out.splitlines() if not x.startswith(("GROUND ", "SCALED "))]

    assert kept(checked.stdout) == kept(plain.stdout)
    assert (tmp / "ground.txt").read_text() == (tmp / "plain.txt").read_text()


def test_ground_check_needs_scaled_check(race):
    alone = race(None, None, "--ground-check")
    assert alone.returncode == 2 and "--scaled-check" in alone.stderr


def test_wide_needs_scaled_check_and_fine_objects(race):
    alone = race(None, None, "--wide", "39")
    assert alone.returncode == 2 and "--scaled-check" in alone.stderr
    no_objects = race(None, None, "--scaled-check", "3", "--wide", "39")
    assert no_objects.returncode == 2 and "--fine-objects" in no_objects.stderr
    for bad in ("0", "65", "x"):
        refused = race(None, None, "--scaled-check", "3", "--fine-objects", "--wide", bad)
        assert refused.returncode == 2 and "--wide" in refused.stderr


def test_wide_prints_its_lines_and_does_not_change_the_race(race):
    tmp = race.tmp_path
    common = ("--scaled-check", "3", "--fine-objects")
    plain = race(None, None, "--report", str(tmp / "plain.txt"), *common)
    wide = race(None, None, "--report", str(tmp / "wide.txt"), *common, "--wide", "39")
    assert plain.returncode == 0 and wide.returncode == 0, wide.stdout + wide.stderr

    def fields(prefix):
        line = next(line for line in wide.stdout.splitlines() if line.startswith(prefix + " "))
        return dict(field.split("=") for field in line.split()[1:])

    centre = fields("WIDE_CENTRE")
    assert list(centre) == [
        "frames", "identical", "pixels", "differing_pixels", "road_line", "object", "dash",
        "other", "first_frame",
    ]  # fmt: skip
    # Every frame's middle is the narrow picture; the synthetic program is not a race.
    assert int(centre["frames"]) > 0 and centre["identical"] == centre["frames"]
    assert int(centre["pixels"]) == int(centre["frames"]) * 320 * 3 * 224 * 3
    assert centre["differing_pixels"] == "0" and centre["first_frame"] == "none"
    assert list(fields("WIDE_GROUND")) == [
        "examined", "grass", "mark", "road", "wrong", "short_lines", "short_updates",
        "beyond_lines", "no_edge_lines",
    ]  # fmt: skip
    sky = fields("WIDE_SKY")
    assert list(sky) == ["examined", "plane", "backdrop", "panel", "untracked_race_frames"]
    assert set(fields("WIDE_GROUND").values()) == {"0"} and set(sky.values()) == {"0"}
    objects = fields("WIDE_OBJECTS")
    assert list(objects) == [
        "frames", "margin_pictures", "uncut", "off_view", "copies", "left_out_room", "seam_pixels",
        "differing_outside_seam", "gaps",
    ]  # fmt: skip
    assert set(objects.values()) == {"0"}

    def kept(out):
        return [x for x in out.splitlines() if not x.startswith(("WIDE_", "SCALED "))]

    assert kept(wide.stdout) == kept(plain.stdout)
    assert (tmp / "wide.txt").read_text() == (tmp / "plain.txt").read_text()

    def counts(out, prefix):
        line = next(x for x in out.splitlines() if x.startswith(prefix + " "))
        return [f for f in line.split()[1:] if not f.startswith("run_ms_per_frame=")]

    # The checks of the narrow picture give the same counts with the wide one beside it.
    assert counts(wide.stdout, "SCALED") == counts(plain.stdout, "SCALED")


def test_scaled_at_writes_the_wide_picture(race):
    out = race.tmp_path / "wide.ppm"
    common = ("--scaled-check", "3", "--fine-objects", "--wide", "39")
    done = race(None, None, *common, "--scaled-at", f"0:{out}")
    assert done.returncode == 0, done.stdout + done.stderr
    assert out.read_bytes().startswith(b"P6\n1194 672\n255\n")
    assert out.stat().st_size == 16 + 1194 * 672 * 3
    # Not a race: the whole picture lies between black panels.
    data = out.read_bytes()[16:]
    assert data[:3] == b"\x00\x00\x00" and data[1193 * 3 : 1194 * 3] == b"\x00\x00\x00"


def test_scaled_at_needs_scaled_check_and_is_refused_with_resync(race):
    alone = race(None, None, "--scaled-at", f"0:{race.tmp_path / 'x'}")
    assert alone.returncode == 2 and "--scaled-check" in alone.stderr
    resynced = race(None, None, "--resync", "--scaled-check", "4")
    assert resynced.returncode == 2 and "--resync" in resynced.stderr


def test_scaled_check_reports_points_mode_and_edge_measures(race):
    for mode in ("fine", "whole"):
        done = race(None, None, "--scaled-check", "4", "--crisp-points", mode)
        assert done.returncode == 0, done.stdout + done.stderr
        line = next(line for line in done.stdout.splitlines() if line.startswith("SCALED "))
        fields = dict(field.split("=") for field in line.split()[1:])
        assert fields["points"] == mode
        # Not a race: no road, so nothing is drawn fine and nothing differs.
        assert fields["off_road_identical"] == fields["frames"] == fields["sampled_back_identical"]
        assert (fields["fine_frames"], fields["edge_lines"]) == ("0", "0")
        assert fields["within_1px"] == "0"
        assert fields["within_2px_and_a_line"] == "0" and fields["max_edge_difference"] == "0"
    default = race(None, None, "--scaled-check", "4")
    assert " points=fine " in default.stdout


def test_crisp_points_needs_scaled_check_and_a_known_value(race):
    alone = race(None, None, "--crisp-points", "fine")
    assert alone.returncode == 2 and "--scaled-check" in alone.stderr
    odd = race(None, None, "--scaled-check", "4", "--crisp-points", "sharp")
    assert odd.returncode == 2 and "whole, fine or none" in odd.stderr


def test_edge_log_options_need_each_other_and_a_sensible_threshold(race):
    log = race.tmp_path / "edges.txt"
    done = race(None, None, "--scaled-check", "4", "--edge-log", str(log), "--edge-log-over", "4")
    assert done.returncode == 0, done.stdout + done.stderr
    assert log.read_text() == ""  # not a race: no edges at all
    alone = race(None, None, "--scaled-check", "4", "--edge-log-over", "4")
    assert alone.returncode == 2 and "--edge-log" in alone.stderr
    odd = race(None, None, "--scaled-check", "4", "--edge-log", str(log), "--edge-log-over", "x")
    assert odd.returncode == 2 and "--edge-log-over" in odd.stderr
    refused = race(None, None, "--edge-log", str(race.tmp_path / "kept.txt"))
    assert refused.returncode == 2 and not (race.tmp_path / "kept.txt").exists()


def test_objects_check_counts_passes_and_names_the_first_that_differs(race):
    # The synthetic program draws nothing and its ROM has no picture table, so
    # the model covers none of its passes: each counts as differing.
    done = race(None, None, "--objects-check")
    assert done.returncode == 0, done.stdout + done.stderr
    line = next(line for line in done.stdout.splitlines() if line.startswith("OBJECTS "))
    fields = dict(field.split("=") for field in line.split()[1:])
    assert int(fields["passes"]) >= 3 and fields["identical"] == "0"
    assert fields["first_difference"] == "0"
    assert {"records", "dropped", "pictures"} <= set(fields)
    plain = race(None, None)
    assert [x for x in done.stdout.splitlines() if not x.startswith("OBJECTS ")] == (
        plain.stdout.splitlines()
    )


def test_objects_check_is_refused_with_resync(race):
    done = race(None, None, "--resync", "--objects-check")
    assert done.returncode == 2 and "--resync" in done.stderr and "--objects-check" in done.stderr


def test_fine_objects_needs_scaled_check(race):
    done = race(None, None, "--fine-objects")
    assert done.returncode == 2 and "--scaled-check" in done.stderr


def test_fine_objects_prints_counts_and_does_not_change_the_race(race):
    done = race(None, None, "--scaled-check", "3", "--fine-objects")
    assert done.returncode == 0, done.stdout + done.stderr
    line = next(line for line in done.stdout.splitlines() if line.startswith("FINE_OBJECTS "))
    fields = dict(field.split("=") for field in line.split()[1:])
    # The synthetic program never draws an object: no set is noted, every frame is plain.
    assert int(fields["frames"]) > 0
    assert (fields["set_frames"], fields["fine_frames"], fields["stale_lines"]) == ("0", "0", "0")
    assert {"entries", "fine_entries", "covered_pixels", "same_at_centre"} <= set(fields)
    scaled = next(line for line in done.stdout.splitlines() if line.startswith("SCALED "))
    assert f"sampled_back_identical={fields['frames']}" in scaled
    plain = race(None, None)
    kept = [x for x in done.stdout.splitlines() if not x.startswith(("SCALED ", "FINE_OBJECTS ", "FINE_DASHES "))]
    assert kept == plain.stdout.splitlines()


def test_crisp_points_none_draws_no_road(race):
    done = race(None, None, "--scaled-check", "3", "--crisp-points", "none")
    assert done.returncode == 0, done.stdout + done.stderr
    line = next(line for line in done.stdout.splitlines() if line.startswith("SCALED "))
    fields = dict(field.split("=") for field in line.split()[1:])
    assert fields["points"] == "none" and fields["crisp_frames"] == "0"
    assert fields["off_road_identical"] == fields["frames"]


def test_inbetween_needs_the_fine_road_and_fine_objects(race):
    bare = race(None, None, "--inbetween", "blend")
    assert bare.returncode == 2 and "--scaled-check" in bare.stderr
    no_objects = race(None, None, "--scaled-check", "3", "--inbetween", "blend")
    assert no_objects.returncode == 2 and "--fine-objects" in no_objects.stderr
    whole = race(None, None, "--scaled-check", "3", "--fine-objects", "--crisp-points", "whole", "--inbetween", "blend")
    assert whole.returncode == 2 and "--crisp-points fine" in whole.stderr


def test_inbetween_rejects_an_unknown_mode(race):
    done = race(None, None, "--scaled-check", "3", "--fine-objects", "--inbetween", "sideways")
    assert done.returncode == 2 and "predict, blend or late" in done.stderr


def test_inbetween_check_needs_inbetween(race):
    done = race(None, None, "--scaled-check", "3", "--fine-objects", "--inbetween-check")
    assert done.returncode == 2 and "--inbetween" in done.stderr


def test_inbetween_is_refused_with_resync(race):
    done = race(None, None, "--resync", "--scaled-check", "3", "--fine-objects", "--inbetween", "blend")
    assert done.returncode == 2 and "--resync" in done.stderr


def _fields(output: str, name: str) -> dict[str, str]:
    line = next(line for line in output.splitlines() if line.startswith(name + " "))
    return dict(field.split("=") for field in line.split()[1:])


def test_inbetween_reports_counts_and_does_not_change_the_race(race):
    for mode in ("predict", "blend", "late"):
        done = race(None, None, "--scaled-check", "3", "--fine-objects", "--inbetween", mode, "--inbetween-check")
        assert done.returncode == 0, done.stdout + done.stderr
        fields = _fields(done.stdout, "INBETWEEN")
        # The synthetic program is not a race: no update is ever noted, nothing is drawn in between.
        assert fields["mode"] == mode
        assert (fields["between"], fields["guessed"], fields["stale_lines"]) == ("0", "0", "0")
        assert {"frames", "no_pair", "road", "objects", "table"} <= set(fields)
        assert _fields(done.stdout, "INBETWEEN_ENDS")["updates"] == "0"
        assert _fields(done.stdout, "INBETWEEN_SKIP_ONE")["triples"] == "0"
        assert "edge_still" in _fields(done.stdout, "INBETWEEN_MOTION")
        assert _fields(done.stdout, "INBETWEEN_GUESS")["updates"] == "0"
        plain = race(None, None)
        kept = [x for x in done.stdout.splitlines() if not x.startswith(("SCALED ", "FINE_OBJECTS ", "FINE_DASHES ", "INBETWEEN"))]
        assert kept == plain.stdout.splitlines()


def test_inbetween_without_the_check_prints_only_the_counts(race):
    done = race(None, None, "--scaled-check", "3", "--fine-objects", "--inbetween", "blend")
    assert done.returncode == 0, done.stdout + done.stderr
    assert "INBETWEEN mode=blend" in done.stdout and "INBETWEEN_ENDS" not in done.stdout


def test_inbetween_leaves_a_frame_it_cannot_make_as_it_was(race):
    tmp = race.tmp_path
    plain = race(None, None, "--scaled-check", "3", "--fine-objects", "--scaled-at", f"0:{tmp / 'plain.ppm'}")
    between = race(None, None, "--scaled-check", "3", "--fine-objects", "--inbetween", "predict",
                   "--scaled-at", f"0:{tmp / 'between.ppm'}")
    assert plain.returncode == 0 and between.returncode == 0, between.stdout + between.stderr
    assert (tmp / "between.ppm").read_bytes() == (tmp / "plain.ppm").read_bytes()


def test_dashes_check_prints_counts_and_does_not_change_the_race(race):
    done = race(None, None, "--dashes-check")
    assert done.returncode == 0, done.stdout + done.stderr
    fields = _fields(done.stdout, "DASHES")
    # The synthetic program passes the place the test build takes for the dash
    # routine, but it is not a race: no pass is covered by the model.
    assert int(fields["passes"]) >= 3 and fields["covered"] == "0" and fields["identical"] == "0"
    assert fields["first_difference"] == "none"
    assert {"covered", "identical", "sprites_after_objects", "dash_sprites"} <= set(fields)
    plain = race(None, None)
    assert [x for x in done.stdout.splitlines() if not x.startswith("DASHES ")] == plain.stdout.splitlines()


def test_dashes_check_is_refused_with_resync(race):
    done = race(None, None, "--resync", "--dashes-check")
    assert done.returncode == 2 and "--resync" in done.stderr and "--dashes-check" in done.stderr


def test_fine_objects_reports_how_the_dashes_were_drawn(race):
    done = race(None, None, "--scaled-check", "3", "--fine-objects")
    assert done.returncode == 0, done.stdout + done.stderr
    fields = _fields(done.stdout, "FINE_DASHES")
    # The synthetic program has no road: no frame is drawn with the crisp road, none with strips.
    assert (fields["frames"], fields["strip_frames"], fields["sprite_frames"]) == ("0", "0", "0")
    between = race(None, None, "--scaled-check", "3", "--fine-objects", "--inbetween", "blend", "--inbetween-check")
    assert between.returncode == 0, between.stdout + between.stderr
    assert _fields(between.stdout, "FINE_DASHES")["strip_frames"] == "0"
    assert {"dash_still", "dash_moving"} <= set(_fields(between.stdout, "INBETWEEN_MOTION"))


def test_soft_objects_needs_fine_objects_and_does_not_change_the_race(race):
    alone = race(None, None, "--scaled-check", "3", "--soft-objects")
    assert alone.returncode == 2 and "--fine-objects" in alone.stderr
    done = race(None, None, "--scaled-check", "3", "--fine-objects", "--soft-objects")
    assert done.returncode == 0, done.stdout + done.stderr
    plain = race(None, None)
    kept = [x for x in done.stdout.splitlines() if not x.startswith(("SCALED ", "FINE_OBJECTS ", "FINE_DASHES "))]
    assert kept == plain.stdout.splitlines()


def test_sound_options_write_a_wav_and_a_log_and_do_not_change_the_race(race):
    import struct

    wav = race.tmp_path / "sound.wav"
    log = race.tmp_path / "fm.log"
    plain = race()
    done = race(None, None, "--sound-wav", str(wav), "--fm-log", str(log))
    assert done.returncode == 0, done.stdout + done.stderr
    assert done.stdout.rstrip().endswith("RESULT match")
    lines = [line for line in done.stdout.splitlines() if line.startswith("SOUND ")]
    assert len(lines) == 1
    fields = dict(part.split("=") for part in lines[0].split()[1:])
    assert set(fields) == {
        "z80_steps", "fm_writes", "fm_writes_68000", "psg_writes_z80", "psg_writes_68000",
        "z80_ram_writes_68000", "samples",
    }  # fmt: skip
    # Everything but the SOUND line is as without the options.
    others = [line for line in done.stdout.splitlines() if line not in lines]
    assert others == plain.stdout.splitlines()
    data = wav.read_bytes()
    assert data[:4] == b"RIFF" and data[8:16] == b"WAVEfmt " and data[36:40] == b"data"
    fmt, channels, rate, _, _, bits = struct.unpack("<HHIIHH", data[20:36])
    assert (fmt, channels, rate, bits) == (1, 2, 53267, 16)
    samples = int(fields["samples"])
    assert struct.unpack("<I", data[40:44])[0] == 4 * samples == len(data) - 44
    assert struct.unpack("<I", data[4:8])[0] == len(data) - 8
    # The sound side runs for as long as the frames did: about 889 samples a frame.
    assert samples > 0
    # The tiny test game never starts the Z80, so nothing is played: the log
    # holds only the time the run ended, which is the time the samples cover.
    assert fields["z80_steps"] == "0"
    end = log.read_text().split()
    assert end[0] == "END" and len(end) == 2
    assert abs(int(end[1]) // 1008 - samples) <= 1


def test_without_sound_options_there_is_no_sound_line(race):
    assert "SOUND " not in race().stdout


def test_sound_options_are_refused_with_resync(race):
    done = race(None, None, "--resync", "--fm-log", str(race.tmp_path / "fm.log"))
    assert done.returncode == 2
    assert "--resync" in done.stderr
