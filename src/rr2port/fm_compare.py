"""Compare two logs of writes to the FM chip: the port's and the original's.

A log has one line per write: a time, the port (0 to 3), the value in hex
and, optionally, which processor wrote it (z: the Z80, m: the 68000). The
port's log ends with a line "END <time>" for when its run ended; what the
original wrote after that is set aside. Without that line nothing is set
aside, so a port whose sound stopped early cannot pass for a short run.
Ports 0 and 2 choose a register on part 1 and part 2 of the chip; ports 1
and 3 write the chosen register. The port's log (rr-race-replay --fm-log)
gives the time in master clocks since power-on; the original's
(scripts/run-mame-fm-log-rr206.sh) in seconds.

What is compared is the sequence of register writes: which part, which
register, which value, in which order. Their times are reported, not judged.

The writes are also compared as two streams, because how they interleave
depends on timing finer than a sample: the music (every register but 2A) and
the samples (register 2A on part 1, the chip's sample player). The sample
streams are lined up again after each place they differ, and those places
("slips") are counted. Whatever the original wrote after the port's run ended
is set aside first.
"""

from __future__ import annotations

import statistics
import sys
from collections import Counter
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

MASTER_HZ = 53_693_175
SAMPLE_HZ = MASTER_HZ / 7 / 144


@dataclass(frozen=True)
class Event:
    seconds: float
    part: int
    register: int | None  # None: data written before any register was chosen
    value: int
    from_68000: bool | None = None  # None: the log does not say

    def name(self) -> str:
        register = "none" if self.register is None else f"{self.register:02X}"
        return f"part{self.part}:{register}={self.value:02X}"

    def key(self) -> tuple[int, int | None, int]:
        return (self.part, self.register, self.value)


def _read(path: Path, seconds_of: Callable[[str], float]) -> tuple[list[Event], float | None]:
    """The register writes in a log, and the time on its END line if it has one."""
    chosen: dict[int, int | None] = {0: None, 1: None}
    events = []
    end = None
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if not line.strip():
            continue
        try:
            fields = line.split()
            if fields[0] == "END" and len(fields) == 2:
                end = seconds_of(fields[1])
                continue
            when, port_text, value_text = fields[:3]
            source = {"z": False, "m": True}[fields[3]] if len(fields) == 4 else None
            seconds, port, value = seconds_of(when), int(port_text), int(value_text, 16)
            if len(fields) > 4 or not 0 <= port <= 3 or not 0 <= value <= 0xFF:
                raise ValueError
        except (ValueError, KeyError, IndexError):
            raise ValueError(
                f"{path}: line {number} is not '<time> <port 0-3> <hex value> [z|m]'"
            ) from None
        part = port >> 1
        if port & 1:
            events.append(Event(seconds, part, chosen[part], value, source))
        else:
            chosen[part] = value
    return events, end


def read_port_log(path: Path) -> list[Event]:
    return _read(path, lambda text: int(text) / MASTER_HZ)[0]


def read_port_end(path: Path) -> float | None:
    """When the port's run ended, in seconds, or None if its log does not say."""
    return _read(path, lambda text: int(text) / MASTER_HZ)[1]


def read_mame_log(path: Path) -> list[Event]:
    return _read(path, float)[0]


def _from_68000(events: list[Event]) -> str:
    if any(e.from_68000 is None for e in events) or not events:
        return "unknown"
    return str(sum(1 for e in events if e.from_68000))


@dataclass(frozen=True)
class Comparison:
    same_in_order: int
    register_differences: dict[str, tuple[int, int]]  # register: (port count, original count)
    timing_samples: list[float]  # port minus original, over the common prefix


def compare(port: list[Event], mame: list[Event]) -> Comparison:
    same = 0
    while same < len(port) and same < len(mame) and port[same].key() == mame[same].key():
        same += 1
    ours = Counter((e.part, e.register) for e in port)
    theirs = Counter((e.part, e.register) for e in mame)
    differing = {}
    for part, register in sorted(
        set(ours) | set(theirs), key=lambda k: (k[0], -1 if k[1] is None else k[1])
    ):
        if ours[(part, register)] != theirs[(part, register)]:
            name = f"part{part}:" + ("none" if register is None else f"{register:02X}")
            differing[name] = (ours[(part, register)], theirs[(part, register)])
    timing = [(port[i].seconds - mame[i].seconds) * SAMPLE_HZ for i in range(same)]
    return Comparison(same, differing, timing)


SAMPLE_REGISTER = (0, 0x2A)
_WINDOW = 24  # values that must agree for two streams to count as lined up again
_REACH = 4000  # how far apart (in values, both sides together) a slip may put them


@dataclass(frozen=True)
class Slip:
    port_index: int
    seconds: float
    port_extra: tuple[int, ...]
    mame_extra: tuple[int, ...]


@dataclass(frozen=True)
class Alignment:
    same: int
    slips: list[Slip]
    lost_at: int | None  # port index where the streams never lined up again


def align(port: list[Event], mame: list[Event]) -> Alignment:
    """Walk two value streams together, lining them up again after each difference."""
    a = [e.value for e in port]
    b = [e.value for e in mame]
    i = j = same = 0
    slips: list[Slip] = []
    while i < len(a) and j < len(b):
        if a[i] == b[j]:
            i, j, same = i + 1, j + 1, same + 1
            continue
        found = None
        for total in range(1, _REACH):
            for skip_a in range(total + 1):
                skip_b = total - skip_a
                window = a[i + skip_a : i + skip_a + _WINDOW]
                if len(window) == _WINDOW and window == b[j + skip_b : j + skip_b + _WINDOW]:
                    found = (skip_a, skip_b)
                    break
            if found:
                break
        if found is None:
            if max(len(a) - i, len(b) - j) > _WINDOW:
                return Alignment(same, slips, i)
            break  # the tails are too short to line up: one last slip, below
        slips.append(
            Slip(i, port[i].seconds, tuple(a[i : i + found[0]]), tuple(b[j : j + found[1]]))
        )
        i, j = i + found[0], j + found[1]
    if i < len(a) or j < len(b):
        when = port[i].seconds if i < len(port) else (port[-1].seconds if port else 0.0)
        slips.append(Slip(i, when, tuple(a[i:]), tuple(b[j:])))
    return Alignment(same, slips, None)


def _percentile(sorted_values: list[float], fraction: float) -> float:
    return sorted_values[min(len(sorted_values) - 1, int(fraction * len(sorted_values)))]


def main(argv: list[str] | None = None, out: Callable[[str], None] = print) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 2:
        print("usage: rr2-fm-compare PORT_LOG MAME_LOG", file=sys.stderr)
        return 2
    try:
        port = read_port_log(Path(argv[0]))
        port_end = read_port_end(Path(argv[0]))
        mame = read_mame_log(Path(argv[1]))
    except (OSError, ValueError) as error:
        print(f"rr2-fm-compare: {error}", file=sys.stderr)
        return 2
    after = 0
    if port_end is not None:
        after = sum(1 for e in mame if e.seconds > port_end)
        mame = [e for e in mame if e.seconds <= port_end]
    result = compare(port, mame)
    out(f"EVENTS port={len(port)} mame={len(mame)}")
    if port_end is None:
        out("PORT_END unknown: the port's log has no END line")
    elif after:
        out(f"AFTER_PORT_END mame={after}")
    out(f"FROM_68000 port={_from_68000(port)} mame={_from_68000(mame)}")
    out(f"SAME_IN_ORDER {result.same_in_order}")
    at = result.same_in_order
    same = at == len(port) == len(mame)
    if same:
        out("FIRST_DIFFERENCE none")
    else:
        ours = port[at].name() if at < len(port) else "end"
        theirs = mame[at].name() if at < len(mame) else "end"
        out(f"FIRST_DIFFERENCE index={at} port={ours} mame={theirs}")
    if result.register_differences:
        counts = " ".join(f"{name}:{a}/{b}" for name, (a, b) in result.register_differences.items())
        out(f"REGISTERS_DIFFERING (port/mame writes) {counts}")
    if result.timing_samples:
        ordered = sorted(result.timing_samples)
        out(
            "TIMING samples (port later is positive): "
            f"median={statistics.median(ordered):.1f} p10={_percentile(ordered, 0.10):.1f} "
            f"p90={_percentile(ordered, 0.90):.1f} p99={_percentile(ordered, 0.99):.1f} "
            f"min={ordered[0]:.1f} max={ordered[-1]:.1f}"
        )
    if same and port:
        out("RESULT same")
        return 0

    def is_sample(e: Event) -> bool:
        return (e.part, e.register) == SAMPLE_REGISTER

    port_music = [e for e in port if not is_sample(e)]
    mame_music = [e for e in mame if not is_sample(e)]
    music = compare(port_music, mame_music)
    at = music.same_in_order
    music_same = at == len(port_music) == len(mame_music)
    if music_same:
        first = "none"
    else:
        ours = port_music[at].name() if at < len(port_music) else "end"
        theirs = mame_music[at].name() if at < len(mame_music) else "end"
        first = f"index={at} port={ours} mame={theirs}"
    out(
        f"MUSIC port={len(port_music)} mame={len(mame_music)} same_in_order={at} "
        f"first_difference={first}"
    )
    if music.timing_samples:
        ordered = sorted(music.timing_samples)
        out(
            "MUSIC_TIMING samples (port later is positive): "
            f"median={statistics.median(ordered):.1f} p1={_percentile(ordered, 0.01):.1f} "
            f"p99={_percentile(ordered, 0.99):.1f} min={ordered[0]:.1f} max={ordered[-1]:.1f}"
        )
    port_samples = [e for e in port if is_sample(e)]
    mame_samples = [e for e in mame if is_sample(e)]
    lined = align(port_samples, mame_samples)
    line = (
        f"SAMPLES port={len(port_samples)} mame={len(mame_samples)} same={lined.same} "
        f"slips={len(lined.slips)} port_extra={sum(len(s.port_extra) for s in lined.slips)} "
        f"mame_extra={sum(len(s.mame_extra) for s in lined.slips)}"
    )
    out(line if lined.lost_at is None else f"{line} lost_at={lined.lost_at}")
    for slip in lined.slips[:20]:
        ours = ",".join(f"{v:02X}" for v in slip.port_extra[:8]) or "-"
        theirs = ",".join(f"{v:02X}" for v in slip.mame_extra[:8]) or "-"
        out(
            f"SLIP at={slip.seconds:.3f}s sample={slip.port_index} "
            f"port_extra={ours} mame_extra={theirs}"
        )
    ok = music_same and lined.lost_at is None and bool(port_music or port_samples)
    out("RESULT music-same" if ok else "RESULT different")
    return 1
