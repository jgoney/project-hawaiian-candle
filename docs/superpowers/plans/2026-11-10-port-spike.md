# Native Port Spike (Stage 0 + Stage 1 proof) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Commit the existing research, then prove that 68000 code from RR206 can be mechanically translated to C and reproduce MAME's execution instruction for instruction, over a boot window and an in-race window, with suspend/resume and save/restore.

**Architecture:** A Python translator (`rr2port`) decodes ROM code and emits one small C function per 68000 instruction. A C runtime (`port/core`) holds all machine state in one struct plus the 64 KiB RAM image and runs instructions through a lookup-and-dispatch loop, so execution can stop between any two instructions and nothing lives on the native call stack. For the spike, hardware is not modelled: MAME records every hardware read, and the runtime replays them, with interrupts injected at the recorded instruction. That isolates the question "is the translation exact" from "is the hardware model exact". The spike ends with a findings document; later Stage 1 milestones are planned after the owner reviews it.

**Tech Stack:** Python ≥3.12 with uv, pytest, ruff; capstone 5.x for 68000 decoding; C11 with Apple clang and make; MAME 0.289 debugger scripts.

**Spec:** `docs/superpowers/specs/2026-11-10-native-port-design.md` (sections "Stage 0", "Stage 1: Proof first", "Stage 2", "Complete state").

## Global Constraints

- Target ROM is RR206 only, SHA-256 `c7a609019b1f052bcee6204a3a09ea77a5150a748de90b51382fedf4df207848`, CRC-32 `0876E992`. Every tool that reads a ROM verifies this and refuses anything else.
- Nothing ROM-derived is committed: no ROM bytes, no generated C, no traces, no captures. All of these go under the ignored `generated/port/`. Annotation files hold addresses only.
- Tools open the ROM read-only.
- Tests that need the ROM, MAME output or a C compiler skip cleanly when it is absent; the default `uv run --group dev pytest` must pass on a clean checkout.
- Generated code and core are C11, built with `-std=c11 -Wall -Wextra -Werror -O2`. No macOS-only APIs in `port/core`.
- All state that determines future behaviour lives in `RrMachine`. No static mutable variables in `port/core` or generated code.
- Memory is big-endian. Addresses are masked to 24 bits. `0xE00000–0xFFFFFF` mirrors work RAM every 64 KiB. A 32-bit hardware access is two 16-bit accesses, high word first.
- Python: ruff clean with the existing config (line length 100).
- MAME 0.289. Existing script conventions apply: shell scripts verify the ROM hash, write only under `generated/`, and reject capture names outside `[A-Za-z0-9_-]`.
- Claims about the original game in docs keep the verified/hypothesis labelling used in `analysis/`.

## Review Focus

1. **Wrong ROM** (RR205, a patched or truncated file) given to the translator or checker: refuse with a message naming the expected and actual hash; never emit or run. Test in Task 4 and Task 6.
2. **Jump to an address that was never translated** (missed code, indirect jump): stop immediately with the target address and the address of the jumping instruction; never continue. Test in Task 3.
3. **Hardware replay out of step** (code reads a different address than MAME recorded, or the log runs out): stop with the instruction address, the address wanted and the address recorded. Test in Task 3.
4. **Truncated or corrupt trace file** (MAME killed mid-write): the reader reports the line number; the checker must not report success on a short comparison. Test in Task 2 and Task 6.
5. **Save file from a different ROM or format version** loaded into the runtime: refuse with a clear message. Test in Task 3.

## File Structure

```
port/
  Makefile                      builds core, tests, and rr-trace-check
  annotations/rr206.toml        extra entry points and jump tables (addresses only)
  core/rr_machine.h             RrCpu, RrMachine, RrHw
  core/rr_mem.c  rr_mem.h       bus access and routing
  core/rr_run.c  rr_run.h       step, interrupt entry, fatal, save/load, crc32
  core/rr_hw_replay.c           hardware backed by a recorded access log
  tools/rr_trace_check.c        replays a trace window and compares
  tests/test_core.c             C unit tests
src/rr2port/
  trace.py                      trace/hw-log readers, interrupt finder
  mame_cmd.py                   generates MAME debugger scripts for a window
  decode.py                     capstone wrapper → Insn
  discover.py                   control-flow discovery, annotations
  cycles.py                     68000 cycle costs
  emit.py                       Insn → C
  cli.py                        rr2-translate, rr2-trace-report
scripts/run-mame-port-trace-rr206.sh
tests/port/                     pytest for rr2port, plus compile-and-run harness
analysis/port-spike-findings.md written in Task 8
```

Generated at run time (ignored): `generated/port/rr206_code.c`, `generated/port/build/`, `generated/port/traces/<window>/`.

---

### Task 1: First commit and housekeeping (Stage 0)

**Files:**
- Modify: `analysis/godot-plan.md` (top of file), `README.md` (end)

- [ ] **Step 1: Check nothing ROM-derived would be committed**

Run: `git add -A --dry-run | grep -Ei '\.(gen|bin|smd|rom|zip|inp)$|generated/|ghidra-projects/|godot/captures/'`
Expected: no output. If anything prints, fix `.gitignore` first.

- [ ] **Step 2: Mark the old plan superseded and point to the new design**

Insert as the first lines under the title of `analysis/godot-plan.md`:

```markdown
> **Superseded 2026-11-10** by `docs/superpowers/specs/2026-11-10-native-port-design.md`.
> The asset pipeline and MCP policy sections still apply; the clean-room
> reconstruction approach does not.
```

Append to `README.md`: one line pointing to the design document as the current direction, and one line pointing to this plan.

- [ ] **Step 3: Run existing checks**

Run: `uv run --group dev pytest && uv run --group dev ruff check .`
Expected: all pass.

- [ ] **Step 4: First commit, then branch**

```bash
git add -A
git commit -m "Initial commit: RR206 research tooling, notes, and native port design"
git switch -c port-spike
```

---

### Task 2: Reference trace windows from MAME

**Files:**
- Create: `src/rr2port/__init__.py`, `src/rr2port/trace.py`, `src/rr2port/mame_cmd.py`, `scripts/run-mame-port-trace-rr206.sh`, `tests/port/test_trace.py`, `tests/port/test_mame_cmd.py`
- Modify: `pyproject.toml` (add `src/rr2port` to wheel packages), `mame/README.md` (new section)

**Interfaces:**
- Produces, on disk under `generated/port/traces/<window>/`:
  - `trace.txt`: one line per instruction about to execute:
    `T <pc:6 hex> <sr:4 hex> <d0>…<d7> <a0>…<a7> <totalcycles:decimal>` (registers 8 hex digits each, space separated). The last line's instruction is not executed.
  - `hw.txt`: one line per 68000 access to `0x100000–0xDFFFFF`, in order: `R|W <addr:6 hex> <size:1|2> <value hex>`.
  - `ram-start.bin`, `ram-end.bin`: 65,536 bytes of work RAM at the first and last trace line.
  - `handoff-NN.bin` (race window only): work RAM each time `0xDF48` is about to execute, `NN` from `00`.
  - `meta.txt`: ROM SHA-256, MAME version, window name, input recording name.
- Produces, in Python:
  - `TraceStep(pc: int, sr: int, d: tuple[int, ...], a: tuple[int, ...], cycles: int)`
  - `read_trace(path: Path) -> Iterator[TraceStep]`, raising `TraceFormatError(line_number, reason)`
  - `Interrupt(index: int, level: int)`; `find_interrupts(steps: Sequence[TraceStep], vectors: Mapping[int, int] = {4: 0x14BE, 6: 0x11B8}) -> list[Interrupt]`. An interrupt is at index `k` when `steps[k].pc` is a vector target, `steps[k].a[7] == steps[k-1].a[7] - 6`, and the interrupt mask in `steps[k].sr` equals the level.
  - `HwAccess(kind: str, addr: int, size: int, value: int)`; `read_hw_log(path: Path) -> list[HwAccess]`
  - `Window(name: str, start: Literal["reset"] | int, end_count: int, playback: str | None)`; `WINDOWS = {"boot": Window("boot", "reset", 30, None), "race": Window("race", 500, 20, "full-throttle")}`
  - `build_trace_cmd(window: Window, out_dir: str) -> str`

**Window definitions:**
- `boot`: trace from reset (`pc == 0x200`); ends at the 30th entry to `0x11B8` after the first execution of `0xCBCC8`.
- `race`: plays back `generated/mame/full-throttle.inp`; trace starts when `0xDF48` is about to execute for the 500th time; ends when it is about to execute 20 handoffs later. `handoff-00.bin` equals `ram-start.bin`; `handoff-20.bin` equals `ram-end.bin`.

**MAME mechanics:** the debugger script uses `trace` with a `tracelog` action for `trace.txt`, a read/write watchpoint over `0x100000–0xDFFFFF` logging `wpaddr`, `wpsize`, `wpdata` with `logerror` (rows prefixed `RR2HW`, split out of `error.log` into `hw.txt` by the shell script), and `save <file>,ff0000,10000` for RAM images. Because `save` cannot format file names, `build_trace_cmd` emits one breakpoint line per handoff. Confirm `totalcycles`, `wpdata` on reads, and `trace` action syntax against MAME 0.289's `help` before relying on them; if `totalcycles` is unavailable, write `0` and record that in Task 8.

- [ ] **Step 1: Write failing tests** in `tests/port/test_trace.py`:

```python
LINE = "T 000200 2700 " + " ".join(["00000000"] * 15) + " 00FFFFF6 123"

def test_read_trace_parses_fields(tmp_path):
    p = tmp_path / "trace.txt"; p.write_text(LINE + "\n")
    (step,) = read_trace(p)
    assert (step.pc, step.sr, step.a[7], step.cycles) == (0x200, 0x2700, 0x00FFFFF6, 123)

def test_read_trace_reports_line_number_of_truncated_line(tmp_path):
    p = tmp_path / "trace.txt"; p.write_text(LINE + "\n" + LINE[:40] + "\n")
    with pytest.raises(TraceFormatError) as e:
        list(read_trace(p))
    assert e.value.line_number == 2

def test_find_interrupts_detects_vblank_entry():
    before = step(pc=0x1000, sr=0x2000, sp=0xFFFF00)
    after = step(pc=0x11B8, sr=0x2600, sp=0xFFFEFA)
    assert find_interrupts([before, after]) == [Interrupt(index=1, level=6)]

def test_find_interrupts_ignores_plain_jump_to_handler():
    assert find_interrupts([step(pc=0x1000, sr=0x2000, sp=0xFFFF00),
                            step(pc=0x11B8, sr=0x2000, sp=0xFFFF00)]) == []

def test_read_hw_log_rejects_unknown_kind(tmp_path): ...  # "X 0C0004 2 0000" → TraceFormatError line 1
```

and in `tests/port/test_mame_cmd.py`:

```python
def test_race_cmd_saves_one_ram_image_per_handoff():
    text = build_trace_cmd(WINDOWS["race"], "out")
    assert [f"handoff-{n:02d}.bin" in text for n in range(21)] == [True] * 21
    assert "ff0000,10000" in text

def test_boot_cmd_starts_trace_before_first_instruction():
    lines = build_trace_cmd(WINDOWS["boot"], "out").splitlines()
    assert lines.index(next(l for l in lines if l.startswith("trace "))) < lines.index("g")
```

- [ ] **Step 2: Run and see them fail**

Run: `uv run --group dev pytest tests/port -v`
Expected: FAIL, `ModuleNotFoundError: rr2port`.

- [ ] **Step 3: Implement `trace.py` and `mame_cmd.py`** to the interfaces above.

- [ ] **Step 4: Implement `scripts/run-mame-port-trace-rr206.sh <boot|race> [ROM]`**

Model it on `scripts/replay-mame-physics-rr206.sh` (hash check, `generated/mame` directories, `-nothrottle -sound none`). It writes the generated debugger script and all outputs to `generated/port/traces/<window>/`, and exits non-zero if `trace.txt` is missing or its first line's PC is not the window's start address (`000200` for boot, `00DF48` for race).

- [ ] **Step 5: Run tests, then capture both windows**

Run: `uv run --group dev pytest tests/port -v` → PASS.
Run: `./scripts/run-mame-port-trace-rr206.sh boot && ./scripts/run-mame-port-trace-rr206.sh race`
Expected: both directories populated; `ram-start.bin` and `ram-end.bin` are 65,536 bytes; the race directory has `handoff-00.bin` … `handoff-20.bin`.

- [ ] **Step 6: Sanity-check the captures**

Run: `uv run python -c "from pathlib import Path; from rr2port.trace import *; s=list(read_trace(Path('generated/port/traces/race/trace.txt'))); print(len(s), len(find_interrupts(s)))"`
Expected: several hundred thousand steps; interrupt count is at least 60 (20 handoffs at about 5 frames each, plus HBlank entries if enabled). Record both numbers in the commit message.

- [ ] **Step 7: Document in `mame/README.md` and commit**

```bash
git add src/rr2port tests/port scripts/run-mame-port-trace-rr206.sh mame/README.md pyproject.toml
git commit -m "Add MAME reference trace windows for the port spike"
```

---

### Task 3: C runtime core

**Files:**
- Create: `port/Makefile`, `port/core/rr_machine.h`, `port/core/rr_mem.h`, `port/core/rr_mem.c`, `port/core/rr_run.h`, `port/core/rr_run.c`, `port/core/rr_hw_replay.c`, `port/tests/test_core.c`

**Interfaces (produces):**

```c
/* rr_machine.h */
typedef struct RrHw RrHw;
struct RrHw {
    uint16_t (*read)(RrHw *hw, uint32_t addr, int size);              /* size 1 or 2 */
    void     (*write)(RrHw *hw, uint32_t addr, int size, uint16_t v);
    void     (*close)(RrHw *hw);
};
typedef struct {
    uint32_t d[8], a[8], pc;
    uint16_t sr;
    uint64_t cycles;       /* 68000 cycles charged so far */
    uint64_t insns;        /* instructions executed so far */
} RrCpu;
typedef struct {
    RrCpu cpu;
    uint8_t ram[0x10000];
    const uint8_t *rom; uint32_t rom_size; uint32_t rom_crc32;
    RrHw *hw;
} RrMachine;

/* rr_mem.h */
uint8_t  rr_read8 (RrMachine *m, uint32_t addr);
uint16_t rr_read16(RrMachine *m, uint32_t addr);
uint32_t rr_read32(RrMachine *m, uint32_t addr);
void rr_write8 (RrMachine *m, uint32_t addr, uint8_t v);
void rr_write16(RrMachine *m, uint32_t addr, uint16_t v);
void rr_write32(RrMachine *m, uint32_t addr, uint32_t v);

/* rr_run.h */
typedef void (*RrInsnFn)(RrMachine *m);
RrInsnFn rr_lookup(uint32_t pc);              /* defined by generated code; NULL if untranslated */
extern const uint32_t rr_code_rom_crc32;      /* defined by generated code */
void rr_step(RrMachine *m);                   /* one instruction; fatal if rr_lookup is NULL */
void rr_interrupt(RrMachine *m, int level);   /* 68000 autovector entry: push PC, push SR, set S, set mask, load vector 24+level */
_Noreturn void rr_fatal(const RrMachine *m, const char *fmt, ...);  /* prints "RR2FATAL pc=%06X insn=%llu: ..." to stderr, exit(2) */
uint32_t rr_crc32(const uint8_t *data, size_t n);
int rr_save(const RrMachine *m, FILE *f);     /* "RR2SAVE1", rom_crc32, cpu, ram */
int rr_load(RrMachine *m, FILE *f);           /* nonzero + message on bad magic or CRC mismatch */

/* rr_hw_replay.c */
RrHw *rr_hw_replay_open(const char *hw_log_path, const RrMachine *m);
size_t rr_hw_replay_remaining(RrHw *hw);
size_t rr_hw_replay_position(RrHw *hw);
void   rr_hw_replay_seek(RrHw *hw, size_t position);
```

Replay rules: a read must match the next `R` record's address and size and returns its value; a write must match the next `W` record's address, size and value. Any mismatch, or running past the end, is `rr_fatal` with wanted versus recorded. Writes to ROM addresses are `rr_fatal`. The replay position is part of saved state for the checker (Task 7 saves it alongside `rr_save` output).

`make -C port test` builds and runs `port/tests/test_core.c`, which supplies its own small `rr_lookup` table. Build outputs go to `generated/port/build/`.

- [ ] **Step 1: Write failing tests** in `port/tests/test_core.c` (plain `assert`, one function per case, `main` runs all):
  - `test_ram_is_big_endian`: `rr_write32(m, 0xFF0000, 0x11223344)` → `m->ram[0..3] == 11 22 33 44`; `rr_read16(m, 0xFF0002) == 0x3344`.
  - `test_ram_mirrors`: write8 at `0xE01234`, read8 at `0xFF1234` returns it.
  - `test_address_masks_to_24_bits`: `rr_read8(m, 0xFFFF0000)` reads `ram[0]`.
  - `test_long_hw_read_is_two_words_high_first`: fake `RrHw` records calls; `rr_read32(m, 0xC00000)` produces reads at `0xC00000` then `0xC00002`, size 2 each.
  - `test_interrupt_entry`: `pc=0x1234, sr=0x2000, a7=0xFFFF00`, vector 30 in a fake ROM = `0x11B8`; after `rr_interrupt(m, 6)`: `pc==0x11B8`, `sr==0x2600`, `a7==0xFFFEFA`, word at `a7` is `0x2000`, long at `a7+2` is `0x1234`.
  - `test_save_load_round_trip`: save, scribble over cpu and ram, load, compare equal.
  - `test_load_rejects_other_rom`: change `rom_crc32` before load → nonzero return.
  - Death tests, each run in a forked child and expecting exit status 2 with `RR2FATAL` on stderr: `test_step_at_untranslated_pc_is_fatal` (message contains the PC), `test_replay_address_mismatch_is_fatal`, `test_replay_exhausted_is_fatal`.

- [ ] **Step 2: Run and see it fail**

Run: `make -C port test`
Expected: compile errors for missing headers.

- [ ] **Step 3: Implement the core** to the interfaces above.

- [ ] **Step 4: Run tests**

Run: `make -C port test`
Expected: `all core tests passed`, exit 0, no warnings.

- [ ] **Step 5: Commit**

```bash
git add port
git commit -m "Add port runtime core: machine state, bus, replay hardware, save/load"
```

---

### Task 4: Translator front end (decode and discover)

**Files:**
- Create: `src/rr2port/decode.py`, `src/rr2port/discover.py`, `port/annotations/rr206.toml`, `tests/port/test_decode.py`, `tests/port/test_discover.py`
- Modify: `pyproject.toml` (dependency `capstone>=5.0.3,<6`)

**Interfaces (produces):**

```python
@dataclass(frozen=True)
class Insn:
    pc: int
    size: int                 # bytes, including extension words
    mnemonic: str             # lower case, no size suffix, e.g. "add", "dbf", "movem"
    opsize: int               # 1, 2, 4, or 0 when not applicable
    operands: tuple[Operand, ...]
    flow: str                 # "next" | "jump" | "cond" | "call" | "ret" | "indirect" | "stop"
    targets: tuple[int, ...]  # static successor addresses other than pc+size

def decode(rom: bytes, pc: int) -> Insn            # raises DecodeError(pc, reason)
def discover(rom: bytes, entries: Iterable[int], jump_tables: Mapping[int, Sequence[int]] = {}) -> dict[int, Insn]
def load_annotations(path: Path) -> Annotations    # .entries: list[int]; .jump_tables: dict[int, list[int]]
def trace_entries(trace_path: Path) -> set[int]    # every distinct PC in a trace
def verify_rr206(rom: bytes) -> None               # raises WrongRomError(expected_sha256, actual_sha256)
```

`Operand` is a small tagged dataclass covering the 68000 addressing modes (data register, address register, indirect, post-increment, pre-decrement, displacement, indexed, absolute short/long, PC-relative with and without index, immediate, register list, and the `ccr`/`sr`/`usp` specials). Odd `pc` is a `DecodeError`. `discover` follows `next`, `jump`, `cond` and `call` edges, stops at `ret`, `indirect` and `stop`, and adds `jump_tables[pc]` targets for an `indirect` instruction listed there. `port/annotations/rr206.toml` starts with the exception vector targets found by `rr2rom.rom` plus `0x35A`, `0xE038` and `0xCBCC8` as entries, each with a comment citing the note that verifies it.

For the spike, the translator also seeds discovery with `trace_entries` of both windows. That is a spike shortcut and must be listed as such in Task 8; the full translator needs the annotation file to be complete.

- [ ] **Step 1: Write failing tests**

```python
def test_decode_add_word_register():
    i = decode(bytes.fromhex("D041"), 0)
    assert (i.mnemonic, i.opsize, i.size, i.flow) == ("add", 2, 2, "next")

def test_decode_jsr_absolute_long_is_call_with_target():
    i = decode(bytes.fromhex("4EB9000CBCC8"), 0)
    assert (i.flow, i.targets, i.size) == ("call", (0xCBCC8,), 6)

def test_decode_dbf_targets_itself():
    i = decode(bytes.fromhex("51C8FFFE"), 0)
    assert (i.mnemonic, i.flow, i.targets) == ("dbf", "cond", (0,))

def test_decode_rts_and_jmp_indirect():
    assert decode(bytes.fromhex("4E75"), 0).flow == "ret"
    assert decode(bytes.fromhex("4ED0"), 0).flow == "indirect"     # jmp (a0)

def test_decode_rejects_odd_pc_and_illegal_opcode(): ...           # DecodeError carries pc

def test_discover_follows_branch_and_call_but_not_data():
    # 0: bsr.s +4 ; 2: rts ; 4: dc.w $FFFF (data) ; 6: moveq #0,d0 ; 8: rts
    rom = bytes.fromhex("6104" "4E75" "FFFF" "7000" "4E75")
    assert sorted(discover(rom, [0])) == [0, 2, 6, 8]

def test_discover_uses_jump_table_annotation(): ...                # jmp (a0) at 0 with jump_tables={0: [4]} reaches 4

def test_verify_rr206_names_both_hashes():
    with pytest.raises(WrongRomError) as e:
        verify_rr206(b"\0" * 0x100000)
    assert "c7a60901" in str(e.value) and e.value.actual_sha256 in str(e.value)
```

- [ ] **Step 2: Run and see them fail**

Run: `uv run --group dev pytest tests/port/test_decode.py tests/port/test_discover.py -v`
Expected: FAIL on import.

- [ ] **Step 3: Implement `decode.py` and `discover.py`**

Decode with capstone (`CS_ARCH_M68K`, `CS_MODE_M68K_000`, detail on) and normalise into `Insn`. Any capstone result that uses a 68020-only encoding is a `DecodeError`. If Task 6 exposes a capstone misdecode, add a local override in `decode.py` with a test, and note it for Task 8.

- [ ] **Step 4: Run tests, then a ROM smoke check**

Run: `uv run --group dev pytest tests/port -v` → PASS.
Run (needs ROM and traces): `uv run python -c "..."` calling `discover` with annotations plus both windows' `trace_entries`, and asserting every traced PC is a key of the result.
Expected: assertion holds; print the instruction count and the mnemonic histogram of traced instructions. Save the histogram to `generated/port/traced-mnemonics.txt` for Task 5.

- [ ] **Step 5: Commit**

```bash
git add src/rr2port tests/port port/annotations pyproject.toml uv.lock
git commit -m "Add 68000 decode and code discovery for the port translator"
```

---

### Task 5: Translator back end (C emission and cycle costs)

**Files:**
- Create: `src/rr2port/cycles.py`, `src/rr2port/emit.py`, `src/rr2port/cli.py`, `tests/port/harness.py`, `tests/port/test_emit.py`, `tests/port/test_cli.py`
- Modify: `pyproject.toml` (scripts `rr2-translate = "rr2port.cli:translate_main"`, `rr2-trace-report = "rr2port.cli:report_main"`)

**Interfaces:**
- Consumes: `Insn`, `discover`, `load_annotations`, `trace_entries`, `verify_rr206` (Task 4); `rr_machine.h`, `rr_mem.h`, `rr_run.h` (Task 3).
- Produces:
  - `emit_insn(insn: Insn) -> str`: a C function `static void i_<pc:06X>(RrMachine *m)` that performs the instruction exactly (result, all five condition codes, address-register side effects, memory accesses in the 68000's order), sets `m->cpu.pc` to the successor, and adds its cycle cost to `m->cpu.cycles`. `insns` is incremented by `rr_step`, not here.
  - `emit_program(insns: Mapping[int, Insn], rom_crc32: int) -> str`: all instruction functions, `rr_lookup` (direct-mapped table indexed by `pc >> 1`), and `rr_code_rom_crc32`.
  - `base_cycles(insn: Insn) -> int` and, for data-dependent costs (taken/not-taken branches, `dbcc`, `scc`, shifts by register, `mulu`/`muls`/`divu`/`divs`, `movem`), a C expression produced by `cycles_expr(insn) -> str`. Costs follow the standard MC68000 timing tables.
  - CLI: `rr2-translate ROM --annotations port/annotations/rr206.toml [--trace-seed DIR ...] -o generated/port/rr206_code.c`. Exits 2 with the `WrongRomError` message for any other ROM, and refuses an output path outside an ignored directory (`generated/`).
  - `tests/port/harness.py`: `run_snippet(code: bytes, *, d=(), a=(), sr=0x2700, ram: Mapping[int, bytes] = {}, steps=1) -> Result` where `Result` has `d`, `a`, `pc`, `sr`, `cycles` and `ram(addr, n)`. It places `code` at address 0 of a synthetic ROM, translates it, compiles with the core, runs, and parses the result. Skips the test when no C compiler is found.

Scope: implement every mnemonic and addressing mode that appears in `generated/port/traced-mnemonics.txt`. Any other instruction is emitted as a call to `rr_fatal(m, "unimplemented %s", ...)` so it fails loudly if reached. `stop` and privileged SR changes that appear in the traces must be exact; `rr_step` on a stopped CPU is out of scope unless the traces show it.

- [ ] **Step 1: Write failing tests** in `tests/port/test_emit.py` (CCR is the low byte of `sr`: X=0x10, N=0x08, Z=0x04, V=0x02, C=0x01):

| Test | Code (hex) | Before | After |
|---|---|---|---|
| `test_add_word_overflow` | `D041` | `d0=0x7FFF, d1=1` | `d0=0x8000`, CCR `0x0A`, cycles 4 |
| `test_add_word_preserves_upper_half` | `D041` | `d0=0x1234FFFF, d1=1` | `d0=0x12340000`, CCR `0x15` |
| `test_sub_byte_borrow` | `9001` | `d0=0, d1=1` | `d0=0xFF`, CCR `0x19` |
| `test_moveq_negative_keeps_x` | `70FF` | CCR `0x10` | `d0=0xFFFFFFFF`, CCR `0x18` |
| `test_lsl_word_shifts_out_carry` | `E348` | `d0=0x8000` | `d0=0`, CCR `0x15` |
| `test_mulu_word` | `C0C1` | `d0=0xFFFF, d1=0xFFFF` | `d0=0xFFFE0001`, CCR `0x08` |
| `test_divu_word` | `80C1` | `d0=7, d1=2` | `d0=0x00010003`, CCR `0x00` |
| `test_dbf_loops_then_falls_through` | `51C8FFFE` | `d0=1`, 2 steps | after step 1 `pc=0, d0.w=0`; after step 2 `pc=4, d0.w=0xFFFF` |
| `test_movem_predecrement_order` | `48E7C000` | `d0=0x11111111, d1=0x22222222, a7=0xFFFF08` | `a7=0xFFFF00`; RAM at `0xFFFF00` = `11111111 22222222` |
| `test_jsr_pushes_return_address_into_ram` | `4EB900000008` then `4E71 4E75` | `a7=0xFFFF04` | `pc=8`, `a7=0xFFFF00`, RAM long at `0xFFFF00` = `0x00000006` |
| `test_rts_returns` | `4E75` | `a7=0xFFFF00`, RAM long `0x00000006` | `pc=6`, `a7=0xFFFF04` |
| `test_unimplemented_instruction_is_fatal` | an opcode outside the traced set | | process exits 2, stderr has `RR2FATAL` and `unimplemented` |

and in `tests/port/test_cli.py`: `test_translate_refuses_wrong_rom` (a 1 MiB zero file → exit 2, message names both hashes, no output file written) and `test_translate_refuses_output_outside_generated`.

- [ ] **Step 2: Run and see them fail**

Run: `uv run --group dev pytest tests/port/test_emit.py tests/port/test_cli.py -v`
Expected: FAIL on import.

- [ ] **Step 3: Implement `cycles.py`, `emit.py`, `cli.py` (translate only) and the harness.**

Write shared flag helpers once as `static inline` functions in a header emitted at the top of the generated file (add/sub/logic/shift flag computation per size), and have instruction functions call them.

- [ ] **Step 4: Run tests**

Run: `uv run --group dev pytest tests/port -v && uv run --group dev ruff check .`
Expected: PASS, clean.

- [ ] **Step 5: Translate the real ROM**

Run: `uv run rr2-translate "Road Rash II (USA, Europe) (RR206).gen" --annotations port/annotations/rr206.toml --trace-seed generated/port/traces/boot --trace-seed generated/port/traces/race -o generated/port/rr206_code.c && make -C port code`
Expected: file written; `make -C port code` compiles it with no warnings. `git status --short` shows nothing under `generated/`.

- [ ] **Step 6: Commit**

```bash
git add src/rr2port tests/port pyproject.toml port/Makefile
git commit -m "Add C emission and cycle costs to the port translator"
```

---

### Task 6: Trace conformance checker

**Files:**
- Create: `port/tools/rr_trace_check.c`, `tests/port/test_trace_check.py`
- Modify: `port/Makefile` (target `trace-check` → `generated/port/build/rr-trace-check`)

**Interfaces:**
- Consumes: everything from Tasks 2, 3 and 5.
- Produces: `rr-trace-check --rom ROM --window DIR [--slice N] [--save-restore-at K] [--cycles-out FILE]`

Behaviour:
1. Load the ROM; refuse (exit 2) unless its CRC-32 equals `rr_code_rom_crc32` and `0x0876E992`.
2. Initialise `RrMachine` from the first trace line's registers and `ram-start.bin`; open `hw.txt` as replay hardware.
3. For each trace line `k` except the last: if registers, SR or PC differ from the machine, print `DIVERGE insn=<k> pc=<hex> field=<name> want=<hex> got=<hex>` and exit 1. Then `rr_step`. Then, if line `k+1` is an interrupt entry (same rule as `find_interrupts`), call `rr_interrupt(level)`.
4. Each time the PC about to execute is `0xDF48` and `handoff-NN.bin` exists for that occurrence, compare all 65,536 bytes of RAM; on difference print `RAMDIFF handoff=<NN> first_offset=<hex> want=<hex> got=<hex> count=<n>` and exit 1.
5. At the end: compare against the final trace line and `ram-end.bin`, require the hardware log fully consumed, and print `OK insns=<n> interrupts=<n> handoffs=<n> hw=<n> seconds=<wall>`.
6. `--cycles-out` writes one line per instruction: `<k> <pc hex> <cycles charged>`.
7. A trace line that fails to parse is exit 2 with the line number; it is never treated as end of trace.

Interrupt detail for step 3: an interrupt is taken between instructions, so line `k+1` being a handler entry means instruction `k` ran to completion first. The checker executes `k`, then injects. Verify this against the pushed return address in the trace (the long at the new stack pointer + 2 equals the PC the machine would have executed next); a mismatch is a `DIVERGE` with `field=int_return`.

- [ ] **Step 1: Write failing tests** in `tests/port/test_trace_check.py` using a synthetic window built in `tmp_path` from a tiny ROM (the harness's snippet ROM: `moveq #1,d0 ; addq.w #1,d0 ; bra.s *`), with trace lines written by hand:
  - `test_matching_window_reports_ok`: exit 0, stdout starts with `OK insns=`.
  - `test_register_divergence_is_reported_with_index`: corrupt `d0` on trace line 2 → exit 1, `DIVERGE insn=2` and `field=d0`.
  - `test_truncated_trace_is_an_error_not_a_pass`: cut the last line in half → exit 2, message has the line number.
  - `test_unconsumed_hardware_log_fails`: extra `R` record → exit 1.
  - `test_ram_end_difference_fails`: flip a byte in `ram-end.bin` → exit 1, `RAMDIFF`.
  - `test_wrong_rom_is_refused`: pass a different ROM → exit 2.

The synthetic ROM needs a checker built against synthetic code; the test builds it through the harness with the real-ROM CRC check replaced by the generated `rr_code_rom_crc32` only (make the `0x0876E992` check a compile-time option, on by default).

- [ ] **Step 2: Run and see them fail**

Run: `uv run --group dev pytest tests/port/test_trace_check.py -v`
Expected: FAIL, checker source missing.

- [ ] **Step 3: Implement `rr_trace_check.c`.**

- [ ] **Step 4: Run tests**

Run: `uv run --group dev pytest tests/port -v`
Expected: PASS.

- [ ] **Step 5: Run against the real windows and fix the translator until both pass**

Run: `make -C port trace-check && generated/port/build/rr-trace-check --rom "Road Rash II (USA, Europe) (RR206).gen" --window generated/port/traces/boot` and the same with `race`.
Expected: `OK …` for both. Each `DIVERGE` names the first wrong instruction; fix its emission, add a row to the Task 5 table reproducing it, and rerun. Keep a list of every fix (mnemonic, what was wrong) for Task 8.

If a divergence is caused by something the replay approach cannot represent (for example a hardware access MAME's watchpoint does not report), stop and record it for Task 8 instead of working around it silently.

- [ ] **Step 6: Commit**

```bash
git add port tests/port src/rr2port
git commit -m "Add trace conformance checker; boot and race windows match MAME"
```

---

### Task 7: Suspend/resume and save/restore proof

**Files:**
- Modify: `port/tools/rr_trace_check.c`, `tests/port/test_trace_check.py`

**Interfaces:**
- Consumes: `rr_save`, `rr_load`, `rr_hw_replay_position`, `rr_hw_replay_seek` (Task 3).
- Produces: working `--slice N` and `--save-restore-at K`.

`--slice N`: the checker's main loop is restructured as `run_slice(state, n)` called repeatedly from `main`, returning to the caller after at most `N` instructions with no information kept in locals between calls. `--save-restore-at K`: at instruction `K`, write `rr_save` output plus the replay position and trace index to a temp file, zero the whole `RrMachine`, reload, and continue.

- [ ] **Step 1: Write failing tests**
  - `test_slicing_does_not_change_result`: synthetic window with `--slice 1` → same `OK` line (ignoring `seconds`) as without.
  - `test_save_restore_mid_run_does_not_change_result`: `--save-restore-at 2` → `OK`.
  - `test_save_restore_inside_interrupt_handler`: synthetic window with one injected interrupt; restore point is the first handler instruction → `OK`.

- [ ] **Step 2: Run and see them fail**

Run: `uv run --group dev pytest tests/port/test_trace_check.py -v`
Expected: FAIL, options not recognised.

- [ ] **Step 3: Implement both options.**

- [ ] **Step 4: Run tests, then the real windows**

Run: `uv run --group dev pytest tests/port -v` → PASS.
Run the race window three ways: `--slice 1`, `--slice 997`, and `--save-restore-at` set to the index of an instruction inside an HBlank or VBlank handler (take the index from `find_interrupts` + 3).
Expected: `OK` with identical counts each time.

- [ ] **Step 5: Commit**

```bash
git add port tests/port
git commit -m "Prove suspend/resume and save/restore at arbitrary instruction boundaries"
```

---

### Task 8: Timing analysis and findings

**Files:**
- Modify: `src/rr2port/cli.py` (`report_main`), `tests/port/test_report.py` (create)
- Create: `analysis/port-spike-findings.md`

**Interfaces:**
- Consumes: `read_trace`, `find_interrupts` (Task 2); `--cycles-out` file (Task 6).
- Produces: `rr2-trace-report WINDOW_DIR --ours CYCLES_FILE`, printing:
  1. **Cycle agreement:** for each mnemonic, instructions compared, and how many had `ours != MAME delta` (MAME delta = next line's `totalcycles` minus this line's). Lists the ten largest disagreements with PC.
  2. **Interrupt placement:** for each interrupt: index, level, MAME `totalcycles`, MAME cycles since the previous interrupt of that level, our cycles over the same span.
  3. **Summary:** for level 6, min/max/mode of the MAME spacing and of ours; fraction of all instructions whose cost agrees.

- [ ] **Step 1: Write failing tests** in `tests/port/test_report.py` with hand-built steps:
  - `test_cycle_agreement_counts_mismatches`: three steps with MAME deltas 4, 8; ours 4, 10 → one mismatch reported for that mnemonic.
  - `test_interrupt_spacing`: two level-6 interrupts 1000 MAME cycles apart → spacing 1000.
  - `test_report_handles_zero_totalcycles`: all `cycles == 0` → prints `MAME cycle data unavailable` and skips sections 1 and 3's cycle parts without error.

- [ ] **Step 2: Run and see them fail**, then **Step 3: implement `report_main`**, then **Step 4: run tests** (`uv run --group dev pytest tests/port -v` → PASS).

- [ ] **Step 5: Produce the reports**

Run the checker with `--cycles-out` on both windows, then `rr2-trace-report` on each. Also time the race window: instructions per second from the `OK` line.

- [ ] **Step 6: Write `analysis/port-spike-findings.md`**

Use the verified/hypothesis convention. Required sections, each answered from the evidence produced above:

1. **Result:** did both windows match, and over how many instructions, interrupts, handoffs and hardware accesses.
2. **Suspending and resuming:** what the per-instruction dispatch model costs (instructions per second, ratio to the original's roughly 1–2 million per second) and whether block-level grouping is needed for speed.
3. **Interrupt delivery and cycle costs:** cycle agreement rate; which instruction classes disagree and why (data-dependent costs, bus stalls, DMA).
4. **Can reference timing place interrupts exactly:** is MAME's VBlank spacing constant in CPU cycles; how far our count drifts over a frame; a recommendation between exact cycle placement and the injected-timestep fallback for Stage 2.
5. **Translator fixes made:** the list kept in Task 6, including any capstone overrides.
6. **Spike shortcuts that the full translator must replace:** trace-seeded discovery, replayed hardware, traced-only instruction coverage.
7. **Anything the replay approach could not represent.**
8. **Recommendation:** proceed to full translation as designed, or the changes needed to the design first.

- [ ] **Step 7: Commit and stop for review**

```bash
git add src/rr2port tests/port analysis/port-spike-findings.md
git commit -m "Add port spike timing report and findings"
```

The spec requires the owner to review these findings before the rest of Stage 1 is planned. Do not start full translation in this plan.
