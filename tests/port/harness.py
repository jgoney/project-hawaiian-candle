"""Compile-and-run harness: translate a code snippet, run it natively, read back state."""

from __future__ import annotations

import shutil
import subprocess
import zlib
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

import pytest

from rr2port.discover import discover
from rr2port.emit import emit_program

REPO = Path(__file__).resolve().parents[2]
CORE = REPO / "port" / "core"
HW = REPO / "port" / "hw"
RENDER = REPO / "port" / "render"
CORE_SOURCES = [
    CORE / "rr_mem.c",
    CORE / "rr_run.c",
    CORE / "rr_hw_replay.c",
    *sorted(HW.glob("*.c")),
    *sorted(RENDER.glob("*.c")),
]
THIRD_PARTY = REPO / "port" / "third_party"
# The sound cores are other people's code: built once, with their own flags.
THIRD_PARTY_SOURCES = [THIRD_PARTY / "z80" / "z80.c", THIRD_PARTY / "nuked-opn2" / "ym3438.c"]
ROM_SIZE = 0x1000
_cache: dict[bytes, Path] = {}


@dataclass
class Result:
    returncode: int
    stderr: str
    d: tuple[int, ...] = ()
    a: tuple[int, ...] = ()
    pc: int = 0
    sr: int = 0
    cycles: int = 0
    memory: bytes = b""

    @property
    def ccr(self) -> int:
        return self.sr & 0x1F

    def ram(self, addr: int, n: int) -> bytes:
        offset = addr & 0xFFFF
        return self.memory[offset : offset + n]


def compiler() -> str:
    cc = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
    if cc is None:
        pytest.skip("no C compiler available")
    return cc


def third_party_objects() -> list[Path]:
    """The sound cores as object files, built when missing or older than their source."""
    out_dir = REPO / "generated" / "port" / "build" / "harness"
    out_dir.mkdir(parents=True, exist_ok=True)
    objects = []
    for source in THIRD_PARTY_SOURCES:
        obj = out_dir / (source.stem + ".o")
        if not obj.exists() or obj.stat().st_mtime < source.stat().st_mtime:
            subprocess.run(
                [compiler(), "-std=c11", "-O2", "-w", "-c", "-o", str(obj), str(source)],
                check=True,
            )
        objects.append(obj)
    return objects


def build_snippet(
    code: bytes,
    tmp_path: Path,
    entries: Sequence[int] = (0,),
    main: Path | None = None,
    defines: Sequence[str] = (),
) -> tuple[Path, Path]:
    """Translate and compile ``code`` placed at address 0. Returns (binary, rom file).

    ``main`` is the C file providing main(); the default is the snippet driver.
    """
    main = main or Path(__file__).with_name("harness_main.c")
    rom = code.ljust(ROM_SIZE, b"\xff")
    rom_path = tmp_path / "snippet.rom"
    rom_path.write_bytes(rom)
    key = code + repr((str(main), tuple(entries), tuple(defines))).encode()
    if key in _cache:
        return _cache[key], rom_path
    insns = discover(rom, entries)
    source = tmp_path / "snippet_code.c"
    source.write_text(emit_program(insns, zlib.crc32(rom)))
    binary = tmp_path / "snippet"
    subprocess.run(
        [compiler(), "-std=c11", "-Wall", "-Wextra", "-Werror", "-O0", f"-I{CORE}", f"-I{HW}",
         f"-I{RENDER}", *(f"-I{source.parent}" for source in THIRD_PARTY_SOURCES),
         *(f"-D{name}" for name in defines),
         "-o", str(binary), str(source), str(main), *map(str, CORE_SOURCES),
         *map(str, third_party_objects())],
        check=True,
    )  # fmt: skip
    _cache[key] = binary
    return binary, rom_path


def run_snippet(
    code: bytes,
    tmp_path: Path,
    *,
    d: Mapping[int, int] | None = None,
    a: Mapping[int, int] | None = None,
    sr: int = 0x2700,
    ram: Mapping[int, bytes] | None = None,
    steps: int = 1,
) -> Result:
    binary, rom_path = build_snippet(code, tmp_path)
    script = [f"sr {sr:X}", f"steps {steps}"]
    script += [f"d {n} {value:X}" for n, value in (d or {}).items()]
    script += [f"a {n} {value:X}" for n, value in (a or {}).items()]
    script += [f"ram {addr:X} {data.hex()}" for addr, data in (ram or {}).items()]
    ram_out = tmp_path / "ram.bin"
    done = subprocess.run(
        [str(binary), str(rom_path), str(ram_out)],
        input="\n".join(script) + "\n",
        capture_output=True,
        text=True,
    )
    if done.returncode != 0:
        return Result(returncode=done.returncode, stderr=done.stderr)
    values: dict[str, int] = {}
    for line in done.stdout.splitlines():
        fields = line.split()
        key = fields[0] if len(fields) == 2 else fields[0] + fields[1]
        values[key] = int(fields[-1], 10 if fields[0] == "cycles" else 16)
    return Result(
        returncode=0,
        stderr=done.stderr,
        d=tuple(values[f"d{n}"] for n in range(8)),
        a=tuple(values[f"a{n}"] for n in range(8)),
        pc=values["pc"],
        sr=values["sr"],
        cycles=values["cycles"],
        memory=ram_out.read_bytes(),
    )
