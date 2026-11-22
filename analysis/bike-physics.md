# RR206 bike tuning and motion records

## Verified table chain

The bike-shop ordering is also the index order used by the selection loader at
`0xDCDE`:

1. The selected bike index comes from `0xFFF502` (or `0xFFF504` for the other
   player).
2. `0xDCDE` indexes the 16-longword pointer table at `0x278BA`.
3. It copies 19 words (38 bytes) from the selected record into `0xFF0546` for
   player 1, or `0xFF0570` for player 2.
4. It separately indexes 16-byte records at `0x277AE` and copies their fields
   into player-specific control/handling RAM.
5. Vehicle type zero in the runtime pointer table at `0x2744A` resolves to the
   player-1 tuning buffer at `0xFF0546`.

## Bike-to-record map (verified)

| Index | Bike | Tuning record | Sharing |
| ---: | --- | ---: | --- |
| 0 | Shuriken 400 | `0x2791A` | unique |
| 1 | Panda 500 | `0x27940` | unique |
| 2 | Shuriken TT 250 | `0x27966` | unique |
| 3 | Panda 900 | `0x2798C` | unique |
| 4 | Banzai 7.11 | `0x279B2` | shared with nitro variant |
| 5 | Banzai 600 N | `0x279D8` | shared with Banzai 600 |
| 6 | Banzai 750 N | `0x27A24` | shared with Banzai 750 |
| 7 | Shuriken 1000 N | `0x27A4A` | shared with Shuriken 1000 |
| 8 | Banzai 7.11 N | `0x279B2` | shared with standard variant |
| 9 | Diablo 1000 N | `0x27A70` | shared with Diablo 1000 |
| 10 | Panda 600 | `0x279FE` | unique |
| 11 | Banzai 600 | `0x279D8` | shared with nitro variant |
| 12 | Banzai 750 | `0x27A24` | shared with nitro variant |
| 13 | Shuriken 1000 | `0x27A4A` | shared with nitro variant |
| 14 | Diablo 1000 | `0x27A70` | shared with nitro variant |
| 15 | Wild Thing 2000 | `0x27BEC` | unique secret-bike record |

The shared records show that the nitro distinction is not encoded in the base
gear table. It comes from the secondary per-bike records.

## 38-byte tuning record layout

The offsets and indexing behavior are verified in `0x1A730`. Field names are
descriptive interpretations of that arithmetic.

| Offset | Words | Interpretation | Verified use |
| ---: | ---: | --- | --- |
| `+0x00` | 1 | maximum gear index | Prevents incrementing the gear beyond this value; all player bikes use `5` |
| `+0x02` | 6 | base motion delta by gear | Selected with `gear * 2`, adjusted for controls/state, then written to vehicle `+0x14` |
| `+0x0E` | 6 | upper/upshift speed thresholds | Current vehicle word `+0x12` is compared against the current-gear entry before gear increment |
| `+0x1A` | 6 | lower/downshift speed thresholds | Current vehicle word `+0x12` is compared against the new/current gear entry before gear decrement |

Static analysis established that vehicle `+0x12`, `+0x14`, and `+0x18`
participate as a speed-like value, its signed per-update delta, and a gear
index. The full-race capture described below now verifies those interpretations
dynamically for the player object.

## Dynamic confirmation from `full-throttle`

The MAME 0.288 per-frame RAM capture contains 15,676 consecutive frames. Its
input recording SHA-256 is
`09f3617f6115c2de548f67bfde4f94b143758fad5a2b142ade3bb577330ed32c` and its
RAM-diff SHA-256 is
`98365787fe5df56361ac6faa75e21fb89985488bec4d114738605b9499323c1e`.

The active-speed interval is frames 1,142 through 12,841. Within it:

- `0xFFF502` is `0`, selecting Shuriken 400.
- `0xFF0A74` (player object `+0x12`) ranges from `0x0000` to `0x2F37` and
  changes in 2,029 frames.
- `0xFF0A7A` (player object `+0x18`) ranges from gear 0 through 5.
- During clean throttle acceleration, `0xFF062C` is `0x10`.
- `0xFF0A76` (player object `+0x14`) exactly matches the selected Shuriken
  base-delta word for the live gear:

| Gear | Loaded tuning delta | Observed live delta |
| ---: | ---: | ---: |
| 0 | `0x0022` | `+34` |
| 1 | `0x001A` | `+26` |
| 2 | `0x0015` | `+21` |
| 3 | `0x0012` | `+18` |
| 4 | `0x000F` | `+15` |
| 5 | `0x000C` | `+12` |

This is verified runtime evidence that `0xFF0A74` is player speed state,
`0xFF0A76` is its signed motion/acceleration delta, `0xFF0A7A` is the live
gear, and the table loaded at `0xFF0546` supplies gear-specific acceleration.
The exact conversion from internal speed units to displayed MPH remains
unknown.

## Deterministic write-path replay

Replaying `full-throttle.inp` under the debugger produced 1,924 player motion
updates at the final `move.w D1,(0x14,A1)` instruction at `0x1A948`. The raw
`full-throttle.physics.log` SHA-256 is
`62e055293dd7b7c8cb21d402bcfb50dd165151dca8243fc5e12817eecc6abcaa`.

Verified observations:

- Controller bit `0x10` and player object flag bit 1 agreed on all 1,924
  updates, with zero mismatches. This verifies bit `0x10` as the acceleration
  control and object `+0x68` bit 1 as acceleration-enabled state.
- With normal acceleration state (`flags == 0x42`), the final delta is either
  the exact gear-specific base delta or the speed limiter's `-5` delta.
- In fifth gear, all 546 `+12` events occur from speed `0x2A30` through
  `0x2EDF`; all 710 `-5` limiter events occur from `0x2EE0` through `0x2F37`.
  There are zero threshold violations, dynamically verifying Shuriken 400's
  top threshold `0x2EE0`.
- With acceleration disabled and ordinary flags (`0x40`), all 288 observed
  updates produce `-5`, verifying the coasting-drag path at `0x1A8DC`.
- Object flag bit 0 was present for 43 updates (`flags == 0x43`), and every one
  produced `-5`, matching the forced-drag path at `0x1A8FC`. The gameplay
  meaning of bit 0 remains unknown.
- Object flag bit 5 was present for 78 updates and activated the variable term
  derived from object `+0x3E` at `0x1A8EA`. Its interpretation as road grade or
  slope is still a strong hypothesis, not yet visually verified.
- This run used a non-nitro bike, so it does not dynamically test the doubling
  paths at `0x1A8A4`/`0x1A8C2`.
- No updates had object flag bit 2 set, and raw controller bit `0x40` was absent
  from the trace. Static input decoding connects those bits, but the `-0x40`
  adjustment at `0x1A8CE` is not yet dynamically identified; it may be an
  attack/action penalty rather than ordinary braking.

The debugger replay resolves an apparent ambiguity in the per-frame snapshots:
`0x1A730` runs less frequently than the integrator that advances speed, so the
speed can pass a lower-gear threshold between calls. The top-gear equilibrium
is nevertheless exact at the table threshold when observed at this function.

## Speed integrator

Static disassembly and runtime captures identify `0x175C4` as the vehicle-list
motion integrator. For the ordinary player path at `0x17606`–`0x17646`, it:

1. reads signed delta from object `+0x14`;
2. when object flag `+0x68` bit 3 is set, subtracts one arithmetic quarter of
   that delta;
3. multiplies the result by the unsigned runtime timestep at `0xFF1C08`;
4. adds the low word to speed at object `+0x12`;
5. clamps a negative result to zero; and
6. stores `speed * timestep` as a long at object `+0x26`.

In compact form, the ordinary path is:

```text
applied_delta = delta                         # flag bit 3 clear
applied_delta = delta - arithmetic(delta / 4) # flag bit 3 set
speed = max(0, speed + applied_delta * timestep)
```

`0xFF1C08` is most commonly 4–7 in both existing gameplay recordings, with 5
the mode in each. Snapshot ordering and collision/state-specific writes mean a
frame-to-frame comparison is not expected to match this isolated path on every
event, but the dominant clean updates match the current-timestep equation. The
meaning of flag bit 3 is not yet verified.

## Timestep scheduler (verified)

The race timestep is the number of video frames elapsed since the previous
game-loop pass:

1. `0xE038` is the in-race VBlank callback (the VBlank handler special-cases it).
   Every frame, unless `0xFF254E` is nonzero, it executes
   `addq.w #1,0xFF1C06` at `0xE096`.
2. The game loop calls `0xDF3E` once per pass. When `0xFF1C06` is nonzero, it
   copies it to the timestep `0xFF1C08` and clears it.
3. `0xDF3E` also increments a pass counter at `0xFF1C0E` and adds the timestep to
   `0xFF1C10`. It latches the pass count into `0xFF1C0A` only when the *current*
   timestep is at least 60, so this looks like an updates-per-second counter
   whose test examines the wrong register; it is not needed for motion.
4. Before the increment, the callback forces `0xFF1C08` to 1 if it is zero.

Static writers of `0xFF1C08` are `0xDF48` (the copy above), constant 1 at
`0xBE6A`, `0xD528`, and `0xE088`, and constant 0 at `0xD71A` (race-state reset,
which also clears `0xFF1C04`–`0xFF1C16`). The timestep is read at 49 sites across
vehicle, camera, and object code, so most race motion is frame-rate independent
by design.

`mame/rr206-timestep-replay.cmd` replays `full-throttle.inp` (recorded on MAME
0.288; it replays identically on 0.289, matching the earlier
`full-throttle.physics.log` byte for byte). Across all 2,176 handoffs at `0xDF48`:

- the copied timestep equals the number of `0xE096` increments since the
  previous handoff in every case, with zero exceptions, and also equals the
  number of VBlank interrupts between handoffs;
- the timestep histogram is 3:7, 4:294, 5:911, 6:721, 7:200, 8:41, 9:2;
- the mean is 5.43 frames, so race logic runs at about 11 Hz (6.7–20 Hz).

The raw log is `generated/mame/full-throttle.timestep.log`.

The game appears CPU-bound: the loop runs as fast as the 68000 allows and
scales motion by elapsed frames. A faster CPU should therefore produce more,
smaller updates at the same race speed. This is not yet tested; MAME 0.289
exposes CPU overclocking only through the interactive slider menu, not Lua or
the debugger.

> **Correction 2026-11-10:** the calls described in this paragraph are dead code
> in RR206 (they follow an unconditional branch at `0xE060` and nothing jumps
> into them). See `analysis/code-discovery.md`.

The VBlank callback also calls `0x16CE` (player 1) and `0x179E` (player 2)
every frame. They compute `speed + delta * 0xFF1C06` — speed extrapolated by
the frames since the last loop pass — and feed it into `0xFF2586`/`0xFF2594`.
The road/camera position therefore appears to advance every frame even though
physics updates at about 11 Hz. This interpretation is a **hypothesis**.

## Wild Thing braking and nitro replay

The `wild-thing-controls` capture contains 31,639 frames. Active gameplay uses
bike index 15, loads Wild Thing 2000's unique tuning record, and sets the
secondary nitro parameter at `0xFF056E` to `0x0008`. Capture hashes are:

- RAM diff: `5e40e3f20fcea8b9757ed88038586814a87ec343b79b535c0034300a720b9f4e`
- MAME input: `4deeee79a9d1f5f2e74b6d23c40bcd357df42087d548a3e26bc4b6df5e46f76b`
- Targeted physics log: `be28c86f9318a35811ecc728c3f0eca2014d400678cd1485abd59b6777b4e0dd`

The targeted replay contains 1,813 final player-delta stores and verifies:

- Raw controller bit `0x40` (A/brake) and player object flag bit 2 agree on all
  1,813 updates, with zero mismatches.
- All 36 clean brake-only updates (`flags == 0x44`) produce delta `-69`: the
  static `-0x40` brake adjustment followed by ordinary `-5` drag. Three braking
  updates with flag bit 5 set produce additional variable grade-like changes.
- A nitro-active window is represented by nonzero `0xFF056E` together with
  `0xFF056C < 0x00F0`. All 57 clean acceleration events in that window exactly
  double Wild Thing's live per-gear base delta, with zero exceptions:

| Gear | Normal base delta | Nitro delta |
| ---: | ---: | ---: |
| 0 | `+48` | `+96` |
| 1 | `+40` | `+80` |
| 2 | `+35` | `+70` |
| 3 | `+32` | `+64` |
| 4 | `+30` | `+60` |
| 5 | `+28` | `+56` |

- With nitro inactive, all 429 clean acceleration events use the ordinary base
  delta and all 48 remaining clean events use the limiter's `-5`; there are no
  other results.
- In normal fifth gear, every `+28` event is below `0x4E20` and every `-5`
  limiter event is at or above `0x4E20`, again with zero violations.
- Static code raises the threshold by one eighth while nitro is active, making
  Wild Thing's boosted threshold `0x57E4`. The recording reached `0x5019` but
  contained only four clean boosted fifth-gear updates, so it did not reach or
  dynamically test that boosted ceiling.

These results verify A as braking, B as acceleration/double-tap nitro, the
meaning of the `0xFF056C < 0x00F0` nitro window, and both the base-delta and
nitro-doubling paths. Combat inputs were exercised but are not yet assigned to
specific non-physics animation/state fields.

## Top-gear comparison

These are the final entries in the upper-threshold arrays. They are internal
fixed-point/game units, not displayed MPH.

| Bike | Top upper threshold | Top base delta |
| --- | ---: | ---: |
| Shuriken 400 | `0x2EE0` | `0x000C` |
| Panda 500 | `0x30D4` | `0x000A` |
| Shuriken TT 250 | `0x35E8` | `0x000A` |
| Panda 900 | `0x3908` | `0x0017` |
| Banzai 7.11 | `0x3C8C` | `0x0019` |
| Banzai 600 / N | `0x3200` | `0x000C` |
| Banzai 750 / N | `0x36B0` | `0x0012` |
| Shuriken 1000 / N | `0x3A98` | `0x0018` |
| Diablo 1000 / N | `0x3E80` | `0x0019` |
| Panda 600 | `0x332C` | `0x0010` |
| Wild Thing 2000 | `0x4E20` | `0x001C` |

The Wild Thing's `0x4E20` threshold is 25% above the Diablo's `0x3E80`, which
is consistent with its intentionally exceptional/secret status. Converting
these values to MPH requires correlating `A1+0x12` with the rendered speedometer.

## Nitro/secondary record (partly hypothetical)

The loader copies eight words per bike from `0x277AE`. The last word becomes
`0xFF056E` for player 1. It is zero for ordinary bikes, nonzero for every nitro
variant, and is also nonzero for the Wild Thing. Input and motion code test and
count this field while applying extra motion delta. Therefore its role as a
nitro-related capacity/type parameter is a strong **hypothesis**; the exact unit
still needs a runtime trace.

## Remaining dynamic experiments

1. Hold a nitro boost from near the ordinary top-speed threshold long enough to
   reach and dynamically verify the boosted `0x57E4` ceiling.
2. Overclock the 68000 (MAME slider menu) and rerun the timestep logger to test
   whether the loop rate rises while race speed stays the same. Identify the
   gameplay meaning of the integrator's object flag bit 3.
3. Separate collision, airborne, and slope adjustments to
   `0xFF0A76` from the base gear delta.
4. Correlate `0xFF0A74` with the rendered speedometer to recover its fixed-point
   conversion to displayed MPH.
