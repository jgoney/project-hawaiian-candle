# Race conformance: a native race against the original, pass by pass

Date: 2026-11-10. Plan: `docs/superpowers/plans/2026-11-10-race-conformance-and-smoothness.md`.
Statements are **verified** or a **hypothesis**. "MAME" is MAME 0.289's
`genesis` driver, which is what "the original" means here: the reference
recordings were made on it.

## 1. Result (verified)

The recorded race `full-throttle` (one race, 2,176 passes of the game loop,
its last pass starting in frame 12,842) was played again by the native port, running on its own: its
own video timing, its own interrupts, nothing replayed except the controller
(the buttons the original read, fed to the same reads).

| Measure | Result |
|---|---|
| Passes reached | 2,176 of 2,176 |
| Passes taking the same number of frames as the original | **2,176 of 2,176** |
| Passes at whose start all 64 KB of work RAM equals the original's | **2,176 of 2,176** |
| Bytes hidden from the comparison | none |
| Frames per pass, native | 3:7 4:294 5:911 6:721 7:200 8:41 9:2 |
| Frames per pass, original | 3:7 4:294 5:911 6:721 7:200 8:41 9:2 |

The tool's verdict is `RESULT match`. Nothing is excluded from the comparison:
not the stack, not memory the interrupt handlers write. With `--compare-regs`
the same free run also has the original's cycle count, status register and all
sixteen registers at the start of every pass (2,176 of 2,176).

Restarting every pass from the original's exact state (memory, registers and
cycle count) and running one pass at a time gives the same picture from the
other side: all 2,176 passes end with every register and the cycle count
equal to the original's, and with identical memory.

Forcing the original's timestep into each pass (`--force-timestep`) changes
nothing, because the native timestep is already the original's every time.

Cross-check of the method: the native free run's memory at passes 500, 510
and 520 is byte-identical to the images MAME saved in the separate `race`
trace window.

### Instruction by instruction

`rr-race-replay --follow` compares every instruction the free-running port
executes with MAME's recorded trace: program counter, all sixteen registers
and the cycle count. All of these match to the last instruction:

| Trace window | Passes | Instructions |
|---|---|---|
| `race-start` | 2 to 14 | 995,949 |
| `race-early` | 14 to 44 | 2,603,045 |
| `race-66` | 66 to 80 | 995,000 |
| `race` | 500 to 520 | 1,506,889 |
| `race-822` | 822 to 824 | 115,053 |
| `race-953` | 953 to 955 | 151,136 |

That is 6,367,072 instructions, compared one by one, with no difference.

The boot recording matches for its first 2,523,391 instructions (power-on to
the first controller read in the menus) and then differs only because that
recording had no buttons pressed and this replay feeds it the race's.

Unchanged from before the timing work, re-run on the final code: the 20 boot
pictures are pixel-identical to MAME's snapshots, the port's picture equals a
render of MAME's own video memory at two checkpoints, and work RAM is
byte-identical at five boot checkpoints.

## 2. What it took: MAME's timing, exactly (verified)

The game is frame-locked: a pass of the race loop lasts a whole number of
frames, and the line-interrupt handler waits on the video chip's position
counter. A difference of one CPU cycle in the wrong place moves an interrupt
by one instruction, and a pass that takes 6 frames takes 7. So "the same
race" needed the hardware model to keep MAME's time to the cycle. The first
measurement, with the previous timing model, parted from the original at
pass 1 and never finished the race.

Each rule below was added with a failing test built from MAME's own numbers.

| Rule | Evidence |
|---|---|
| Video position is a function of absolute time alone. Time is counted in attoseconds as MAME counts it: a CPU cycle is 130,370,396,637 as (1 s / 7,670,453, rounded down), a line is 3,420 master clocks, and the video chip sees an access 56 master clocks after the instruction starts. The cycle count is 34 at the first instruction after power-on. | With these, the counter formulas give the value MAME returned for **all 187,920** counter reads in the six race trace windows. Rounding to whole master clocks first got 1 read in 16 wrong. |
| H counter: MAME's 0–479 position, reading 0x49 lower above 0xF7. V counter: the line, plus one beyond position 460. In the last CPU cycle of a line: position 480 and the line counted as over. | Same reads. This is MAME's counter, not the real chip's (which counts differently); it is modelled because MAME is the reference. |
| Both interrupts are raised 205 master clocks into their line. | About 1,600 interrupt entries: an instruction boundary at 204.996 never saw the interrupt, one at 205.078 did. |
| Interrupt entry takes 49–58 cycles and ends on cycle count 5 modulo 10 (the 68000's E clock). | From the earlier plan; holds for every interrupt in the windows. Counting from 34 at power-on was needed for it to hold in a free run. |
| An interrupt is taken after an instruction only if it was raised before the step where MAME's 68000 notes it (section 3). | The trace windows, instruction by instruction. |
| A byte write to Z80 memory costs one extra cycle. | Exact in the windows. |
| A 68000-to-VDP DMA stalls the CPU one cycle for every 228 words after the first word. | All 347 such DMAs in the windows, 61 different lengths (456 words cost 1 cycle, 2,512 cost 11, 2,736 still 11). They bound the rate between 228 and 228.36 words a cycle; that this exact form is MAME's formula is a **hypothesis**. |

## 3. Where the 68000 notes an interrupt (verified for the classes listed)

MAME's 68000 core follows the real chip's microcode. It notes a waiting
interrupt at one step inside each instruction (in MAME's source: where the
next opcode is moved into the decoder, `m68000gen.py`). For most instructions
that step begins the last four cycles, so an interrupt raised any time before
the instruction ends is taken after it. Some instructions differ, and the
race is sensitive to every one of them:

| Instructions | An interrupt is taken after the instruction if raised… | Measured on |
|---|---|---|
| Most | before it ends | ~1,300 entries |
| Shift or rotate of a register | within its first 8 cycles | 42 taken up to 8 cycles in, 12 missed from 9 |
| `mulu`, `muls` | within 8 cycles of fetching the operand | late from 9 cycles; taken at 5 |
| `divu`, `divs` | within 4 cycles of fetching the operand | missed at 6 cycles |
| Short conditional branch that falls through | within its first 4 cycles | missed at 6 and 7 cycles (2 entries) |
| `lea` with an index | up to 2 cycles before it ends | missed in its last 2 cycles (2 entries) |
| `cmp.l` to a data register, `cmpa.l`, `exg`, `btst` and `bset` on a data register | up to 2 cycles after it ends | 34 entries for `cmp.l`, one or more for each of the others |
| Same shape, not individually measured: `cmpa.w`, `bchg` on a data register, `clr.l`/`neg.l`/`negx.l`/`not.l` on a data register, `or.l`/`and.l`/`add.l`/`sub.l` from memory to a data register, `adda.l`/`suba.l` from memory | up to 2 cycles after it ends | **hypothesis**, supported in aggregate: adding this group took the passes that end with identical registers from 2,147 to 2,174 of 2,176 |

This table is fitted to what the recordings contain. It is not MAME's whole
microcode table: MAME's generator script would give that, and running it was
blocked by this environment's permission rules (it is code downloaded from
the internet). Instructions this race never happened to have an interrupt
land in could still be one instruction out.

## 4. Port bugs found and fixed (verified)

- The frame schedule was relative to each frame's start and the instruction
  that overran it; now absolute.
- Time arithmetic overflowed 64 bits eleven minutes into a session; now
  128-bit. (A play session longer than that would have had wrong counters.)
- The cycle count started at 0 where MAME's starts at 34, which put interrupt
  entry 4 cycles out in any free run from power-on. Three copies of the
  power-on sequence are now one (`rr_genesis_power_on`).
- Interrupt positions were approximate (190 and 200 master clocks); the H
  counter was not MAME's; the DMA stall was one cycle long for some lengths.

How the differences were found: five more trace windows were recorded from
MAME around the passes that still differed, and `--follow` named the first
instruction that parted each time. The count of passes ending with identical
registers went 876, 911, 1,674, 2,063, 2,140, 2,174, 2,176 (of 2,176) as the
rules in sections 2 and 3 went in.

## 5. What this does and does not show

**Shown:** for this one recorded race, the native port computes the same game
the original did: every pass in the same number of frames, the same memory at
the start of every one of the 2,176 passes, and in six stretches (6.4 million
instructions) every single instruction with the same registers at the same
cycle.

**Not shown:**

- One race, one track, one bike, a full-throttle run. No combat to speak of,
  no police stop, no crash recovery sequence, no two-player mode, no other
  tracks' code paths. A second recording with those in it is the next
  measurement worth making.
- The picture and the sound during the race were not compared. Video memory
  at race checkpoints has not been compared with MAME's (boot only).
- The reference is MAME, not a console. Where MAME differs from real
  hardware (the H counter, the stall, the clock rounding), the port now
  differs the same way. That is the right reference for "does it play like
  my recordings", and the wrong one for "is it hardware-accurate".
- The interrupt table in section 3 is complete for this race, not for the
  processor. Another race could put an interrupt inside an instruction this
  one never did.

**What can be claimed about "plays the same":** given the same button
presses, the port plays this race the same as the original, measured, not
judged by eye. For races that exercise other code, it is very likely the
same, and the tool to measure any new recording now exists:

```sh
./scripts/run-mame-handoff-capture-rr206.sh <recording>
make -C port race-replay
generated/port/build/rr-race-replay --rom "Road Rash II (USA, Europe) (RR206).gen" \
  --handoffs generated/port/handoffs/<recording>.bin \
  --timesteps generated/port/handoffs/<recording>.timesteps \
  --pads generated/port/handoffs/<recording>.pads
```

`RESULT match` means every pass started from identical memory. A difference
can be chased with `--regs … --resync --compare-regs` (one pass at a time
from the original's state) and `--follow K:TRACE` (instruction by
instruction against a trace window from
`scripts/run-mame-port-trace-rr206.sh`).

## Five more recordings (2026-11-14, verified)

The owner recorded five more sessions on MAME 0.289: four other tracks and
one ending in an arrest. Each was played back in MAME
(`run-mame-handoff-capture-rr206.sh`) and in the port (`rr-race-replay`, free
running, sound on, nothing excluded).

| Recording | Frames recorded | Passes compared | Same frames per pass | All 64 KB of RAM identical at pass start | Verdict |
|---|---|---|---|---|---|
| `arrested` | 9,854 | 1,476 | 1,476 | 1,476 | `RESULT match` |
| `vermont` | 14,954 | 2,476 | 2,476 | 2,476 | `RESULT match` |
| `arizona` | 16,700 | 2,905 | 2,905 | 2,905 | `RESULT match` |
| `tennessee` | 18,574 | 3,238 | 3,238 | 3,238 | `RESULT match` |
| `hawaii` | 22,410 | 2,697 | 2,697 | 2,697 | `RESULT match` |

Limits:

- The comparison ends at the last pass of the game loop the capture logs.
  Each recording goes on after that (between 14 and 36 seconds, by the
  port's sound length against the recorded frame count); that tail is **not
  compared**. What it contains (results or arrest screens) is a
  **hypothesis**.
- What happened in each race (crashes, fights, the police) is known only
  from the owner's account; the tool does not identify events.
- This compares memory, not the picture.
