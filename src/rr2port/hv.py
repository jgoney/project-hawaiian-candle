"""Fit the video chip's line and column counters to MAME's beam position.

MAME is run with a watchpoint on the HV counter port that logs, for each read,
the value returned and where the beam was. From that the value of the V
counter for each beam line and of the H counter for each beam column follow
directly, as plain lookup tables.
"""

from __future__ import annotations

from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

from rr2port.trace import TraceFormatError

HV_PREFIX = "RR2HV "
INT_PREFIX = "RR2INT "
HV_PORT = 0xC00008


@dataclass(frozen=True)
class HvSample:
    addr: int  # 0xC00008 (V byte, or the whole word) or 0xC00009 (H byte)
    size: int  # 1 or 2 bytes
    value: int
    beamx: int
    beamy: int
    cycles: int

    @property
    def v(self) -> int | None:
        if self.size == 2:
            return self.value >> 8
        return self.value if self.addr & 1 == 0 else None

    @property
    def h(self) -> int | None:
        if self.size == 2:
            return self.value & 0xFF
        return self.value if self.addr & 1 else None


@dataclass(frozen=True)
class HvInterrupt:
    level: int
    beamx: int
    beamy: int
    cycles: int


@dataclass
class CounterFit:
    v_by_beamy: dict[int, int] = field(default_factory=dict)
    h_by_beamx: dict[int, int] = field(default_factory=dict)
    residual: list[HvSample] = field(default_factory=list)
    samples: int = 0


def read_hv_log(error_log: Path) -> tuple[list[HvSample], list[HvInterrupt]]:
    """Read ``RR2HV`` and ``RR2INT`` rows from MAME's error log."""
    samples: list[HvSample] = []
    interrupts: list[HvInterrupt] = []
    with error_log.open(errors="replace") as handle:
        for line_number, line in enumerate(handle, 1):
            fields = line.split()
            try:
                if line.startswith(HV_PREFIX):
                    if len(fields) != 7:
                        raise ValueError("expected 'RR2HV addr bits value beamx beamy cycles'")
                    bits = int(fields[2], 16)
                    if bits not in (8, 16):
                        raise ValueError(f"unsupported access width {bits} bits")
                    samples.append(
                        HvSample(
                            addr=int(fields[1], 16),
                            size=bits // 8,
                            value=int(fields[3], 16),
                            beamx=int(fields[4]),
                            beamy=int(fields[5]),
                            cycles=int(fields[6]),
                        )
                    )
                elif line.startswith(INT_PREFIX):
                    if len(fields) != 5:
                        raise ValueError("expected 'RR2INT level beamx beamy cycles'")
                    interrupts.append(HvInterrupt(*(int(value) for value in fields[1:])))
            except ValueError as error:
                raise TraceFormatError(line_number, str(error)) from None
    return samples, interrupts


def _majority(pairs: list[tuple[int, int]]) -> dict[int, int]:
    """For each key, the value seen most often (ties go to the smaller value)."""
    counts: dict[int, Counter[int]] = {}
    for key, value in pairs:
        counts.setdefault(key, Counter())[value] += 1
    return {
        key: min(seen, key=lambda value: (-seen[value], value))
        for key, seen in sorted(counts.items())
    }


def fit_counters(samples: list[HvSample]) -> CounterFit:
    """Build the V-by-line and H-by-column tables and list what they cannot explain."""
    fit = CounterFit(samples=len(samples))
    fit.v_by_beamy = _majority([(s.beamy, s.v) for s in samples if s.v is not None])
    fit.h_by_beamx = _majority([(s.beamx, s.h) for s in samples if s.h is not None])
    for sample in samples:
        v_wrong = sample.v is not None and fit.v_by_beamy[sample.beamy] != sample.v
        h_wrong = sample.h is not None and fit.h_by_beamx[sample.beamx] != sample.h
        if v_wrong or h_wrong:
            fit.residual.append(sample)
    return fit
