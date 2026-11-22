# Road Rash II Godot research lab

## The native game

The project's main scene (`scenes/console.tscn`) runs the natively translated
game: one console frame per 60 Hz physics tick, shown at a whole-number scale.

```sh
scripts/build-godot-extension.sh   # translate the ROM and build godot/bin/librr2.dylib
scripts/run-godot-console.sh       # run it
```

The native library is built from your own ROM and is not in the repository.
The ROM is read at run time from `RR2_ROM`, or from the default file name in
the repository root.

| | Player 1 | Player 2 |
| --- | --- | --- |
| Directions | arrow keys, d-pad or left stick (gamepad 1) | I J K L, gamepad 2 |
| A, B, C | Z, X, C; gamepad X, A, B | T, Y, U; gamepad X, A, B |
| Start | Enter; gamepad Start | Backspace; gamepad Start |

F5 resets. F12 saves the current picture under `user://`.

**P** captures the moment, for reporting a fault. It saves a folder under
`generated/captures/` (named by date and time; ignored by Git) with:

- `picture.png`: the picture as drawn, before the CRT screen;
- `ram.bin`: the game's 64 KB of work memory;
- `settings.txt`: speed, crisp road, in-between mode and the rest;
- `session.txt`: everything done since start-up, by frame: buttons, speed
  changes, resets and picture settings.

The port is deterministic, so the session brings it back to that exact
frame, paused or moving:

```sh
make -C port session-replay
generated/port/build/rr-session-replay --rom "Road Rash II (USA, Europe) (RR206).gen" \
  --session generated/captures/<folder>/session.txt \
  --ram generated/captures/<folder>/ram.bin --picture /tmp/moment.ppm
```

`RESULT reproduced` means its memory at the end is byte for byte the
captured memory. `--last N:PREFIX` also writes the last N frames' pictures,
`--at FRAME:FILE` the picture after a given frame, and `--plain` adds the
game's own 320 x 224 picture beside each, to tell a fault in the game's
picture from one in the port's drawing.
A replay runs every frame of the session again, so a ten-minute session
takes a few minutes. It works on a halted game too.
**Tab** (or F1) shows a list of every key and what it is set to now; Tab
again hides it.

The window **remembers its settings** from one launch to the next: the
crisp road, the fine road points, the full-picture objects, the in-between
mode, the softened far objects, the wide view, the CRT screen, the mute
and the game speed. They are saved whenever one is changed (in `console_settings.cfg`
in Godot's user-data folder for this project) and put back, without a
word, when the game starts. Delete that file to start as new.

The game's **sound** plays as the original makes it: its own sound program
running beside the game and playing a model of the FM chip
(`analysis/sound.md`). **M** mutes and unmutes. The square-wave chip is not
modelled yet, and there is no console output filter, so the top end is a
little brighter than a real console's.

**R** switches between the original picture and the **crisp road**: the
picture drawn several times as large, with the road's edges and edge
lines drawn as clean slopes at that size from the game's own road points,
instead of the original's staircase. It only changes how a one-player race
is drawn: the game itself runs exactly as before, and the sky, centre-line
dashes and dashboard are the original picture enlarged. The larger size is chosen to fit the window evenly: 3 times in
the default 960×672 window (4 times in a 1280×896 one, and so on up to 5),
so that every step of a slope is the same size on screen. The original
picture is the default. See
`analysis/race-picture.md`, sections 5 and 6, for what was measured and what
it does not yet do. The road's points are placed with fractions of a pixel
(section 6), to keep distant edges from wobbling; **F** switches that off
and on again while the crisp road is showing, so the two can be compared.
With it on, the road's edge near the camera, at sharp distant bends and at
hill crests is not exactly where the original draws it (section 6 says by
how much).

While the crisp road is showing, riders, cars and roadside objects are drawn
from their **full pictures** in the ROM at the larger size, instead of from
the small sprites the game scales them down to, so distant ones keep their
detail (section 7). Each stands where the game puts it, at the game's size.
**O** switches that off and on again, so the two can be compared. Objects
the original shrinks to a pixel or two are plainly visible this way, so the
far scene is a little busier than the original's. **B** softens the far
objects: one drawn smaller than its art takes the average colour of the art
it covers instead of picking single pixels, so distant things are smoother
and shimmer less (those in-between colours are not in the game's palette);
B again makes them sharp.

**W** turns on the **wide view**: the race is shown 398 of the game's
pixels across instead of 320 (16:9 instead of 10:7), with more of the sky,
the hills, the grass, the road and the roadside at either side. The game
knows nothing of it and runs exactly as before; the middle 320 pixels are
the picture as it is without W, and the port draws the two side bands from
what the game holds: its sky and hills shown further round, the grass and
the road's edge carried on from the road's own points, and the roadside
objects and riders that stand beyond the screen's sides drawn from their
full pictures. The dashboard is not widened, it is finished off: each mirror
gets an outer frame like its inner one, and the dashboard's blue fills the
rest of the margins. Menus and a two-player race sit in the middle between
black margins. It needs
the crisp road (R), the fine road points (F) and the full-picture objects
(O); with one of them off W says so, and turning one off while W is on
puts the 320 view back until it is on again. A window that is too
narrow for the wider picture at the size it has is made wider (960 x 672
becomes 1194 x 672) and put back when the wide view goes; a window with
room is left alone, and a fullscreen or maximised one may show the wide
picture a step smaller (the note says at what size).
`analysis/widescreen.md` says what was measured and what is not there yet
(roadside objects the game itself leaves out as off-screen do not appear
at the sides until they would be on the original's screen).

With all of that on, the window draws **in-between pictures**: the game
still works out the race about 11 times a second, exactly as before, but
every frame shows the scene part-way between two of its updates, so the road,
the roadside and the other riders move 60 times a second instead of standing
still for several frames (section 8). **I** steps through the modes:

| Mode | What it shows | Cost |
|---|---|---|
| blend (as the window opens) | the scene sliding from one update to the next | about one update late (about 90 ms at 100% speed) |
| predict | the newest update's motion carried forward | nothing late, but a sharp change of motion shows as a small correction |
| late blend | as blend, later still | never has to guess; the most delayed |
| off | the picture as before: still between updates | |

The centre-line dashes are drawn from the same road, so they sit on its
centre and slide with it (section 9); near the finish line they are the
game's own sprites again and step. Rider and bike poses and the riders in
the mirrors still change only when the game changes them. A frame that cannot be made in
between (a menu, an update the port cannot describe) is drawn as before.

**V** shows the picture on a CRT tube (`shaders/crt.gdshader`): scanline
beams that swell with brightness, a phosphor mask in the window's own
pixels, a soft glow around bright areas and slightly curved glass with
rounded corners, all worked out in linear light. V again takes it off. It
is a look, not a measurement: it repaints the finished frame and changes
nothing in the game.

Keys 1 to 4 run the
game's processor at 100%, 200%, 400% or 800% of its original speed while the
video timing stays the same (see `analysis/smoothness-experiment.md`). If the game reaches
something the port does not handle yet, it stops and shows the error with the
address or hardware register involved; F5 starts again.

The class behind it, `RoadRashConsole`, comes from a native extension written
in plain C (`port/host/rr_godot.c`). Headless tests:

```sh
Godot --headless --path godot --script res://tests/test_console.gd
Godot --headless --path godot --script res://tests/test_pad_input.gd
Godot --headless --path godot --script res://tests/test_console_scene.gd
```

## The earlier research lab

The rest of this file describes the clean-room experiments that preceded the
port. They are kept for reference; `scenes/vertical_slice.tscn` is no longer
the main scene.

This Godot 4.7.1 GDScript project is a clean-room behavioral reconstruction
harness. It contains no ROM-derived graphics, audio, or bulk dumps.

Run the current conformance scene:

```sh
scripts/run-godot-editor.sh
```

The launcher disables Godot AI telemetry and opens the project in editor mode.
The optional MCP plugin is installed locally under `addons/godot_ai/`; see
`addons/README.md` for the pinned source and setup details.

Headless smoke test:

```sh
/Applications/Godot.app/Contents/MacOS/Godot --headless --path godot --quit-after 2
```

The main scene is a playable hybrid-rendering experiment: a true-3D gray-box
road driven by the verified RR206 gear-delta and speed-integration arithmetic.
Its fixed 12 Hz update schedule, conversion to world speed, steering, road,
camera, and traffic are deliberately labeled provisional in the HUD.

Controls:

- W/up or controller A: accelerate
- S/down or controller B: brake
- A/D, arrows, or the left stick: steer
- Shift or controller X: prototype nitro control
- 1/2: Shuriken 400 / Wild Thing 2000
- R: reset

The conformance-only scene remains available at
`res://scenes/physics_lab.tscn`. Run it headlessly with:

```sh
/Applications/Godot.app/Contents/MacOS/Godot --headless --path godot \
  res://scenes/physics_lab.tscn --quit-after 2
```
