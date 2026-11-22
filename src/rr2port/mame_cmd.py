"""Generate MAME debugger scripts that record a reference trace window.

The MAME debugger reads bare numbers as hexadecimal, so every count is written
with an explicit ``0x`` prefix.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from typing import Literal

HANDOFF_PC = 0xDF48
BOOT_START_PC = 0x206
FRONTEND_PC = 0xCBCC8
VBLANK_PC = 0x11B8
RAM_RANGE = "ff0000,10000"
PICTURE_PREFIX = "boot-v"


@dataclass(frozen=True)
class Window:
    name: str
    start: Literal["reset"] | int
    end_count: int
    playback: str | None


WINDOWS = {
    "boot": Window("boot", "reset", 30, None),
    "race": Window("race", 500, 20, "full-throttle"),
    # The first passes of the same race, where native timing first parts from MAME's.
    "race-start": Window("race-start", 2, 12, "full-throttle"),
    # The stretch after it, holding the first passes that still differed once
    # race-start matched instruction for instruction.
    "race-early": Window("race-early", 14, 30, "full-throttle"),
    # Two of the ten passes (68 and 79) that still differed after that.
    "race-66": Window("race-66", 66, 14, "full-throttle"),
    # The last two passes with a difference (823 and 954), each with the pass before it.
    "race-822": Window("race-822", 822, 2, "full-throttle"),
    "race-953": Window("race-953", 953, 2, "full-throttle"),
}

# MAME's "pc" is one word past the instruction start, so the PC is taken from the
# disassembly line that follows each register line. "a7" would parse as hex.
_TRACE_ACTION = (
    '{tracelog "T %04X ",sr; '
    'tracelog "%08X %08X %08X %08X %08X %08X %08X %08X ",d0,d1,d2,d3,d4,d5,d6,d7; '
    'tracelog "%08X %08X %08X %08X %08X %08X %08X %08X ",a0,a1,a2,a3,a4,a5,a6,sp; '
    'tracelog "%d\\n",totalcycles}'
)


def _watchpoints() -> list[str]:
    return [
        'wp 100000,d00000,r,1,{logerror "RR2HW R %06X %X %X\\n",wpaddr,wpsize,wpdata; g}',
        'wp 100000,d00000,w,1,{logerror "RR2HW W %06X %X %X\\n",wpaddr,wpsize,wpdata; g}',
        "wpdisable",
    ]


def _trace(out_dir: str) -> str:
    return f"trace {out_dir}/raw-trace.txt,maincpu,noloop,{_TRACE_ACTION}"


def _save(out_dir: str, name: str) -> str:
    return f"save {out_dir}/{name},{RAM_RANGE}"


def _boot_cmd(window: Window, out_dir: str) -> list[str]:
    last = window.end_count - 1
    return [
        "temp0=0",
        "temp1=0",
        _trace(out_dir),
        f'bp {BOOT_START_PC:x},1,{{wpenable; tracelog "RR2START\\n"; '
        f"{_save(out_dir, 'ram-start.bin')}; g}}",
        f"bp {FRONTEND_PC:x},temp0==0,{{temp0=1; g}}",
        f"bp {VBLANK_PC:x},temp0==1 && temp1<{last:#x},{{temp1++; g}}",
        f'bp {VBLANK_PC:x},temp0==1 && temp1=={last:#x},{{temp1++; tracelog "RR2END\\n"; '
        f"{_save(out_dir, 'ram-end.bin')}; quit}}",
        "g",
    ]


def _race_cmd(window: Window, out_dir: str) -> list[str]:
    assert isinstance(window.start, int)
    first = window.start
    final = first + window.end_count
    pc = f"{HANDOFF_PC:x}"
    lines = [
        "temp0=0",
        f"bp {pc},temp0<{first - 1:#x},{{temp0++; g}}",
        # Tracing begins with the instruction after this one, so start one handoff early.
        f"bp {pc},temp0=={first - 1:#x},{{temp0++; {_trace(out_dir)}; g}}",
        f'bp {pc},temp0=={first:#x},{{temp0++; wpenable; tracelog "RR2START\\n"; '
        f"{_save(out_dir, 'ram-start.bin')}; {_save(out_dir, 'handoff-00.bin')}; g}}",
    ]
    for number in range(1, window.end_count):
        lines.append(
            f"bp {pc},temp0=={first + number:#x},"
            f"{{temp0++; {_save(out_dir, f'handoff-{number:02d}.bin')}; g}}"
        )
    lines += [
        f'bp {pc},temp0=={final:#x},{{temp0++; tracelog "RR2END\\n"; '
        f"{_save(out_dir, 'ram-end.bin')}; "
        f"{_save(out_dir, f'handoff-{window.end_count:02d}.bin')}; quit}}",
        "g",
    ]
    return lines


def build_trace_cmd(window: Window, out_dir: str) -> str:
    """Return the debugger script that records ``window`` into ``out_dir``."""
    body = _boot_cmd(window, out_dir) if window.start == "reset" else _race_cmd(window, out_dir)
    return "\n".join(["focus maincpu", *_watchpoints(), *body]) + "\n"


def picture_name(vblank_count: int) -> str:
    """File stem of the reference picture taken at a VBlank handler entry count."""
    return f"{PICTURE_PREFIX}{vblank_count:04d}"


def build_picture_cmd(vblank_counts: Sequence[int]) -> str:
    """Debugger script that snapshots the screen at the given VBlank handler entries.

    Counts are 1-based entries to the VBlank handler since power-on, so the port
    can stop at the same point without sharing MAME's frame numbering. The
    snapshot taken there matches the port's picture one entry earlier (see
    analysis/first-picture-findings.md).
    MAME appends ``.png`` and saves under ``-snapshot_directory``. No trace is
    taken: a snapshot during tracing made MAME hang on exit.
    """
    counts = sorted(set(vblank_counts))
    if not counts or counts[0] < 1:
        raise ValueError("at least one positive VBlank count is needed")
    pc = f"{VBLANK_PC:#x}"
    others = " && ".join(f"temp0!={count - 1:#x}" for count in counts)
    lines = ["focus maincpu", "temp0=0", f"bp {pc},{others},{{temp0++; g}}"]
    for count in counts:
        last = "quit" if count == counts[-1] else "g"
        lines.append(
            f"bp {pc},temp0=={count - 1:#x},{{temp0++; snap {picture_name(count)}; {last}}}"
        )
    lines.append("g")
    return "\n".join(lines) + "\n"


HBLANK_PC = 0x14BE


def build_hv_cmd(first_handoff: int, handoffs: int) -> str:
    """Debugger script that logs HV counter reads and interrupt entries with the beam position.

    Inside the given range of game-loop handoffs of the race recording it logs
    ``RR2HV <addr> <bits> <value> <beamx> <beamy> <totalcycles>`` for every read
    of the HV counter port and ``RR2INT <level> <beamx> <beamy> <totalcycles>``
    as each line (level 4) and frame (level 6) interrupt handler is entered.
    """
    pc = f"{HANDOFF_PC:#x}"
    last = first_handoff + handoffs
    return (
        "\n".join(
            [
                "focus maincpu",
                "temp0=0",
                "temp1=0",
                'wp 0xc00008,0x2,r,1,{logerror "RR2HV %06X %X %X %d %d %d\\n",'
                "wpaddr,wpsize,wpdata,beamx,beamy,totalcycles; g}",
                "wpdisable",
                f'bp {HBLANK_PC:#x},temp1==1,{{logerror "RR2INT 4 %d %d %d\\n",'
                "beamx,beamy,totalcycles; g}",
                f'bp {VBLANK_PC:#x},temp1==1,{{logerror "RR2INT 6 %d %d %d\\n",'
                "beamx,beamy,totalcycles; g}",
                f"bp {pc},temp0<{first_handoff:#x},{{temp0++; g}}",
                f"bp {pc},temp0=={first_handoff:#x},{{temp0++; temp1=1; wpenable; g}}",
                f"bp {pc},temp0>{first_handoff:#x} && temp0<{last:#x},{{temp0++; g}}",
                f"bp {pc},temp0=={last:#x},{{temp0++; quit}}",
                "g",
            ]
        )
        + "\n"
    )


def build_stop_cmd(vblank_count: int) -> str:
    """Debugger script that stops MAME as the Nth VBlank handler entry begins.

    The debugger is left stopped at the handler's first instruction, so a Lua
    script can read the machine's state at exactly that point
    (``mame/rr206-vdp-state.lua``).
    """
    if vblank_count < 1:
        raise ValueError("the VBlank count starts at 1")
    pc = f"{VBLANK_PC:#x}"
    target = f"{vblank_count - 1:#x}"
    return (
        "\n".join(
            [
                "focus maincpu",
                "temp0=0",
                f"bp {pc},temp0!={target},{{temp0++; g}}",
                f"bp {pc},temp0=={target},{{temp0++}}",
                "g",
            ]
        )
        + "\n"
    )


PAD1_READ_RETURN = 0x1154  # rts of the port 1 pad read at 0x10F4; D0.b holds the raw byte
PAD2_READ_RETURN = 0x11B6  # rts of the port 2 pad read at 0x1156


def build_handoff_stop_cmd() -> str:
    """Debugger script that stops MAME at every game-loop handoff and logs pad reads.

    ``mame/rr206-handoff-ram.lua`` saves work RAM at each stop and resumes. All
    controller access in RR206 goes through two routines; the value each call
    returns is logged as ``RR2PAD <port> <value>``, and each handoff as
    ``RR2HANDOFF <cycles> <sr> <d0-d7> <a0-a7>``, so a replay can hand the game
    the same reads pass by pass and can restart a pass from the original's state.
    """
    return (
        "\n".join(
            [
                "focus maincpu",
                # No "g": the debugger stays stopped here for the Lua script. The
                # line gives MAME's cycle count and every register at the handoff.
                f'bp {HANDOFF_PC:#x},1,{{logerror "RR2HANDOFF %d %04X ",totalcycles,sr; '
                'logerror "%08X %08X %08X %08X %08X %08X %08X %08X ",d0,d1,d2,d3,d4,d5,d6,d7; '
                'logerror "%08X %08X %08X %08X %08X %08X %08X %08X\\n",a0,a1,a2,a3,a4,a5,a6,sp}',
                f'bp {PAD1_READ_RETURN:#x},1,{{logerror "RR2PAD 1 %02X\\n",d0&0xff; g}}',
                f'bp {PAD2_READ_RETURN:#x},1,{{logerror "RR2PAD 2 %02X\\n",d0&0xff; g}}',
                "g",
            ]
        )
        + "\n"
    )
