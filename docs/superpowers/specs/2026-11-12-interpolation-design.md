# In-between pictures (interpolation): design

Date: 2026-11-12. Status: for the owner's approval.
"Owner" marks what the owner decided; "my call" marks choices made while
writing this that the owner has not discussed.

## Why

The race logic updates about 11 times a second and the picture stands
completely still between updates, for 3 to 8 video frames at a time
(`analysis/race-picture.md` 3.4, verified). That is the choppiness.

Running the CPU at 200% gives about 27 updates a second. The owner likes how
that looks: obstacles are seen sooner and more smoothly, and it "doesn't feel
drastically different from the game". But the bike then performs slightly
differently (`analysis/smoothness-experiment.md`), and the rider's lean poses
change so often that the bike looks jittery; the original's slow update rate
makes the driving look "deliberate".

The aim (owner): the game's logic left exactly as it is, and a new picture
on every video frame (60 a second) that shows the scene part-way between
updates. Smooth world, deliberate bike.

## What the owner will see

- With the crisp road and full-picture objects on (R and O in the Godot
  window, as today), a new key cycles: in-between pictures **off**,
  **blend**, **predict**, **late blend**.
- **Blend:** the scene slides from the previous update towards the newest
  one, about one typical update late (about 90 ms at 100% CPU speed, about
  35 ms at 200%). Almost always exactly right; when an update takes longer
  than usual it guesses ahead for a frame or two (see "Timing").
- **Predict:** the newest update's motion is carried forward until the next
  update arrives. Nothing is late; when the motion changes sharply the guess
  is briefly off and is corrected at the next update.
- **Late blend:** as blend, but late by the longest an update normally
  takes, so it never guesses. The most delayed and the most correct; there
  for comparison. (My call to include it.)
- Both work at any CPU speed (1 to 4 keys), so 100% and 200% can be compared
  with smoothing on. (Owner: build both for an A/B comparison.)
- Rider and bike poses still snap from one drawing to the next, at the rate
  the game changes them. Tweened art is a later idea, not part of this.

## Approach (owner agreed): move the camera and redraw

In-between pictures are not made by sliding or cross-fading finished
pictures. For each video frame the scene is described again for a moment
between two updates (or just past the newest), and drawn by the existing
enlarged renderer (`rr_vdp_render_line_fine`, crisp road, fine sprites).

Call the two updates involved A (older) and B (newer), and `t` the place
between them: 0 is A, 1 is B, above 1 is past B (predict only).

1. **Road.** The port already repeats the game's road projection with
   fractions kept (`rr_road_project`, verified against the game's road
   points on every crisp frame of the recorded race). The in-between road
   is that projection run from an in-between camera: place along the track
   (segment and fraction), sideways position and height, each taken
   part-way between A's and B's values, over the same track data.
2. **Roadside objects.** An object is the same object in A and B when its
   track segment, side, picture and roadside byte agree (measured: 94% of
   roadside records match the update before; the rest are objects appearing
   or leaving). A matched object's screen position and size are taken
   part-way between its A and B draw records, the size through distance so
   that it follows perspective. (Builder's ruling in the plan: the first
   draft of this design re-placed the object from the in-between camera by
   the roadside formulas; taking the records part-way gives the game's own
   values at both ends by construction. The skip-one measurement below
   shows what it costs.)
3. **Vehicles** (riders, cars, the player's bike). A draw record is tied to
   an entry of the game's vehicle list when its distance equals that
   vehicle's track position minus the camera's (measured: exact for every
   record so tied). A matched vehicle's screen position and size are taken
   part-way between its A and B records.
4. **Sky and hills.** The up/down and sideways shifts of the scenery above
   the horizon are numbers per update; they are taken part-way too.
5. **Everything else snaps.** Anything not matched (about 7% of draw
   records: one recurring near-road picture, linked objects and extras that
   have not been identified), the dashboard, and anything this design does
   not name is shown as the game drew it, changing when its update is
   reached. That is today's behaviour, so nothing is lost.

## What must hold (requirements)

1. **The game is untouched.** The CPU, memory and video chip model run
   exactly as now. `rr-race-replay` still ends in `RESULT match` with every
   new option on. Interpolation only reads.
2. **Off means today's picture.** With in-between pictures off, every frame
   is byte for byte what the current build draws.
3. **The ends are the game's own picture.** Drawn at `t = 0` the in-between
   scene equals the scene the current fine renderer draws for update A, and
   at `t = 1` for B: the same road rows and the same object entries
   (position, size, picture, mirror, cuts). This is measured over the whole
   recorded race and reported as counts, and any class of exception is
   listed, not hidden. It is the proof that the in-between scene is the
   same scene and not a look-alike.
4. **Never a wrong picture for the sake of smoothness.** If the two updates
   cannot be described (no verified road inputs, an object set not found in
   video memory, a two-player race, a menu), the frame is drawn as today.
   The existing per-line and per-frame checks against video memory keep
   their meaning for the update being shown.
5. **Time runs forward, evenly.** From one video frame to the next nothing
   that is moving steadily stands still or steps back. When a new update
   shows that a guess was off, the correction is made at once (my call;
   easing it in over a frame or two is a later tuning option).
6. **Poses and pictures.** A matched object shows the picture of the update
   nearer in time (A below `t = 0.5`, B from there on; in predict always
   B). (My call.)
7. **Appearing and leaving.** An object in only one of the two updates is
   shown from, or until, the moment its update is reached; it is not faded.
   (My call.)
8. **Speed.** Drawing a frame at scale 3 stays well inside one video frame
   (16.6 ms) on the owner's machine; today it is about 6 ms. The measured
   figure is reported.

## Timing: one clock, three delays (my call)

How long an update stays on screen is not known until the next one arrives
(3 to 8 frames at 100%, 2 to 5 at 200%). So a blend that must both arrive
at B on time and never guess would have to stall or jump every update.
Instead there is one mechanism:

- Each update is stamped with the video frame in which the original
  picture first shows it. The scene's motion is the straight path through
  those stamped updates, continued past the newest one at the speed of the
  last step.
- The picture for a video frame is that path read at "now minus a delay".
  It advances by exactly one frame's worth every frame, so motion is even
  whatever the updates' lengths.
- **Predict** is delay 0: always past the newest update, always a guess.
- **Blend** is a delay of one typical update (the builder picks the rule,
  for example the average of recent update lengths). Usually between two
  known updates; past the newest one only when an update runs long.
- **Late blend** is a delay of the longest normal update (8 frames at
  100%): never past the newest update in a steady race.

The share of frames each mode spent guessing is measured and reported.

## Jumps that are not motion (my call)

When the camera or an object moves between two updates by more than motion
can explain (a crash and return to the road, the start of a race, a vehicle
put back on the road ahead), nothing is slid or carried forward across the
gap: the new update is shown at once, as today. The builder sets the limits
from the measured movement (the 99th percentiles in the table at the end)
and counts how often the rule fires.

The recorded race has no crash and no police stop. Until the owner's second
recording exists this rule is tested on synthetic data only, and the
analysis says so: behaviour in a crash is **unverified** when stages 1 and
2 are finished. The second recording is not a condition for finishing them.

## Measurements to report (in `analysis/`, verified or hypothesis)

On the `full-throttle` recording, at 100% and at 200% CPU speed:

- Requirement 3's counts.
- How many race frames were drawn in-between, and how many fell back and
  why.
- Smoothness: for the road edge on a few screen lines and for matched
  objects, the frame-to-frame movement. Today it is zero for most frames and
  a jump on the rest; the report shows what it became in blend and predict.
- Guessing: the share of frames each mode drew past the newest update, and
  how far the guessed picture was from the next update when it arrived (expected from the recon: about 1 pixel typically, 4
  pixels at the 90th percentile, 15 or more at the 99th, of 320).
- Skip one: the scene made from updates k−1 and k+1 at the moment of update
  k, compared with the game's real update k. The only measurement of an
  in-between picture against something the game drew.
- Drawing time per frame.

## Stages

1. **The in-between scene, in the replay tool.** Road, roadside objects,
   vehicles, sky and hills; requirement 3 measured on the race; pictures
   dumped for a look.
2. **In the console and the Godot window.** The key, blend and predict,
   pacing, the measurements above.
3. **The centre-line dashes** (decided after looking at stage 2). The dashes
   are sprites the game adds after the objects (`0x13F20`); they are not
   draw records and their drawing has not been ported. In stages 1 and 2
   they snap. They are a strong cue of speed on the road, so stepping dashes
   on a sliding road may be the most visible leftover. Fixing it means
   porting that routine the way the object drawing was ported.

## Not part of this (non-goals)

- Tweened or redrawn art; motion blur; softening far objects (a separate
  small item); upscaled art (stage 2 of high-resolution objects).
- Any change to the game's logic, speed or controls.
- Two-player races (they fall back to today's picture).
- Sound.

## What would make this a failure (anti-goals)

- The race no longer replays identically, or the bike handles differently.
- An object, rider or road edge shown somewhere the game never put it at
  the moments where the game's own picture is known (`t = 0` and `t = 1`).
- A smoother picture that looks less like Road Rash: the look at each update
  is the current enlarged picture and stays so.

## Constraints carried over

- Nothing derived from the ROM is committed; test data is synthetic.
- Every claim in `analysis/` is marked verified or hypothesis; nothing is
  called "the same" without a measurement.
- Godot is the host; the C library is not rebuilt while a Godot window may
  be using it.

## Left to the builder

- Where the in-between scene lives in the code and how A and B are kept.
- What "one typical update" is for blend's delay, and the limits of the
  jump rule.
- How vehicle size is taken part-way (plain, or through distance so that it
  follows perspective), provided requirement 3 holds.
- How the unidentified records are handled beyond "snap".
- The key (suggested: I) and the on-screen messages.
- Default state when the window opens (my call: blend, since it cannot show
  a wrong position; the owner can change the default after the A/B).

## Note on `analysis/race-picture.md`

The committed file holds sections 6 and 7 twice (an editing accident in the
last fix). The working tree removes the duplicate and keeps the corrected
wording; both sections stand, and nothing in them is withdrawn.

## What the recon measured (2026-11-12, recorded race, 2,174 updates)

| Thing | Followed between updates | Movement per update (of 320 pixels) |
|---|---|---|
| Roadside objects | 94.3% matched by segment, side, picture, roadside byte | median 3, 90th percentile 12, 99th 34 |
| Other vehicles | every record tied to the vehicle list by exact distance | median 2, 90th percentile 9, 99th 24 |
| Player's bike | present in every update | median 1, largest 18 |
| Road edges | per-line numbers | median 0, 90th percentile 8, 99th about 25 |
| Not identified | about 7% of records | not measured |

"Carry the last motion forward" for one update: within 1 pixel at the
median, 4 at the 90th percentile, 15 at the 99th, for roadside objects and
for other vehicles. The player's picture changes in 43% of updates, other
vehicles' in 27%. These are measurements of one recorded race at original
speed; the scripts were throwaway and are not in the repository.
