"""Command-line entry points for the port translator."""

from __future__ import annotations

import argparse
import sys
import zlib
from collections.abc import Sequence
from pathlib import Path

from rr2port.discover import (
    AnnotationError,
    Annotations,
    WrongRomError,
    discover,
    load_annotations,
    trace_entries,
    verify_rr206,
)
from rr2port.emit import emit_program


def _fail(message: str) -> int:
    print(f"error: {message}", file=sys.stderr)
    return 2


def translate_main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="rr2-translate", description="Translate RR206 68000 code to C."
    )
    parser.add_argument("rom", type=Path)
    parser.add_argument("--annotations", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    args = parser.parse_args(argv)

    # Generated C is derived from the ROM and must stay out of git.
    if "generated" not in args.output.resolve().parts:
        return _fail(f"output must be under a generated/ directory: {args.output}")
    try:
        rom = args.rom.read_bytes()
    except OSError as error:
        return _fail(f"cannot read ROM {args.rom}: {error.strerror}")
    try:
        verify_rr206(rom)
    except WrongRomError as error:
        return _fail(str(error))

    try:
        annotations = load_annotations(args.annotations) if args.annotations else Annotations()
    except AnnotationError as error:
        return _fail(str(error))
    entries = set(annotations.entries)
    try:
        insns = discover(
            rom,
            sorted(entries),
            annotations.jump_tables,
            callback_slots=annotations.callback_slots,
            callback_registrars=annotations.callback_registrars,
            pointer_tables=annotations.pointer_tables,
        )
    except AnnotationError as error:
        return _fail(str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(emit_program(insns, zlib.crc32(rom)))
    print(f"translated {len(insns)} instructions to {args.output}")
    return 0


def _read_costs(path: Path):
    with path.open() as handle:
        for line in handle:
            fields = line.split()
            yield (int(fields[0]), int(fields[1], 16), *(int(value) for value in fields[2:]))


def report_main(argv: Sequence[str] | None = None) -> int:
    from rr2port.decode import DecodeError, decode
    from rr2port.report import build_report, format_report
    from rr2port.trace import read_trace

    parser = argparse.ArgumentParser(
        prog="rr2-trace-report",
        description="Compare translated cycle costs and interrupt timing with a MAME trace.",
    )
    parser.add_argument("window", type=Path)
    parser.add_argument("--ours", type=Path, required=True, help="rr-trace-check --cycles-out")
    parser.add_argument("--rom", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        rom = args.rom.read_bytes()
        verify_rr206(rom)
    except OSError as error:
        return _fail(f"cannot read ROM {args.rom}: {error.strerror}")
    except WrongRomError as error:
        return _fail(str(error))

    names: dict[int, str] = {}

    def mnemonic_of(pc: int) -> str:
        if pc not in names:
            try:
                names[pc] = decode(rom, pc).mnemonic
            except DecodeError:
                names[pc] = "?"
        return names[pc]

    report = build_report(
        read_trace(args.window / "trace.txt"), _read_costs(args.ours), mnemonic_of
    )
    sys.stdout.write(format_report(report))
    return 0


def code_report_main(argv: Sequence[str] | None = None) -> int:
    from rr2port.code_report import build_code_report, format_code_report

    parser = argparse.ArgumentParser(
        prog="rr2-code-report",
        description="Report discovered code and unresolved indirect jumps.",
    )
    parser.add_argument("rom", type=Path)
    parser.add_argument("--annotations", type=Path, required=True)
    parser.add_argument(
        "--window", type=Path, action="append", default=[], help="trace window to check against"
    )
    args = parser.parse_args(argv)
    try:
        rom = args.rom.read_bytes()
        verify_rr206(rom)
        annotations = load_annotations(args.annotations)
        windows = {path.name: trace_entries(path / "trace.txt") for path in args.window}
        report = build_code_report(rom, annotations, windows)
    except OSError as error:
        return _fail(f"cannot read {error.filename}: {error.strerror}")
    except (WrongRomError, AnnotationError) as error:
        return _fail(str(error))
    sys.stdout.write(format_code_report(report))
    return 0 if report.complete else 1


def annotate_indirect_main(argv: Sequence[str] | None = None) -> int:
    from rr2port.indirect import merge_jump_tables
    from rr2port.trace import TraceFormatError

    parser = argparse.ArgumentParser(
        prog="rr2-annotate-indirect",
        description="Merge indirect jump targets recorded by MAME into the annotation file.",
    )
    parser.add_argument("annotations", type=Path)
    parser.add_argument("logs", type=Path, nargs="+", help="files of 'site target' hex pairs")
    parser.add_argument("--rom", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        rom = args.rom.read_bytes()
        verify_rr206(rom)
        before = load_annotations(args.annotations)
        observed: dict[int, set[int]] = {}
        for log in args.logs:
            for line_number, line in enumerate(log.read_text().splitlines(), 1):
                fields = line.split()
                if len(fields) != 2:
                    raise TraceFormatError(line_number, f"{log}: expected 'site target'")
                observed.setdefault(int(fields[0], 16), set()).add(int(fields[1], 16))
        text = merge_jump_tables(args.annotations.read_text(), observed, rom)
    except OSError as error:
        return _fail(f"cannot read {error.filename}: {error.strerror}")
    except (WrongRomError, AnnotationError, TraceFormatError, ValueError) as error:
        return _fail(str(error))
    # The merged file must still describe a consistent code map (for example, a
    # logged site must be a discovered indirect jump). Put the old text back if not.
    original = args.annotations.read_text()
    args.annotations.write_text(text)
    try:
        merged = load_annotations(args.annotations)
        discover(
            rom,
            merged.entries,
            merged.jump_tables,
            callback_slots=merged.callback_slots,
            callback_registrars=merged.callback_registrars,
            pointer_tables=merged.pointer_tables,
        )
    except AnnotationError as error:
        args.annotations.write_text(original)
        return _fail(str(error))
    added = 0
    for site in sorted(observed):
        new = observed[site] - set(before.jump_tables.get(site, ()))
        if new:
            added += len(new)
            print(f"{site:06X}: +{len(new)} targets")
    print(f"added {added} targets")
    return 0


def picture_report_main(argv: Sequence[str] | None = None) -> int:
    from rr2port.picture_report import (
        PictureFormatError,
        format_report,
        read_line_state,
        read_sources,
        read_video_state,
    )

    parser = argparse.ArgumentParser(
        prog="rr2-picture-report",
        description="Summarise one frame of the picture from rr-race-replay's -at files.",
    )
    parser.add_argument("--sources", type=Path, help="file written by --sources-at")
    parser.add_argument("--line-state", type=Path, help="file written by --line-state-at")
    parser.add_argument("--video-state", type=Path, help="file written by --video-state-at")
    args = parser.parse_args(argv)
    if args.sources is None and args.line_state is None and args.video_state is None:
        return _fail("give at least one of --sources, --line-state, --video-state")
    try:
        sources = None if args.sources is None else read_sources(args.sources)
        states = None if args.line_state is None else read_line_state(args.line_state)
        state = None if args.video_state is None else read_video_state(args.video_state)
    except OSError as error:
        return _fail(f"cannot read {error.filename}: {error.strerror}")
    except PictureFormatError as error:
        return _fail(str(error))
    sys.stdout.write(format_report(sources, states, state))
    return 0


def video_report_main(argv: Sequence[str] | None = None) -> int:
    from rr2port.picture_report import PictureFormatError, read_video_state
    from rr2port.video_report import VideoLogError, format_summary, read_video_log, summarise

    parser = argparse.ArgumentParser(
        prog="rr2-video-report",
        description="Summarise rr-race-replay's --video-log by writing instruction.",
    )
    parser.add_argument("log", type=Path, help="file written by --video-log")
    parser.add_argument(
        "--video-state",
        type=Path,
        required=True,
        help="a --video-state-at file, for table addresses",
    )
    parser.add_argument(
        "--round", type=lambda text: int(text, 0), default=1,
        help="group writers by address rounded down to a multiple of this (default 1: exact)",
    )  # fmt: skip
    args = parser.parse_args(argv)
    try:
        events = read_video_log(args.log)
        state = read_video_state(args.video_state)
    except OSError as error:
        return _fail(f"cannot read {error.filename}: {error.strerror}")
    except (VideoLogError, PictureFormatError) as error:
        return _fail(str(error))
    step = max(args.round, 1)
    rows = summarise(events, state, routine_of=lambda pc: pc - pc % step)
    sys.stdout.write(format_summary(rows, len({event.handoff for event in events})))
    return 0


def ram_writes_main(argv: Sequence[str] | None = None) -> int:
    from rr2port.ram_writes import RamWriteLogError, format_writers, read_ram_writes, writers

    parser = argparse.ArgumentParser(
        prog="rr2-ram-writes",
        description="List the instructions that wrote a range of work RAM (--ram-writes log).",
    )
    parser.add_argument("log", type=Path, help="file written by rr-race-replay --ram-writes")
    args = parser.parse_args(argv)
    try:
        found = writers(read_ram_writes(args.log))
    except OSError as error:
        return _fail(f"cannot read {error.filename}: {error.strerror}")
    except RamWriteLogError as error:
        return _fail(str(error))
    sys.stdout.write(format_writers(found))
    return 0


def scene_check_main(argv: Sequence[str] | None = None) -> int:
    from rr2port.handoffs import HandoffFormatError, iter_handoffs
    from rr2port.picture_report import PictureFormatError, read_video_state
    from rr2port.scene import NotARaceScene, load_tables
    from rr2port.scene_check import check_handoffs, find_pass_shown, format_result

    parser = argparse.ArgumentParser(
        prog="rr2-scene-check",
        description="Check the scene reader against the tables the game itself produced.",
    )
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--handoffs", type=Path, required=True, help="work RAM at every pass")
    parser.add_argument(
        "--video-state",
        type=Path,
        action="append",
        default=[],
        help="a --video-state-at file: report which pass's staged table it shows",
    )
    args = parser.parse_args(argv)
    try:
        rom = args.rom.read_bytes()
        verify_rr206(rom)
        tables = load_tables(rom)
        sys.stdout.write(format_result(check_handoffs(args.handoffs, tables)))
        states = [(path, read_video_state(path)) for path in args.video_state]
        shown: dict[Path, list[tuple[int, int]]] = {path: [] for path, _ in states}
        if states:
            for k, ram in iter_handoffs(args.handoffs):
                for path, state in states:
                    shown[path] += find_pass_shown(state, [(k, ram)])
        for path, _ in states:
            passes = " ".join(f"{k}{'ab'[copy]}" for k, copy in shown[path]) or "none"
            sys.stdout.write(f"SHOWN {path.name} staged_at_handoffs={passes}\n")
    except OSError as error:
        return _fail(f"cannot read {error.filename}: {error.strerror}")
    except (WrongRomError, HandoffFormatError, PictureFormatError, NotARaceScene) as error:
        return _fail(str(error))
    return 0


def object_check_main(argv: Sequence[str] | None = None) -> int:
    from rr2port.handoffs import HandoffFormatError
    from rr2port.objects_check import check_drawing, check_records, format_drawing, format_records

    parser = argparse.ArgumentParser(
        prog="rr2-object-check",
        description="Check the object reader against what the game drew and placed. The image "
        "files come from rr-race-replay --ram-at-pc.",
    )
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--before", type=Path, help="images taken at 0xA4EE")
    parser.add_argument("--after", type=Path, help="images taken at 0xA5C6")
    parser.add_argument("--every", type=int, default=1, help="check every Nth image")
    parser.add_argument("--records-before", type=Path, help="images taken at 0x16E8C")
    parser.add_argument("--records-after", type=Path, help="images taken at 0x17196")
    args = parser.parse_args(argv)
    drawing = args.before is not None and args.after is not None
    records = args.records_before is not None and args.records_after is not None
    if not drawing and not records:
        return _fail("give --before and --after, or --records-before and --records-after")
    try:
        rom = args.rom.read_bytes()
        verify_rr206(rom)
        if drawing:
            result = check_drawing(args.before, args.after, rom, max(args.every, 1))
            sys.stdout.write(format_drawing(result))
        if records:
            found = check_records(args.records_before, args.records_after, rom)
            sys.stdout.write(format_records(found))
        else:
            sys.stdout.write("ROADSIDE_RECORDS not attempted\nVEHICLE_RECORDS not attempted\n")
    except OSError as error:
        return _fail(f"cannot read {error.filename}: {error.strerror}")
    except (WrongRomError, HandoffFormatError) as error:
        return _fail(str(error))
    return 0


def extract_art_main(argv: Sequence[str] | None = None) -> int:
    from rr2port.objects import extract_art
    from rr2port.picture_report import PictureFormatError, read_video_state

    parser = argparse.ArgumentParser(
        prog="rr2-extract-art",
        description="Write every object picture in the ROM as a PNG, with one sheet of them all.",
    )
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--video-state", type=Path, required=True, help="a race frame: its colours")
    parser.add_argument("--out", type=Path, required=True, help="a directory under generated/")
    args = parser.parse_args(argv)
    try:
        rom = args.rom.read_bytes()
        verify_rr206(rom)
        count = extract_art(rom, read_video_state(args.video_state).cram, args.out)
    except OSError as error:
        return _fail(f"cannot read {error.filename}: {error.strerror}")
    except (WrongRomError, PictureFormatError, ValueError) as error:
        return _fail(str(error))
    sys.stdout.write(f"wrote {count} pictures and sheet.png to {args.out}\n")
    return 0
