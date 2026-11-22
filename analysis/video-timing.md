# Video timing and the live window: findings

Date: 2026-11-10. Plan: `docs/superpowers/plans/2026-11-10-godot-window-and-video-timing.md`.
Statements are **verified** or a **hypothesis**. "MAME" is MAME 0.289's
`genesis` driver.

## 1. Result (verified)

- The natively translated game runs in a Godot window, one console frame per
  60 Hz tick (`scripts/run-godot-console.sh`). The Godot side is a native
  extension written in plain C; no binding library or extra build tool was
  needed. Verified by headless tests: the class works when called from
  scripts, the picture after 600 frames equals the
  command-line runner's, and the input mapping produces the right pad bits
  from simulated key actions. **Not yet verified:** real keyboard or gamepad
  use by a person; that is the owner's session.
- A scripted run (`godot/tests/probe_front_end.gd`) that presses Start every
  90 frames goes from power-on through the title into **a race**, and the
  port draws it: road, rival riders, the flag waver, the mirrors and the
  dashboard. 5,400 frames ran without reaching untranslated code or
  unmodelled hardware (verified, reproducible with that script). That the
  race frames are correct is a judgement by eye; nothing has compared them
  with MAME yet.
- **Owner's play session, 2026-11-10 (verified by the owner, not by an
  automated check):** two complete races played start to finish in the Godot
  window, Alaska and Hawaii at level 1 on the standard bike, with everything
  working "about as you'd expect" and nothing halting. This is the first
  native use of steering, throttle, braking and a race's end-of-race path.
  It is a judgement of look and feel, not a comparison with MAME.
- The picture offset from the last round is explained and gone (section 2).
- Game memory now tracks MAME closely (section 3).

## 2. The one-frame picture offset (verified)

`first-picture-findings.md` reported that MAME's snapshot at the Nth VBlank
handler entry equals the port's picture at entry N−1, and left open whether
the port was a frame early. It is not.

MAME's video memory (registers, VRAM, CRAM, VSRAM) was dumped with the
debugger stopped at entry N and drawn by the port's renderer, at two entries
where the picture is changing:

| Entry | MAME's memory vs MAME's snapshot | vs port at N−1 | vs port at N | vs port at N+1 |
| --- | ---: | ---: | ---: | ---: |
| 120 (logo animating) | 949 pixels differ | 949 | **0** | 955 |
| 660 (credit line changing) | 1,142 | 1,142 | **0** | 0 |

So MAME's snapshot shows the frame before its own video memory, and drawing
MAME's video memory at entry N gives the port's picture at entry N. Two
limits on that statement: the memories were compared through the picture, not
byte for byte, so off-screen video memory could still differ; and only entry
120 separates N from N+1 (at 660 both are identical).
The dump was calibrated first: at entry 434 its work RAM is byte-identical to
the boot trace window's `ram-end.bin`.

The reference to use from now on is MAME's video memory rendered by the
port's renderer (`scripts/run-mame-vdp-state-rr206.sh`), not MAME's snapshot.
That checks the game and the hardware model, but not the renderer against
MAME's renderer; the 20 snapshot comparisons, at the one-entry offset, remain
the check of the renderer itself.

## 3. Game memory against MAME (verified)

Work RAM of the self-timed native boot compared with MAME's at the same
VBlank handler entry:

| Entry | Bytes differing (of 65,536) |
| --- | ---: |
| 120 | 0 |
| 434 | 3 (was 83 before this plan) |
| 660 | 1 |

The remaining differences are where an interrupt landed within the
foreground code, as before.

## 4. Timing fitted to MAME

From `generated/port/hv/race.txt` (44,353 counter reads and 884 interrupt
entries logged with MAME's beam position) and the race trace window:

- **Line length (verified).** MAME's frame interrupts are 127,890–128,120
  cycles apart, most often 128,000 or 128,010, with a mean of 128,005.66.
  262 lines of 3420/7 = 488 4/7 cycles is 128,005.71. The model now uses
  3420/7 per line, and a frame no longer grows by the overshoot of its last
  instruction. The model's frame is a whole 128,005 cycles, so it is 4/7 of a
  cycle short of MAME's and slips about one line against MAME every 14
  seconds; the same truncation makes the H counter wrong for one cycle at
  the very end of each frame (both found by review, not yet fixed).
- **Interrupt entry time (verified).** MAME takes 49–58 cycles to enter an
  interrupt handler, and it is not random: the handler always starts when
  MAME's cycle count is 5 modulo 10 (all 884 interrupts). That is the 68000
  synchronising autovectored interrupts to its E clock, which runs at a tenth
  of the CPU clock. `rr_interrupt` now does the same in place of a flat 44.
- **Interrupt position (verified for the frame interrupt).** Handlers are
  entered on the line the V counter shows, about 100 cycles after the counter
  stepped to it. The model raises interrupts 36 cycles into the line; the
  rest is entry time and the instruction in progress. The frame interrupt is
  always entered on line 224.
- **V counter (verified).** It is the line number from the start of the
  line. In MAME's coordinates: hardware line = (beam line + 38) mod 262, and
  the counter steps at beam column 462.
- **H counter (not fitted).** MAME's value advances about one count per CPU
  cycle and is not a function of beam column alone. Hardware advances 210
  counts in 488 cycles. The model keeps the hardware rate. Hypothesis: MAME's
  H counter is approximate. The race code reads the H byte 754 times in the
  window, in the line interrupt handler, and in shadow mode the model's value
  differs from MAME's on 733 of those reads. Whether the game's behaviour
  depends on the exact value is not known; this is the largest known
  difference between the model and MAME on something the race reads.

Effect, measured on the race trace window. These are replay measurements:
MAME decides where each interrupt lands and the port's cycle count is
compared between those points. They show the port's cycle accounting, not
where the self-timed model would place interrupts.

| | Before | After |
| --- | --- | --- |
| Cycles between interrupts, ours minus MAME | −6 to −142 | line interrupts: 0 for 677 of 753; frame interrupts: 0 for 53 of 129; −70 at worst |
| V counter (high byte of 43,599 word reads) agreeing in shadow mode | 88.2% | 95.9% |
| Instruction costs agreeing | 99.812% | 99.812% |

The remaining shortfall is the two hardware stalls found in the spike: one
extra cycle on byte writes to the Z80's memory, and the 68000 held while a
video DMA runs. Neither is modelled.

## 5. Other model corrections

Made with tests, from the last review:

- The frame-interrupt status flag stays set until the interrupt is
  acknowledged; a status read no longer clears it.
- Sprite vertical position is 9 bits.
- No line interrupt is pending before the game has programmed the counter.

**Not changed:** on hardware a register write also changes the address and
code registers. Nothing recorded from MAME shows which behaviour it has
(reading MAME's source for the control port would settle that, and has not
been done), every picture and both trace windows agree with the current
code, and the race runs, so it was left alone. Open. It would matter for code that writes data straight after a register
write without setting the address again.

## 6. What is still unverified about the race

- **No comparison with MAME yet.** The race frames are judged by eye. The
  check to build next is the one from section 2: dump MAME's video memory and
  RAM at a VBlank entry during a recorded race and compare.
- **Input in a race.** The owner has now ridden two full races natively
  (section 1). Still not exercised or not reported on: the other three
  tracks, higher levels, other bikes, the bike shop, getting busted or
  wrecked, and two-player mode.
- **H counter and DMA stall timing** (section 4).
- **Frame pacing.** Godot runs up to four console frames per drawn frame, so
  the game keeps its speed on a display slower than 60 Hz and a long stall is
  dropped. On a 59.94 Hz display that means an occasional doubled frame.
- **Shadow/highlight, 32-cell mode, 30-row mode, interlace** render magenta
  if the game ever selects them. None appeared in 5,400 frames.
- **Sound.** There is none.

## 7. Recommendation

1. The owner plays it: `scripts/build-godot-extension.sh`, then
   `scripts/run-godot-console.sh`. Anything that halts shows an address or a
   hardware register; anything that looks or feels wrong is worth a note.
2. Race conformance against MAME: replay `full-throttle` natively by feeding
   the recorded inputs, and compare RAM and video memory at VBlank entries,
   as the design's whole-capture check describes.
3. Then the experiment the project has been waiting for: let the game loop
   run every frame instead of about every fifth, and see whether the race
   stays the same speed.
