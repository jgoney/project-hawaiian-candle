# The smoothness experiment: the race loop at higher CPU speeds

Date: 2026-11-10. Plan: `docs/superpowers/plans/2026-11-10-race-conformance-and-smoothness.md`.
Statements are **verified** or a **hypothesis**.

## The question

The race logic takes about 5.4 video frames a pass (about 11 updates a
second) and scales motion by the frames that went by (`analysis/bike-physics.md`).
The game looked CPU-bound. If the 68000 were faster, would the same race
simply update more often and look smoother?

## Short answer (verified for this race)

Partly, and not for free.

1. **More updates, yes, up to a ceiling.** With a faster CPU the loop runs
   about 2.7 times as often. It stops at one update every 2 frames (30 a
   second): the game never does a pass in a single frame, however fast the CPU.
2. **The race is not the same race.** The bike reaches the low gears at
   nearly the same moment (though already a little slower on the way) and takes
   much longer through the top ones, so it covers less ground. The game's
   motion is scaled by elapsed frames only approximately.
3. **Nothing broke.** None of the runs (200%, 400%, 800%) halted or reached
   anything the port does not handle. Other speeds were not run.

So a faster CPU alone is not a faithful way to get smoothness: it changes how
the bike performs. See the recommendation at the end.

## Method (verified)

`rr-race-replay --cpu-speed N --speed-from-handoff 0` runs the 68000 at N% of
its original speed from the first pass of the race on; the menus before it run
at the original speed (they are CPU-dependent too, and at other speeds the
recorded button presses no longer reach the race). The video chip's timing as
the game sees it is unchanged: same lines per frame, same interrupts, same
counters.

Because passes no longer line up with the recording, the controller is
replayed by frame: each read gets what the 100% run read in that frame
(`--pad-log`, `--inputs-by-frame`). Check of the method: at 100% the by-frame
run reproduces the pass-aligned baseline exactly (2,176 passes, same last
frame, same total motion).

Tracked at every pass (`--track`): player speed (`0xFF0A74`), the motion the
pass applied (`0xFF0A88`, speed × timestep), the gear (`0xFF0A7A`), and the
timestep. Each run is 12,843 frames: the recording's last pass starts in
frame 12,842.

```sh
B=generated/port/build/rr-race-replay; H=generated/port/handoffs/full-throttle
T="--track FF0A74:2:speed --track FF0A88:4:motion --track FF0A7A:2:gear"
$B --rom ROM --handoffs $H.bin --timesteps $H.timesteps --pads $H.pads \
   --report base.txt --pad-log pads-by-frame.txt $T
$B --rom ROM --handoffs $H.bin --timesteps $H.timesteps --pads $H.pads \
   --inputs-by-frame pads-by-frame.txt --cpu-speed 400 --speed-from-handoff 0 \
   --max-frames 12843 --report speed-400.txt $T
```

## Results (verified)

### How often the loop runs

| CPU speed | Passes in the race | Updates a second | Frames per pass (how many passes took that long) |
|---|---|---|---|
| 100% | 2,176 | 11.0 | 3:7 4:294 5:911 6:721 7:200 8:41 9:2 |
| 200% | 5,392 | 27.3 | 2:4,485 3:784 4:115 5:8 |
| 400% | 5,818 | 29.5 | 2:5,632 3:185 4:1 |
| 800% | 5,811 | 29.5 | 2:5,609 3:202 |

The frames add up to the same total in every run (11,822 to 11,824), so game
time still follows real frames.

No pass ever took one frame. The pass routine at `0xEEB8` begins by testing a
word through the pointer at `0xFF27B2` and returns at once if it is not zero;
at 800% the main loop calls it about 2,900 times per pass and is turned away.
**Hypothesis:** that word marks display data from the previous pass that the
frame interrupt handler has not finished taking, which needs two frame
interrupts.

### Whether it is the same race

Speed and distance at the same frame, with the same buttons held (the start
of the race is a straight full-throttle run):

| Frame | 100% | 200% | 400% | 800% |
|---|---|---|---|---|
| 1200: speed | 085E | 0770 | 07F8 | 0792 |
| 1500: speed | 2464 | 235F | 238D | 23A3 |
| 1500: distance so far (millions) | 1.99 | 1.90 | 1.93 | 1.93 |
| 1800: speed | 2EEF | 2777 | 29F5 | 2A5C |
| 1800: distance so far | 5.18 | 4.78 | 4.84 | 4.84 |

When each gear is first reached (frame), and the speed at that moment:

| Gear | 100% | 200% | 400% | 800% |
|---|---|---|---|---|
| 1 | 1322 (16A3) | 1316 (15C2) | 1312 (1586) | 1311 (156E) |
| 2 | 1384 (1CE6) | 1379 (1C19) | 1376 (1BFC) | 1377 (1C18) |
| 3 | 1439 (2169) | 1451 (2125) | 1445 (2103) | 1444 (20F5) |
| 4 | 1585 (267C) | 1686 (2567) | 1661 (2602) | 1656 (2624) |
| 5 | 1656 (2A84) | 1863 (2A64) | 1806 (2A49) | 1799 (2A5C) |

The shifts happen at the same speeds, so shifting is speed-driven and
unchanged. What changes is how long the bike takes to get there. It is
already slower early on (at frame 1200 its speed is 5% to 11% lower), yet
gears 1 to 3 arrive within a dozen frames of the original, but going from gear 3 to gear 5
takes 217 frames at 100% and 355 to 412 frames at the higher speeds. By frame
1800 the faster-CPU bikes are 7% behind.

**Hypothesis** for the cause: some part of the speed update is applied once
per pass instead of once per frame (a drag or rounding term that is not
multiplied by the timestep), so two and a half times as many passes take two
and a half times as much of it. It bites where the acceleration per frame is
smallest, in the top gears. The code is the player speed update described in
`analysis/bike-physics.md`; which term it is has not been identified.

After that the runs cannot be compared with this recording at all: the
recorded steering no longer fits where the bike is on the road, and it crashes
(first large speed loss at frame 4785 at 100%, but at frames 2469 to 2610 at
the higher speeds). Total motion over the 12,843 frames: 121.1 million at
100%; 64.2, 53.4 and 61.2 million at 200%, 400% and 800%. Those totals show
the divergence, not a property of the engine: a person steering live would not
crash because of it.

## What the plan's test says

The plan's test for "frame-rate independent" was the same distance and
finishing frame within 1% with timesteps of 1 to 2. It fails on distance (7%
behind after 800 frames of straight acceleration, before any steering
matters), and the timestep never reaches 1.

## What it should feel like (to be confirmed by the owner)

In the Godot window, keys **1 2 3 4** select 100%, 200%, 400% and 800% during
play; the speed is shown for two seconds. Expected from the numbers, not yet
seen by a person: the road and riders update about 30 times a second instead
of 11, steering and braking answer sooner (input is used by the next pass,
now at most 2 frames away instead of 5), and the bike feels a little weaker at
the top end. 400% and 800% should be indistinguishable from each other.

**Owner's impression (2026-11-10, played in the Godot window, several
races):** "It does feel noticeably smoother with the overclocking on, though
there are some visual artifacts (I think the equivalent of screen tearing?)."

The owner's screenshots of the artifacts show, in single frames:

- a one-pixel horizontal line in the ground or hill colour drawn across the
  sky, sometimes with a gap near the right edge;
- the far end of the road drawn off to one side of the horizon, detached from
  the near road;
- the whole picture displaced downwards by about a tenth of its height for a
  frame;
- garbled digits in the small counter at the right of the dashboard.

The owner reports that all of the screenshots were taken above 100% (usually
800%, as far as remembered). That is the owner's observation, not an
automated check.

**Likely cause of the one-line stripe (`analysis/race-picture.md` section
3.5).** Verified: each line interrupt writes two plane shifts and the
backdrop colour one after another; at original speed all three land within
one screen line (1,276 of 1,276 logged); at 800% they straddle a line
boundary in 390 of 1,200, and in 118 of those both the shift and the colour
were changing, so one line is drawn with the new shift and the old colour.
**Hypothesis:** that is the stripe in the screenshots; no picture with the
stripe has been produced and examined. The larger glitches (road detached,
picture displaced) are not reproduced either; **hypothesis:** the same kind
of race between a line interrupt and the buffer swap. The experiment above
compared game state only, never the picture, so it could not have seen any
of this.

## Recommendation for Stage 3

- **Do not make a faster CPU the default.** It is the cheapest smoothness
  there is, but it changes the bike's performance, which breaks "plays exactly
  like the original", and the conformance measurement
  (`analysis/race-conformance.md`) only holds at 100%.
- **Keep the logic at the original rate and smooth the picture instead:**
  draw in-between frames from two consecutive passes (interpolation). The
  logic, and therefore the feel and the measured conformance, stay untouched.
  This is the faithful route to 60 pictures a second.
- **Offer the faster loop as an option**, labelled as a change to the game
  (more responsive, slightly slower top-end acceleration). 400% is the
  setting to offer: it reaches the 2-frame ceiling and 800% adds nothing.
- Worth doing before either: find the per-pass term in the speed update. If
  it is one small correction, a patched build could make the faster loop
  faithful too, and then the two options above become one.
