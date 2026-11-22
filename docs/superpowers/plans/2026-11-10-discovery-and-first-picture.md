# Code Discovery and First Picture Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the translator find RR206's code from annotations alone, then add a video model good enough that the boot stretch produces the same picture as MAME from the port's own hardware code.

**Architecture:** Part A extends the annotation file with the three ways the game reaches code indirectly (callback slots, pointer tables, observed jump targets), fills it using a MAME logger that records indirect targets over the existing input recordings, and removes trace seeding from the translator. Part B adds a hardware layer (`port/hw`) behind the existing `RrHw` interface: video chip ports, memories and DMA, a scanline renderer, and stubs for the controller, Z80 bus and PSG. It is validated in two steps: first in "shadow" beside the replayed hardware on the existing trace windows, then alone in a self-timed boot run whose picture must equal MAME's snapshot.

**Tech Stack:** Python ≥3.12 (uv, pytest, ruff, Pillow in the dev group for reading PNG); C11 with make; MAME 0.289 debugger scripts; Ghidra 12.0.4 as a reading aid only.

**Spec:** `docs/superpowers/specs/2026-11-10-native-port-design.md` (Stage 1 milestones 1 and 2, "Complete state", "Hardware shim"). Background: `analysis/port-spike-findings.md`.

## Global Constraints

- Target ROM is RR206 only, SHA-256 `c7a609019b1f052bcee6204a3a09ea77a5150a748de90b51382fedf4df207848`, CRC-32 `0876E992`. Every tool that reads a ROM verifies this.
- Nothing ROM-derived is committed: no ROM bytes, generated C, traces, captures, pictures or dumps. They live under the ignored `generated/`. Annotation files hold addresses, counts and notes only.
- The build must not depend on Ghidra or on MAME output. The annotation file alone carries the code map.
- Reaching an untranslated address at run time stays a fatal error naming the address and the jumping instruction.
- Third-party code in `port/` must carry a BSD/MIT/zlib-class licence. This plan adds none: the video model is written here.
- All state that determines future behaviour lives in `RrMachine` or the hardware object it points to. No static mutable variables in `port/core`, `port/hw` or generated code.
- C11, `-std=c11 -Wall -Wextra -Werror -O2`, no macOS-only APIs outside test code.
- Tests needing the ROM, MAME output or a compiler skip cleanly when absent; `uv run --group dev pytest` and `make -C port test` pass on a clean checkout.
- Python is ruff clean (line length 100). Run `ruff format` only on `src/rr2port` and `tests/port`.
- MAME debugger scripts write every number with a `0x` prefix (bare numbers are hexadecimal), and need `-debugger osx`.
- Claims about the original game in `analysis/` are labelled verified or hypothesis.

## Review Focus

1. **Bad indirect target** (odd address, outside the ROM, or not decodable) arriving from a MAME log or an annotation: rejected with a message naming the site and target; never silently added or silently dropped. Tests in Task 1 and Task 3.
2. **Mistyped annotation key** (`pointer_table`, `callback_slot`, a missing `count`): the loader fails and names the key; it does not ignore it. Test in Task 1.
3. **DMA edge cases**: length 0 (means 65,536 units), destination wrapping past `0xFFFF`, source wrapping within its 128 KiB window: no out-of-bounds access, addresses wrap. Test in Task 6.
4. **Sprite list that never ends** (a link field pointing back into the list): the renderer stops after 80 sprites. Test in Task 8.
5. **No reference picture** (a boot window recorded before this plan, or a PNG of the wrong size): the picture check fails with a message saying so; it never passes by default. Test in Task 8.

## File Structure

```
port/annotations/rr206.toml      + callback_slots, [[pointer_tables]], [jump_tables] filled
port/hw/rr_vdp.h  rr_vdp.c       video chip: registers, ports, VRAM/CRAM/VSRAM, DMA, status, HV
port/hw/rr_vdp_render.c          scanline renderer (planes A/B, window, sprites, priority)
port/hw/rr_genesis.h rr_genesis.c  RrHw for the whole machine: routes to the VDP; controller,
                                 Z80 bus and PSG stubs; frame timing
port/tools/rr_trace_check.c      + --shadow-hw, --frame-out
port/tools/rr_boot_run.c         self-timed boot runner
port/tests/test_vdp.c            C unit tests for the video model
src/rr2port/discover.py          + callback slots, pointer tables, indirect_sites, strict loader
src/rr2port/code_report.py       coverage and indirect-site report
src/rr2port/indirect.py          MAME indirect-target logger script + log merge
src/rr2port/picture.py           PNG/PPM loading and comparison
scripts/run-mame-indirect-log-rr206.sh
analysis/code-discovery.md       written in Task 4
analysis/first-picture-findings.md  written in Task 9
```

---

## Part A: code discovery

### Task 1: Annotation kinds for indirectly reached code

**Files:**
- Modify: `src/rr2port/discover.py`, `src/rr2port/cli.py`, `tests/port/test_discover.py`

**Interfaces (produces):**

```python
@dataclass(frozen=True)
class PointerTable:
    address: int        # ROM address of the first entry
    count: int          # number of entries
    kind: str           # "long" (32-bit absolute) or "word_offset" (16-bit signed, relative to base)
    base: int = 0       # for word_offset
    site: int = 0       # the indirect instruction that uses it, for reports
    note: str = ""

@dataclass
class Annotations:
    entries: list[int]
    jump_tables: dict[int, list[int]]     # site -> targets observed at run time
    callback_slots: list[int]             # RAM addresses that hold code pointers
    pointer_tables: list[PointerTable]

class AnnotationError(ValueError): ...    # message names the file key or the bad value

def load_annotations(path: Path) -> Annotations
def pointer_table_targets(rom: bytes, table: PointerTable) -> list[int]
def discover(rom, entries, jump_tables=None, *, callback_slots=(), pointer_tables=()) -> dict[int, Insn]
def indirect_sites(insns: Mapping[int, Insn]) -> list[int]   # jmp/jsr with no static target, sorted
```

Rules:
- `load_annotations` accepts exactly the keys `entries`, `callback_slots`, `pointer_tables`, `jump_tables`; table entries accept exactly `address`, `count`, `kind`, `base`, `site`, `note`, with `address`, `count`, `kind` required. Anything else is `AnnotationError`.
- A callback slot makes the immediate of every discovered `move.l #imm,<slot>` (absolute short or long destination equal to the slot, compared in 24 bits) a code entry.
- Every target from any source must be even, inside the ROM, and decodable; otherwise `discover` raises `AnnotationError` naming the source (site, slot or table address) and the target. Plain `entries` keep today's behaviour (undecodable entries are skipped) because vector slots may point at unused stubs.
- `rr2-translate` passes all four kinds through.

- [ ] **Step 1: Write failing tests** in `tests/port/test_discover.py`:

```python
def test_callback_slot_makes_moved_immediates_code():
    # 0: move.l #$10,$ff0300.l ; a: rts ; 10: rts
    rom = bytes.fromhex("23FC00000010" "00FF0300" "4E75").ljust(0x10, b"\xff") + bytes.fromhex("4E75")
    assert sorted(discover(rom, [0])) == [0, 0xA]
    assert sorted(discover(rom, [0], callback_slots=[0xFF0300])) == [0, 0xA, 0x10]

def test_callback_slot_matches_absolute_short_form():
    # move.l #$10,($0300).w sign-extends to $000300, which is not the slot $FF0300
    ...  # 21FC 00000010 0300 -> not seeded;  21FC 00000010 8300 with slot 0xFF8300 -> seeded

def test_long_pointer_table_targets():
    rom = bytes(8) + bytes.fromhex("00000004" "00000006")
    table = PointerTable(address=8, count=2, kind="long")
    assert pointer_table_targets(rom, table) == [4, 6]

def test_word_offset_pointer_table_targets():
    rom = bytes.fromhex("0004" "FFFE")
    assert pointer_table_targets(rom, PointerTable(0, 2, "word_offset", base=0x100)) == [0x104, 0xFE]

def test_pointer_table_running_past_the_rom_is_an_error(): ...   # AnnotationError names address 0x...

def test_odd_or_undecodable_indirect_target_is_an_error():
    rom = bytes.fromhex("4ED0" "FFFF")
    for target in (1, 2, 0x100000):
        with pytest.raises(AnnotationError) as e:
            discover(rom, [0], {0: [target]})
        assert f"{target:06X}" in str(e.value) and "000000" in str(e.value)

def test_indirect_sites_lists_only_unresolved_jumps():
    # 0: jmp (a0) ; 2: jsr $8.l ; 8: jsr 2(pc,d0.w) ; c: rts
    rom = bytes.fromhex("4ED0" "4EB900000008" "4EBB0002" "4E75")
    assert indirect_sites(discover(rom, [0, 2])) == [0, 8]

@pytest.mark.parametrize("text, key", [
    ("callback_slot = [1]\n", "callback_slot"),
    ("[[pointer_table]]\naddress = 1\n", "pointer_table"),
    ("[[pointer_tables]]\naddress = 8\nkind = 'long'\n", "count"),
    ("[[pointer_tables]]\naddress = 8\ncount = 1\nkind = 'short'\n", "short"),
])
def test_load_annotations_rejects_unknown_or_incomplete_keys(tmp_path, text, key): ...
```

- [ ] **Step 2: Run and see them fail** — `uv run --group dev pytest tests/port/test_discover.py -q` → failures on missing names.
- [ ] **Step 3: Implement** to the interfaces and rules above.
- [ ] **Step 4: Run** `uv run --group dev pytest tests/port -q && uv run --group dev ruff check .` → all pass.
- [ ] **Step 5: Commit** — `git commit -m "Add callback slots and pointer tables to code discovery"`

---

### Task 2: Code report

**Files:**
- Create: `src/rr2port/code_report.py`, `tests/port/test_code_report.py`
- Modify: `src/rr2port/cli.py`, `pyproject.toml` (script `rr2-code-report = "rr2port.cli:code_report_main"`)

**Interfaces:**
- Consumes: `discover`, `indirect_sites`, `Annotations`, `pointer_table_targets`, `trace_entries`.
- Produces:

```python
@dataclass
class SiteRow:
    site: int
    operand: str            # e.g. "jsr (a0)"
    table_targets: int      # from pointer tables naming this site
    observed_targets: int   # from jump_tables
    resolved: bool          # has a pointer table, or is listed in jump_tables

@dataclass
class CodeReport:
    instructions: int
    code_bytes: int
    sites: list[SiteRow]
    missing: dict[str, list[int]]   # window name -> traced addresses not discovered

def build_code_report(rom: bytes, annotations: Annotations, windows: Mapping[str, set[int]]) -> CodeReport
def format_code_report(report: CodeReport) -> str
```

- CLI: `rr2-code-report ROM --annotations FILE [--window DIR ...]`. Exit 0 when every site is resolved and no window has missing addresses; exit 1 otherwise; exit 2 for a wrong ROM or bad annotations.

- [ ] **Step 1: Write failing tests**: a ROM with one resolved and one unresolved site gives `sites` of length 2 with the right `resolved` flags; a window containing an undiscovered address lists it under `missing`; `format_code_report` contains the line `unresolved indirect sites: 1`; CLI returns 1 for that case and 0 when the jump table is added.
- [ ] **Step 2: Run and see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run tests**, then the baseline on the real ROM:

Run: `uv run rr2-code-report "Road Rash II (USA, Europe) (RR206).gen" --annotations port/annotations/rr206.toml --window generated/port/traces/boot --window generated/port/traces/race`
Expected today: 19,931 instructions, 87,104 code bytes, 9 unresolved sites (`0011DC 00E130 018D6E 018EB4 019A26 019BCC 019D1C 02C416 0CBDEE`), 375 missing addresses in total, exit 1. Record the output in the commit message.

- [ ] **Step 5: Commit** — `git commit -m "Add code coverage and indirect-site report"`

---

### Task 3: Record indirect targets from MAME

**Files:**
- Create: `src/rr2port/indirect.py`, `scripts/run-mame-indirect-log-rr206.sh`, `tests/port/test_indirect.py`
- Modify: `src/rr2port/cli.py`, `pyproject.toml` (script `rr2-annotate-indirect`), `mame/README.md`

**Interfaces:**
- Consumes: `indirect_sites`, `Insn`, `Operand`, `load_annotations`.
- Produces:

```python
def target_expression(insn: Insn) -> str
    # MAME debugger expression for where a jmp/jsr will go, numbers 0x-prefixed:
    #   ind      -> "a3"                     (a7 is written "sp")
    #   disp     -> "(a3+0x10)"              (negative displacements as "-0x10")
    #   index    -> "(a3+0x4+X)"  where X is "d0" for a long index or
    #               "(((d0&0xffff)^0x8000)-0x8000)" for a word index
    #   pcindex  -> "(0x1004+X)"
def build_indirect_cmd(sites: Mapping[int, Insn]) -> str
    # one breakpoint per site: bp <site>,1,{logerror "RR2IND %06X %06X\n",<site>,<expr>&0xffffff; g}
def read_indirect_log(error_log: Path) -> dict[int, set[int]]      # TraceFormatError on a malformed RR2IND row
def merge_jump_tables(annotation_text: str, observed: Mapping[int, set[int]], rom: bytes) -> str
    # returns the annotation file text with [jump_tables] replaced by the union of what was
    # there and `observed`; keys and targets sorted, hex, one site per line. [jump_tables]
    # must be the file's last section; raises AnnotationError otherwise, and for any target
    # that is odd, outside the ROM or undecodable (message names site and target).
```

- `scripts/run-mame-indirect-log-rr206.sh <capture> [ROM]`: verifies the ROM hash, generates the script from the current annotations, plays back `generated/mame/<capture>.inp` with `-exit_after_playback -nothrottle -sound none`, and writes the unique `site target` pairs to `generated/port/indirect/<capture>.txt`.
- `rr2-annotate-indirect ANNOTATIONS LOG... --rom ROM` rewrites the annotation file in place and prints how many new targets were added per site.

- [ ] **Step 1: Write failing tests**: `target_expression` for each of the four operand kinds including `a7`, a negative displacement and a word index; `build_indirect_cmd` has one `bp` line per site and ends with `g`; `read_indirect_log` ignores other rows and rejects `RR2IND 0011DC`; `merge_jump_tables` keeps existing targets, adds new ones sorted, leaves everything above `[jump_tables]` byte-identical, and raises `AnnotationError` for target `0x000201` (odd), `0x100000` (outside) and an undecodable address.
- [ ] **Step 2: Run and see them fail.**
- [ ] **Step 3: Implement** the module, script and CLI.
- [ ] **Step 4: Run tests** → pass.
- [ ] **Step 5: Run to a fixed point on the three existing recordings**

Repeat until a round adds no target and `rr2-code-report` shows no new unresolved site:

```sh
for c in full-throttle wild-thing-controls controller-test; do ./scripts/run-mame-indirect-log-rr206.sh $c; done
uv run rr2-annotate-indirect port/annotations/rr206.toml generated/port/indirect/*.txt --rom "Road Rash II (USA, Europe) (RR206).gen"
uv run rr2-code-report "Road Rash II (USA, Europe) (RR206).gen" --annotations port/annotations/rr206.toml --window generated/port/traces/boot --window generated/port/traces/race
```

Expected at the fixed point: no missing addresses in either window; every site that executed is listed in `[jump_tables]`. Sites that never executed in any recording stay unresolved until Task 4. Record the rounds needed and the final counts.

- [ ] **Step 6: Document** the workflow in `mame/README.md` and **commit** — `git commit -m "Record indirect jump targets from MAME and merge them into annotations"`

---

### Task 4: Resolve the sites and drop trace seeding

**Files:**
- Modify: `port/annotations/rr206.toml`, `src/rr2port/cli.py` (remove `--trace-seed`), `tests/port/test_cli.py`, `README.md`
- Create: `analysis/code-discovery.md`

This is reverse-engineering work. For each indirect site (the nine listed in Task 2 plus any found since), read the surrounding code with `ghidra/scripts/ReconRange.java` and decide how its targets are produced:

- **Pointer table in ROM** (the site loads an address from a table): add a `[[pointer_tables]]` entry with the table's address, entry count, kind and base, and a note saying how the count was established (a bounds check in the code, the next table's address, or a terminator). A count taken only from "it looks like code after that" must be labelled hypothesis in the note.
- **Callback slot** (the site calls through a RAM pointer that code fills with immediates): add the slot to `callback_slots`. `0xFF0300` is already known to be one (`0x11DC`, verified in `initial-findings.md`).
- **Computed some other way** (pointers inside data structures, addresses built arithmetically): leave the site on observed targets in `[jump_tables]` and say so.

`analysis/code-discovery.md` has one row per site: address, instruction, mechanism, how its targets are known (table / slot / observed only), target count, and verified or hypothesis. It ends with the totals from `rr2-code-report` and a list of sites that are still "observed only", which is the remaining risk of missed code.

- [ ] **Step 1: Write the failing test** in `tests/port/test_cli.py`: `test_translate_has_no_trace_seed_option` — `translate_main([rom, "--trace-seed", "x", "-o", out])` exits with argparse's status 2 and "unrecognized arguments".
- [ ] **Step 2: Run and see it fail.**
- [ ] **Step 3: Do the per-site analysis**, updating the annotation file and writing `analysis/code-discovery.md`. After each change, rerun `rr2-code-report`; new code brings new sites, which get the same treatment. Rerun Task 3's loop once at the end.
- [ ] **Step 4: Remove `--trace-seed`** from `rr2-translate` and from `README.md`/`analysis/port-spike-findings.md` reproduction commands.
- [ ] **Step 5: Verify from annotations alone**

```sh
uv run rr2-translate "Road Rash II (USA, Europe) (RR206).gen" --annotations port/annotations/rr206.toml -o generated/port/rr206_code.c
make -C port trace-check
for w in boot race; do generated/port/build/rr-trace-check --rom "Road Rash II (USA, Europe) (RR206).gen" --window generated/port/traces/$w; done
uv run rr2-code-report "Road Rash II (USA, Europe) (RR206).gen" --annotations port/annotations/rr206.toml --window generated/port/traces/boot --window generated/port/traces/race
```

Expected: both windows print `OK` with the same counts as before (`insns=5381187 interrupts=434 handoffs=0 hw=243507` and `insns=1506888 interrupts=884 handoffs=21 hw=55866`); the report shows no missing addresses and exits 0, or exits 1 only because of sites documented as never executed and not statically resolvable, each listed in `analysis/code-discovery.md`.

- [ ] **Step 6: Commit** — `git commit -m "Resolve indirect jump sites from annotations; remove trace seeding"`

---

## Part B: first picture

### Task 5: Reference picture for the boot window

**Files:**
- Modify: `src/rr2port/mame_cmd.py`, `scripts/run-mame-port-trace-rr206.sh`, `tests/port/test_mame_cmd.py`, `pyproject.toml` (dev group: `pillow>=11`), `mame/README.md`
- Create: `src/rr2port/picture.py`, `tests/port/test_picture.py`

**Interfaces (produces):**
- Boot window gains `frame-end.png`: MAME's `snap` taken in the same breakpoint action that writes `RR2END` (the frame that has just completed when the 30th VBlank handler is entered). The script passes `-snapshot_directory` pointing at the window directory and fails if the file is missing.
- `load_rgb(path: Path) -> tuple[int, int, bytes]` (width, height, RGB bytes) for `.png` (Pillow) and binary `.ppm` (P6, own parser).
- `compare_pictures(want: Path, got: Path) -> PictureDiff` with `same: bool`, `size_want`, `size_got`, `differing_pixels: int`, `first: tuple[int, int] | None`. Different sizes give `same=False` without comparing pixels. A missing `want` raises `FileNotFoundError` naming it.
- CLI `python -m rr2port.picture WANT GOT`: prints the diff, exit 0 only when identical.

- [ ] **Step 1: Write failing tests**: boot script contains `snap ` before `quit` and no `snap` in the race script; `load_rgb` round-trips a 2×2 PPM written by hand and a 2×2 PNG written with Pillow; `compare_pictures` reports one differing pixel and its coordinates; different sizes are not `same`; missing reference raises.
- [ ] **Step 2: Run and see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run tests**, then re-record the boot window: `./scripts/run-mame-port-trace-rr206.sh boot`.
Expected: `frame-end.png` exists; note its size (320×224 expected; if MAME adds borders, record the active-area crop in `mame/README.md` and add it to `load_rgb` callers as a named constant). `rr-trace-check` on the new window still prints `OK`.
- [ ] **Step 5: Commit** — `git commit -m "Capture a reference picture with the boot window"`

---

### Task 6: Video chip ports, memories and DMA

**Files:**
- Create: `port/hw/rr_vdp.h`, `port/hw/rr_vdp.c`, `port/tests/test_vdp.c`
- Modify: `port/Makefile` (`test` also builds and runs `test_vdp`)

**Interfaces (produces):**

```c
typedef struct {
    uint8_t reg[24];
    uint8_t vram[0x10000];
    uint16_t cram[64];      /* 9-bit colours as written, 0000 BBB0 GGG0 RRR0 */
    uint16_t vsram[40];
    uint16_t addr;          /* current address register */
    uint8_t code;           /* CD5..CD0 */
    uint8_t pending;        /* 1 after the first half of a two-word command */
    uint8_t fill_pending;   /* a VRAM fill is waiting for its data word */
    uint16_t first_word;
    uint16_t status;        /* sticky status bits */
} RrVdp;

void rr_vdp_reset(RrVdp *v);
void rr_vdp_write_control(RrVdp *v, RrMachine *m, uint16_t value);  /* m is the DMA source bus */
void rr_vdp_write_data(RrVdp *v, uint16_t value);
uint16_t rr_vdp_read_data(RrVdp *v);
uint16_t rr_vdp_read_status(RrVdp *v, int in_vblank, int in_hblank);  /* clears pending */
int rr_vdp_save(const RrVdp *v, FILE *f);
int rr_vdp_load(RrVdp *v, FILE *f);
```

Behaviour to implement: register writes (`100r rrrr dddd dddd`, ignored for registers above 23); two-word address/code set; data port writes to VRAM (byte order: high byte at `addr`, low byte at `addr ^ 1`), CRAM and VSRAM with the register 15 auto-increment; data port reads; and DMA when register 1 bit 4 is set and the command has CD5: 68000-to-VDP copy (source = registers 21–23 × 2, length = registers 19–20 words, 0 meaning 65,536, source wrapping within its 128 KiB block), VRAM fill, and VRAM copy. Registers 19–23 hold their end values after a DMA.

The exact byte pattern of VRAM fill and the address behaviour of VRAM copy are easy to get subtly wrong from memory. Their tests assert only memory safety and length here; their exact results are pinned by the picture and shadow checks in Tasks 7–9, and any correction found there gets a unit test added to this file.

- [ ] **Step 1: Write failing tests** in `port/tests/test_vdp.c`:
  - `test_register_write`: control `0x8F02` → `reg[15] == 2`; control `0x9F00` leaves all registers unchanged.
  - `test_vram_write_and_autoincrement`: `0x8F02`, control `0x4000`, `0x0000`, data `0x1234`, data `0x5678` → `vram[0..3] == 12 34 56 78`, `addr == 4`.
  - `test_vram_write_to_odd_address_swaps_bytes`: address 1, data `0x1234` → `vram[1] == 0x12`, `vram[0] == 0x34`.
  - `test_cram_and_vsram_write`: control `0xC000`,`0x0000`, data `0x0EEE` → `cram[0] == 0x0EEE`; control `0x4000`,`0x0010`, data `0x0123` → `vsram[0] == 0x0123`.
  - `test_cram_address_wraps_and_vsram_ignores_out_of_range`: CRAM address `0x80` writes `cram[0]`; VSRAM address `0x50` (index 40) writes nothing.
  - `test_second_control_word_is_not_a_register_write`: `0x4000` then `0x8F04` sets the address/code and leaves `reg[15]` alone.
  - `test_status_read_clears_pending`: `0x4000`, read status, then `0x8F04` is a register write.
  - `test_dma_68k_copy`: machine RAM at `0xFF0000` = `11 22 33 44`; `0x8114` (DMA enable), `0x8F02`, `0x9302`, `0x9400` (length 2), `0x9500`, `0x9680`, `0x977F` (source `0xFF0000 >> 1`), control `0x4000`,`0x0080` → `vram[0..3] == 11 22 33 44`, `reg[19] == 0 && reg[20] == 0`.
  - `test_dma_disabled_does_nothing`: same without the enable bit → VRAM unchanged.
  - `test_dma_length_zero_and_wrap_stay_in_bounds` (Review Focus 3): 68000 copy with length 0 from `0xFF0000` to address `0xFFFE` and a fill with length 0 complete without touching memory outside `vram`, verified with guard bytes around a heap-allocated `RrVdp`.
  - `test_vdp_save_load_round_trip`.
- [ ] **Step 2: Run and see it fail** — `make -C port test` → missing header.
- [ ] **Step 3: Implement `rr_vdp.c`.**
- [ ] **Step 4: Run** `make -C port test` → `all core tests passed` and `all vdp tests passed`.
- [ ] **Step 5: Commit** — `git commit -m "Add video chip ports, memories and DMA"`

---

### Task 7: Machine hardware and the shadow check

**Files:**
- Create: `port/hw/rr_genesis.h`, `port/hw/rr_genesis.c`
- Modify: `port/tools/rr_trace_check.c`, `port/Makefile`, `tests/port/test_trace_check.py`, `tests/port/harness.py` (build with `port/hw` sources)

**Interfaces:**
- Consumes: `RrVdp` API (Task 6), `RrHw` (existing).
- Produces:

```c
typedef struct {
    RrHw hw;                 /* must be first */
    RrMachine *m;
    RrVdp vdp;
    uint8_t z80_ram[0x2000];
    uint8_t pad_ctrl[2], pad_th[2];
    uint16_t pad_buttons[2]; /* active-high: bit0 up,1 down,2 left,3 right,4 B,5 C,6 A,7 Start */
    uint8_t z80_busreq, z80_reset;
    uint64_t frame_start_cycles;   /* m->cpu.cycles at the start of the current frame */
} RrGenesis;

RrGenesis *rr_genesis_new(RrMachine *m);
void rr_genesis_free(RrGenesis *g);
#define RR_CYCLES_PER_LINE 488
#define RR_LINES_PER_FRAME 262
#define RR_VBLANK_LINE 224
```

Address map (24-bit): `0xA00000–0xA0FFFF` Z80 RAM mirrored every 8 KiB (YM2612 at `0xA04000–0xA04003` and other Z80-side devices: writes ignored, reads 0); `0xA10001` version = `0xA0`; `0xA10003/5` controller data (three-button pad protocol on TH); `0xA10009/B` controller control; `0xA11100` bus request (a read returns bit 0 clear once requested); `0xA11200` Z80 reset; `0xA14000` TMSS (writes ignored); `0xC00000–0xC00003` data port; `0xC00004–0xC00007` control/status; `0xC00008–0xC0000F` HV counter from `m->cpu.cycles - frame_start_cycles`; `0xC00011` PSG (ignored). Any other address is `rr_fatal` naming it.

Checker additions:
- `--shadow-hw`: hardware reads still come from the recording, which stays authoritative. Every access is also given to an `RrGenesis`. For reads, the model's answer is compared with the recording and counted. At the end the checker prints one line per address that was read, `SHADOW addr=<hex> reads=<n> differ=<n>`, then the usual `OK` line. Differences are reported, not fatal: read values depend on timing the shadow model does not have.
- The shadow model's frame timing is driven from the trace: `frame_start_cycles` is reset each time a level 6 interrupt is injected.

- [ ] **Step 1: Write failing tests**: C unit tests in `port/tests/test_vdp.c` (same binary) for `rr_genesis`: version register reads `0xA0`; a word written to `0xC00000` after an address set lands in `vdp.vram`; bus request then read returns bit 0 clear; unidentified address `0xB00000` is fatal; pad with no buttons reads `0x7F` with TH high and `0x33` with TH low (three-button pad, active-low lines). Python: synthetic window whose code reads `0xA10001` and writes the VDP, run with `--shadow-hw`, prints `SHADOW addr=A10001 reads=1 differ=0`; with the recording's value changed to `0x80` it prints `differ=1` and still exits 0.
- [ ] **Step 2: Run and see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run tests**, then both real windows with `--shadow-hw`.
Expected: `OK` lines unchanged. Record the `SHADOW` lines. For boot, the reads are the version register, the controller ports, the bus request and two status reads: any `differ` count above zero for the version register, controller ports or bus request is a model bug to fix now (add a unit test reproducing it). Status and HV differences are expected and are recorded for Task 9's findings.
- [ ] **Step 5: Commit** — `git commit -m "Add machine hardware model and shadow check against recorded hardware"`

---

### Task 8: Renderer and the picture check

**Files:**
- Create: `port/hw/rr_vdp_render.c`
- Modify: `port/hw/rr_vdp.h`, `port/tools/rr_trace_check.c` (`--frame-out FILE`), `port/tests/test_vdp.c`, `tests/port/test_trace_check.py`

**Interfaces (produces):**

```c
#define RR_SCREEN_W 320
#define RR_SCREEN_H 224
/* Render one display line into 320 0x00RRGGBB pixels. */
void rr_vdp_render_line(const RrVdp *v, int line, uint32_t *out);
/* Render the whole frame and write a binary PPM (P6). Returns 0 on success. */
int rr_vdp_write_ppm(const RrVdp *v, const char *path);
uint32_t rr_vdp_colour(uint16_t cram_value);   /* 9-bit colour to RGB */
```

Scope: 40-cell mode (register 12 = `0x81`), 28 rows; planes A and B with plane size from register 16, horizontal scroll modes from register 11 (full screen, per cell, per line) read from the table at register 13, vertical scroll full screen and per two cells; window plane (registers 17, 18); sprites from the table at register 5 with link order, size, flips, the 80-sprite and per-line limits; priority between planes and sprites; backdrop colour (register 7); display disabled (register 1 bit 6 clear) shows the backdrop. Shadow/highlight (register 12 bit 3) and interlace are out of scope: if either is enabled, `rr_vdp_render_line` fills the line with magenta `0xFF00FF` so it cannot pass a picture check silently.

`rr_vdp_colour` uses MAME's level table for the 315-5313 so pictures can be compared exactly; take the eight levels from the reference picture (the distinct channel values in `frame-end.png` and further snapshots) and record them, with that provenance, in a comment.

`--frame-out FILE` (only with `--shadow-hw`): after the run, write the shadow model's frame as PPM.

- [ ] **Step 1: Write failing tests** in `port/tests/test_vdp.c`:
  - `test_backdrop_fills_a_blank_display`: register 7 = `0x05`, `cram[5] = 0x000E` → every pixel of line 0 is `rr_vdp_colour(0x000E)`.
  - `test_plane_a_tile_pixel`: a tile with pixel (0,0) = colour 1 placed at cell (0,0) of plane A, `cram[1] = 0x00E0` → pixel (0,0) is that colour, pixel (8,0) is backdrop.
  - `test_tile_flips`, `test_horizontal_scroll_full_screen`, `test_vertical_scroll`, `test_high_priority_plane_b_over_low_priority_plane_a`.
  - `test_sprite_position_and_size`: sprite at screen (16,8) (table values x=144, y=136), size 2×2 cells → covers x 16–31, y 8–23.
  - `test_sprite_link_cycle_terminates` (Review Focus 4): sprite 0 links to 1, 1 links to 0 → `rr_vdp_render_line` returns and draws each once.
  - `test_shadow_highlight_renders_magenta`.
  And in `tests/port/test_trace_check.py`: `--frame-out` writes a 320×224 PPM; `python -m rr2port.picture` exits non-zero with "no reference picture" when `frame-end.png` is absent and with the sizes when it is the wrong size (Review Focus 5).
- [ ] **Step 2: Run and see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run tests**, then the real check:

```sh
generated/port/build/rr-trace-check --rom "Road Rash II (USA, Europe) (RR206).gen" --window generated/port/traces/boot --shadow-hw --frame-out generated/port/boot-shadow.ppm
uv run python -m rr2port.picture generated/port/traces/boot/frame-end.png generated/port/boot-shadow.ppm
```

Expected: identical. Any difference is a model bug: find it with systematic-debugging, starting from the first differing pixel, fix it, and add a unit test in `port/tests/test_vdp.c` that reproduces it (this is where VRAM fill and copy get their exact tests).

- [ ] **Step 5: Commit** — `git commit -m "Add scanline renderer; boot picture matches MAME in shadow mode"`

---

### Task 9: Self-timed boot and findings

**Files:**
- Create: `port/tools/rr_boot_run.c`, `tests/port/test_boot_run.py`, `analysis/first-picture-findings.md`
- Modify: `port/hw/rr_genesis.h`, `port/hw/rr_genesis.c`, `port/Makefile` (target `boot-run`), `README.md`

**Interfaces:**
- Produces in `rr_genesis`:

```c
/* Run until the start of the next frame, delivering VBlank (level 6, when register 1
 * bit 5 is set) at line 224 and line interrupts (level 4, register 0 bit 4, counter in
 * register 10) from the machine's own cycle count. Interrupts are taken between
 * instructions when the CPU's mask allows. Returns instructions executed. */
uint64_t rr_genesis_run_frame(RrGenesis *g);
```

- `rr-boot-run --rom ROM [--frames N | --until-boot-window-end] -o FRAME.ppm [--ram-out FILE]`: starts from the reset vector with RAM zeroed, uses `RrGenesis` as the only hardware, no controller input. `--until-boot-window-end` stops at the same point as the boot trace window: the 30th entry to `0x11B8` after the first execution of `0xCBCC8`. Prints `DONE frames=<n> insns=<n> seconds=<wall>`.

- [ ] **Step 1: Write failing tests** in `tests/port/test_boot_run.py` with a synthetic ROM built through the harness: vector 1 → code that enables the display and VBlank interrupt, sets a backdrop colour, then loops; a VBlank handler that increments a RAM byte. After `--frames 3` the RAM byte is 3 and the PPM is all backdrop. A second test: interrupts masked (SR `0x2700`) → the RAM byte stays 0. A third: wrong ROM for the code → exit 2.
- [ ] **Step 2: Run and see them fail.**
- [ ] **Step 3: Implement** `rr_genesis_run_frame` and the tool.
- [ ] **Step 4: Run tests**, then the real boot:

```sh
make -C port boot-run
generated/port/build/rr-boot-run --rom "Road Rash II (USA, Europe) (RR206).gen" --until-boot-window-end -o generated/port/boot-native.ppm --ram-out generated/port/boot-native-ram.bin
uv run python -m rr2port.picture generated/port/traces/boot/frame-end.png generated/port/boot-native.ppm
cmp -l generated/port/traces/boot/ram-end.bin generated/port/boot-native-ram.bin | wc -l
```

Expected: the picture is identical to MAME's. RAM differences are counted and explained: each differing range is traced to a cause (for example a frame counter or a value read from the status port) or reported as unexplained. If the picture differs, debug it; if it differs only because the self-timed run is a frame ahead or behind, show that by matching against `--frames N±1` and record it.

- [ ] **Step 5: Write `analysis/first-picture-findings.md`** (verified/hypothesis convention): what picture the boot window ends on; shadow results for both windows (the `SHADOW` table); model bugs found and fixed; the picture result; the RAM comparison with causes; what the race window's 43,599 line-counter reads and status differences imply for the race milestone; and what is still stubbed (Z80, YM2612, PSG, six-button pads, shadow/highlight, interlace).
- [ ] **Step 6: Convert the picture for the owner** — `uv run python -c "from PIL import Image; Image.open('generated/port/boot-native.ppm').resize((1280, 896), Image.NEAREST).save('generated/port/boot-native.png')"` — and give its full path in the final message.
- [ ] **Step 7: Commit and stop for review** — `git commit -m "Add self-timed boot runner; native boot picture matches MAME"`
