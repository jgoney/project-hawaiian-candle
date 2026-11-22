"""rr2-fm-compare on hand-written FM write logs."""

from rr2port.fm_compare import MASTER_HZ, compare, main, read_mame_log, read_port_log

# Two register writes on part 1 and one on part 2: address then data each time.
PORT = "1000 0 28\n2000 1 F0\n3000 0 A4\n4000 1 22\n5000 2 B4\n6000 3 C0\n"


def mame_from_port(text: str, shift_master: int = 0) -> str:
    lines = []
    for line in text.splitlines():
        master, port, value = line.split()
        lines.append(f"{(int(master) - shift_master) / MASTER_HZ:.9f} {port} {value}\n")
    return "".join(lines)


def run(tmp_path, port_text, mame_text):
    port, mame = tmp_path / "port.log", tmp_path / "mame.log"
    port.write_text(port_text)
    mame.write_text(mame_text)
    lines: list[str] = []
    status = main([str(port), str(mame)], out=lines.append)
    return status, lines


def test_events_pair_address_and_data_by_part(tmp_path):
    path = tmp_path / "p.log"
    path.write_text(PORT)
    events = read_port_log(path)
    assert [(e.part, e.register, e.value) for e in events] == [
        (0, 0x28, 0xF0), (0, 0xA4, 0x22), (1, 0xB4, 0xC0),
    ]  # fmt: skip
    assert abs(events[0].seconds - 2000 / MASTER_HZ) < 1e-12


def test_identical_logs_are_the_same(tmp_path):
    status, lines = run(tmp_path, PORT, mame_from_port(PORT))
    assert status == 0
    assert "EVENTS port=3 mame=3" in lines
    assert "SAME_IN_ORDER 3" in lines
    assert "FIRST_DIFFERENCE none" in lines
    assert lines[-1] == "RESULT same"


def test_a_changed_value_is_the_first_difference(tmp_path):
    changed = mame_from_port(PORT.replace("4000 1 22", "4000 1 23"))
    status, lines = run(tmp_path, PORT, changed)
    assert status == 1
    assert "SAME_IN_ORDER 1" in lines
    first = next(line for line in lines if line.startswith("FIRST_DIFFERENCE"))
    assert first == "FIRST_DIFFERENCE index=1 port=part0:A4=22 mame=part0:A4=23"
    # The same registers were written the same number of times: only a value differs.
    assert not any(line.startswith("REGISTERS_DIFFERING") for line in lines)
    assert lines[-1] == "RESULT different"


def test_registers_written_a_different_number_of_times_are_listed(tmp_path):
    more = PORT + "7000 0 2A\n8000 1 80\n"
    status, lines = run(tmp_path, more, mame_from_port(PORT))
    assert status == 1
    assert "REGISTERS_DIFFERING (port/mame writes) part0:2A:1/0" in lines


def test_a_shorter_log_is_different(tmp_path):
    short = "".join(mame_from_port(PORT).splitlines(keepends=True)[:4])
    status, lines = run(tmp_path, PORT, short)
    assert status == 1
    assert "EVENTS port=3 mame=2" in lines
    assert "SAME_IN_ORDER 2" in lines
    assert "FIRST_DIFFERENCE index=2 port=part1:B4=C0 mame=end" in lines


def test_an_empty_log_is_reported_not_a_crash(tmp_path):
    status, lines = run(tmp_path, "", mame_from_port(PORT))
    assert status == 1
    assert "EVENTS port=0 mame=3" in lines
    assert lines[-1] == "RESULT different"


def test_data_without_an_address_counts_as_register_none(tmp_path):
    path = tmp_path / "p.log"
    path.write_text("1000 1 55\n")
    events = read_port_log(path)
    assert len(events) == 1 and events[0].register is None and events[0].value == 0x55
    status, lines = run(tmp_path, "1000 1 55\n", mame_from_port("1000 1 55\n"))
    assert status == 0


def test_timing_is_reported_in_samples(tmp_path):
    # The port's writes are one sample (1,008 master clocks) later than the original's.
    status, lines = run(tmp_path, PORT, mame_from_port(PORT, shift_master=1008))
    assert status == 0
    timing = next(line for line in lines if line.startswith("TIMING"))
    assert timing.startswith("TIMING samples (port later is positive): median=1.0 ")


def test_a_bad_line_is_an_input_error(tmp_path):
    status, lines = run(tmp_path, "what\n", "")
    assert status == 2


def test_comparing_nothing_with_nothing_finds_nothing():
    assert compare([], []).same_in_order == 0


def test_mame_log_reader(tmp_path):
    path = tmp_path / "m.log"
    path.write_text("0.000100000 0 28\n0.000200000 1 F0\n")
    events = read_mame_log(path)
    assert [(e.part, e.register, e.value, e.seconds) for e in events] == [(0, 0x28, 0xF0, 0.0002)]


# ---- the two streams: music (every register but the sample register 2A) and samples ----


def stream_log(samples: list[int], music_every: int = 4, music_at: int = 0) -> str:
    """A log that plays `samples` through register 2A with a key-on now and then."""
    lines, t, keys = [], 1000, 0
    for i, value in enumerate(samples):
        if i % music_every == music_at:
            lines += [f"{t} 0 28", f"{t + 100} 1 {keys % 7:02X}"]
            keys += 1
        lines += [f"{t + 200} 0 2A", f"{t + 300} 1 {value:02X}"]
        t += 1000
    return "\n".join(lines) + "\n"


SAMPLES = [(i * 37 + 11) % 251 for i in range(400)]


def test_streams_are_reported_separately_when_only_their_interleaving_differs(tmp_path):
    # The same music and the same samples, but the key-ons fall between other samples.
    theirs = mame_from_port(stream_log(SAMPLES, 4, music_at=2))
    status, lines = run(tmp_path, stream_log(SAMPLES, 4), theirs)
    assert status == 1
    assert "SAME_IN_ORDER 0" in lines
    music = next(line for line in lines if line.startswith("MUSIC"))
    assert music == "MUSIC port=100 mame=100 same_in_order=100 first_difference=none"
    assert lines[-1] == "RESULT music-same"
    samples = next(line for line in lines if line.startswith("SAMPLES"))
    assert samples == "SAMPLES port=400 mame=400 same=400 slips=0 port_extra=0 mame_extra=0"


def test_a_dropped_and_a_doubled_sample_are_counted_as_slips(tmp_path):
    theirs = SAMPLES[:100] + SAMPLES[101:250] + [SAMPLES[250]] + SAMPLES[250:]
    status, lines = run(tmp_path, stream_log(SAMPLES, 400), mame_from_port(stream_log(theirs, 400)))
    assert status == 1
    samples = next(line for line in lines if line.startswith("SAMPLES"))
    assert samples == "SAMPLES port=400 mame=400 same=399 slips=2 port_extra=1 mame_extra=1"
    assert sum(line.startswith("SLIP ") for line in lines) == 2
    assert lines[-1] == "RESULT music-same"


def test_music_that_differs_is_not_music_same(tmp_path):
    ours = stream_log(SAMPLES, 4)
    theirs = mame_from_port(ours.replace(" 1 03\n", " 1 04\n", 1))
    status, lines = run(tmp_path, ours, theirs)
    assert status == 1
    assert lines[-1] == "RESULT different"
    music = next(line for line in lines if line.startswith("MUSIC"))
    assert "first_difference=index" in music


def test_same_music_with_sample_slips_is_called_music_same(tmp_path):
    theirs = SAMPLES[:100] + SAMPLES[101:]
    status, lines = run(tmp_path, stream_log(SAMPLES, 400), mame_from_port(stream_log(theirs, 400)))
    assert status == 1
    assert lines[-1] == "RESULT music-same"


def test_what_the_original_wrote_after_the_ports_run_ended_is_set_aside(tmp_path):
    # The port's log says when its run ended; the original went on a little longer.
    ours = stream_log(SAMPLES, 1000)
    longer = stream_log(SAMPLES + [1, 2, 3], 1000)
    status, lines = run(tmp_path, ours + "END 401000\n", mame_from_port(longer))
    assert "AFTER_PORT_END mame=3" in lines
    assert status == 0 and lines[-1] == "RESULT same"


def test_without_an_end_line_nothing_is_set_aside(tmp_path):
    longer = stream_log(SAMPLES + [1, 2, 3], 1000)
    status, lines = run(tmp_path, stream_log(SAMPLES, 1000), mame_from_port(longer))
    assert status == 1
    assert not any(line.startswith("AFTER_PORT_END") for line in lines)
    assert "PORT_END unknown: the port's log has no END line" in lines


def test_a_port_whose_sound_stops_early_is_different(tmp_path):
    # The port wrote only the first quarter, but its run went on to the end.
    whole = stream_log(SAMPLES, 4)
    quarter = "".join(whole.splitlines(keepends=True)[:250])
    status, lines = run(tmp_path, quarter + "END 401000\n", mame_from_port(whole))
    assert status == 1
    assert lines[-1] == "RESULT different"
    assert not any(line.startswith("AFTER_PORT_END") for line in lines)


def test_writes_are_counted_by_which_processor_made_them(tmp_path):
    ours = "1000 0 28 z\n2000 1 F0 z\n3000 0 A4 m\n4000 1 22 m\nEND 5000\n"
    theirs = mame_from_port("1000 0 28\n2000 1 F0\n3000 0 A4\n4000 1 22\n")
    theirs = "".join(
        line + (" z\n" if i < 2 else " m\n") for i, line in enumerate(theirs.splitlines())
    )
    status, lines = run(tmp_path, ours, theirs)
    assert status == 0
    assert "FROM_68000 port=1 mame=1" in lines


def test_logs_without_a_source_column_say_so(tmp_path):
    status, lines = run(tmp_path, PORT + "END 7000\n", mame_from_port(PORT))
    assert "FROM_68000 port=unknown mame=unknown" in lines


def test_streams_that_never_line_up_again_are_reported(tmp_path):
    other = [(i * 91 + 5) % 239 for i in range(400)]
    status, lines = run(tmp_path, stream_log(SAMPLES, 400), mame_from_port(stream_log(other, 400)))
    assert status == 1
    samples = next(line for line in lines if line.startswith("SAMPLES"))
    assert "lost_at=" in samples
