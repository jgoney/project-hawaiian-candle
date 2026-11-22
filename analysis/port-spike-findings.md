# Native port spike: findings

Date: 2026-11-10. Plan: `docs/superpowers/plans/2026-11-10-port-spike.md`.
Design: `docs/superpowers/specs/2026-11-10-native-port-design.md`.

Statements are **verified** (supported by a run recorded here) or a
**hypothesis** (an interpretation that still needs a test), as in the other
notes. "MAME" means MAME 0.289's `genesis` driver; it is the reference, not
real hardware.

## 1. Result (verified)

RR206 68000 code translated mechanically to C reproduces MAME instruction for
instruction over both reference windows.

| Window | Instructions | Interrupts | Loop handoffs | Hardware accesses | Result |
| --- | ---: | ---: | ---: | ---: | --- |
| `boot` (from `0x206` to the 30th VBlank after `0xCBCC8`) | 5,381,187 | 434 | 0 | 243,507 | match |
| `race` (`full-throttle`, handoffs 500–520 at `0xDF48`) | 1,506,888 | 884 | 21 | 55,866 | match |

"Match" means all of the following held for the whole window:

- before every instruction, PC, SR, D0–D7 and A0–A7 equal the trace;
- every hardware access the code made had the recorded address and size, in the
  recorded order, and every hardware write had the recorded value;
- every recorded hardware access was consumed;
- all 65,536 bytes of work RAM equal MAME's at each of the 21 handoffs and at
  the end of each window. Handoffs 00 and 20 coincide with the window's start
  and end images, so 19 of the 21 are independent intermediate checkpoints.

The race window includes 754 HBlank (level 4) and 130 VBlank (level 6)
interrupts taken in the middle of game code, including back-to-back HBlanks.

Reproduce with:

```sh
./scripts/run-mame-port-trace-rr206.sh boot
./scripts/run-mame-port-trace-rr206.sh race
uv run rr2-translate "Road Rash II (USA, Europe) (RR206).gen" \
  --annotations port/annotations/rr206.toml \
  -o generated/port/rr206_code.c
make -C port trace-check
generated/port/build/rr-trace-check --rom "Road Rash II (USA, Europe) (RR206).gen" \
  --window generated/port/traces/race
```

(During the spike the translator was also given `--trace-seed` options naming
both windows. That shortcut has since been removed; see
`analysis/code-discovery.md`.)

Limits of this result:

- Hardware was replayed, not modelled. This proves the CPU translation, not a
  video or sound model.
- The windows execute 7,422 distinct instruction addresses out of 20,530
  translated. The rest are untested.
- RAM is compared at handoffs and at the end, not after every instruction. A
  wrong write that is overwritten before the next handoff would not be seen
  in RAM, only through its effect on registers.
- The reset instruction at `0x200` is not in the boot trace (the debugger
  script starts after it).

## 2. Suspending and resuming (verified)

Model: one C function per 68000 instruction, a lookup table indexed by address,
and a loop that calls one function at a time. All state is in `RrMachine`:
registers, SR, PC, cycle and instruction counters, and the 64 KiB RAM image,
which holds the original stack including return addresses. Nothing is kept on
the native call stack between instructions.

- The race window gives the same result when the run returns to its caller
  after every instruction (`--slice 1`) and every 997 instructions.
- Saving the whole state to a file, wiping the machine, and reloading gives the
  same result three instructions into an HBlank handler (instruction 1,769),
  three into a VBlank handler (11,105), and at instruction 3,000,000 of boot.

Speed: the checker ran the race window in 0.73 s and boot in 2.4 s, about 2
million instructions per second, while also parsing a text trace and comparing
18 registers per instruction. The race window is 130 frames, or 2.17 s of
original time, so this is at least three times real time with the checking
overhead included. Execution alone was not measured separately. Grouping
instructions into blocks is not needed for speed at 60 updates per second
(hypothesis until measured without the checker).

## 3. Interrupt delivery and cycle costs (verified)

Interrupts are delivered between instructions by calling `rr_interrupt`, which
pushes PC and SR into RAM and loads the vector. Handlers return with their own
`RTE`. No special handling was needed for interrupts landing mid-routine.

Three cases decide where an interrupt lands, and all occur in RR206:

- ordinary entry, when the mask is below the interrupt's level;
- entry immediately after an instruction lowers the mask from 7 (first seen at
  `0x33E` in boot);
- entry immediately after an `RTE`, giving back-to-back HBlank handlers (first
  seen at `0x15D8` in the race).

Cycle costs from the standard 68000 timing tables, compared with MAME's
`totalcycles` for every instruction not followed by an interrupt:

| Window | Compared | Agree | Share |
| --- | ---: | ---: | ---: |
| `boot` | 5,380,753 | 5,373,830 | 99.871% |
| `race` | 1,506,004 | 1,503,169 | 99.812% |

Every one of the 67 distinct mnemonics executed agrees everywhere, including the data-dependent
ones (multiply, divide, shifts, `DBcc`, bit operations), except `move` at a few
addresses that write to hardware:

| Address | Instruction | Extra MAME cycles | Count |
| --- | --- | --- | ---: |
| `0x2F742`, `0x2C34E`, `0x2C388`, `0x250` and five more | byte write to Z80 RAM (`0xA0xxxx`) | +1 | 9,699 |
| `0x12300` | `move.w ($FFFACE).l,($C00004).l` (VDP control port) | +3 to +13 | 60 |

Hypothesis: the +1 is a bus wait on the Z80 area, and the VDP write starts a
DMA that holds the 68000 for a data-dependent time. Neither is an instruction
cost; both belong to the hardware model.

## 4. Can reference timing place interrupts exactly?

Not from cycle counting alone (verified), but the gap is small and looks
modellable (hypothesis).

- MAME's VBlank spacing in the race window is 127,890–128,120 cycles, most
  often 128,000. It is not constant because an interrupt waits for the current
  instruction to finish.
- Our cycle count over the same spans is 58–142 cycles short per frame.
- MAME takes 49–58 cycles from the end of the interrupted instruction to the
  handler's first instruction. The textbook figure is 44, which is what the
  runtime charges. Hypothesis: the extra 5–14 cycles is the autovector
  E-clock synchronisation, which depends on the cycle count modulo 10 and
  could be modelled exactly.
- The rest of the shortfall is the hardware stalls in section 3.

So exact placement needs an interrupt-entry timing model plus VDP DMA and Z80
bus timing, which is a cycle-accurate hardware model in all but name.

**Recommendation for Stage 2:** use two checks, because they answer different
questions.

- **Trace-driven conformance**, as done here: interrupts are injected where
  MAME took them and hardware reads are replayed. It is exact and needs no
  hardware model, and it found every problem in this spike within one run. It
  is the check for the CPU translation. It cannot test the build's own
  hardware models (it replays hardware), it needs an instruction trace that is
  too large for a whole race (section 6), and it cannot use the existing
  `full-throttle` and `wild-thing-controls` RAM captures.
- **The design's injected-timestep check** stays as designed: feed the recorded
  timestep and inputs per loop pass, run on the build's own hardware models,
  and compare RAM at each handoff over whole captures. It is the check for the
  assembled game.

What this spike changes is only the design's first choice for reference
timing. Exact self-timed interrupt placement should not be a Stage 2
requirement. If it is wanted later, validate it statistically (same timestep
histogram as the 3:7 4:294 5:911 6:721 7:200 8:41 9:2 distribution in
`bike-physics.md`), not bit for bit.

## 5. Translator fixes made

No instruction's translation had to be corrected: once interrupts were
injected in the right places, both windows matched on the first complete run.
Changes made on the way:

- `abcd` (packed decimal add) was unimplemented and is used in the race window
  (2 executions). Added, with the N and V results following the commonly
  documented behaviour; the trace agrees.
- Interrupt detection in the checker was changed twice (the second and third
  cases in section 3).
- capstone was dropped before any code depended on it. Its Python detail for
  68000 omits absolute addresses and base registers for several addressing
  modes, so the translator has its own decoder (`src/rr2port/decode.py`).

Emitted behaviours that the traces are consistent with but do not prove in
isolation: the dummy read before `clr`, `Scc` and `move sr` to memory, and
the high-word-first order of long writes.

Known simplifications that only matter when the target is hardware, and that
neither window exercised (an independent review identified them):

- long writes are always high word first; a real 68000 writes the low word
  first for `move.l` to `-(An)` and for `movem` with predecrement;
- `movem` from memory omits the 68000's extra trailing word read;
- the interrupt stack frame is written in a simplified order;
- `link a7` pushes the value before the decrement;
- the N and Z results of an overflowing `divu`/`divs` are unverified.

The decoder rejects addressing modes the 68000 does not allow for each
instruction, so discovery stops when it strays into data. Adding that check
did not change the instruction counts above, so discovery was not walking
into data in these runs.

## 6. Spike shortcuts the full translator must replace

- **Trace-seeded discovery.** Following control flow from the 17 annotated
  entry points finds 19,931 instructions but misses 375 of the 7,422 traced
  addresses. Those are reached through indirect jumps and calls (jump tables
  and callback slots). The spike added every traced address as an entry point.
  The full translator needs jump tables in `port/annotations/rr206.toml`, most
  likely extracted with Ghidra's help and checked against traces.
- **Replayed hardware.** There is no video chip, sound chip, Z80 or controller
  model. Stage 1 needs them to produce a picture and sound.
- **Instruction coverage.** 11 `stop` instructions among the discovered code
  are not implemented (none executed in either window). `movep`, `sbcd`,
  `nbcd`, `tas`, `chk`, `trap`, `trapv`, `rtr` and `reset` are also
  unimplemented and fail loudly if reached. Divide by zero and any switch to
  user mode are fatal errors, not exceptions.
- **Text traces.** A window is 250 MB to 1 GB of text and several minutes of
  MAME time. Whole-race conformance needs a compact binary format or
  checkpoint-only comparison.

## 7. What the replay approach could not represent

Nothing was encountered. Three MAME debugger behaviours had to be worked
around and are documented in `mame/README.md`: bare numbers are hexadecimal,
the `pc` symbol is one word ahead, and a trace started inside a breakpoint
begins with the following instruction.

One thing the approach cannot show by construction: whether hardware *read
values* depend on timing the port would get wrong. That is a hardware-model
question for Stage 1.

## 8. Recommendation

Proceed to full translation as designed, with three adjustments to the design:

1. **Execution model.** Keep per-instruction functions with table dispatch and
   all state in the machine structure. It satisfies the "nothing on the native
   call stack" rule and is fast enough. The design's "organised by original
   routine" then describes how the generated file is laid out, not how it
   executes.
2. **Conformance.** Use trace-driven conformance (section 4) as the check for
   the CPU translation, keep the design's injected-timestep check for the
   assembled game, and drop exact self-timed interrupt placement as a Stage 2
   requirement.
3. **Code discovery is the next real problem.** The translation itself is not
   the risk it appeared to be; finding all the code is. Plan the jump-table
   work first, and keep the loud failure on any untranslated address.

Next milestone after that: a minimal video model so the boot window produces
the title screen picture, per the design's milestone order.
