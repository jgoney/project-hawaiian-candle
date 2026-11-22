# Building the game, and making an installer

Written 2026-11-13. Each part says whether it has been **run** on the
owner's machine (Apple Silicon Mac) or is **not yet tried**.

## What a build is made of

| Piece | Where it comes from | In the repository? |
|---|---|---|
| The game's code, translated to C | made from your ROM by `rr2-translate` | no (`generated/`) |
| The native library `godot/bin/librr2.dylib` | that C plus the port's own code in `port/` | no |
| The window: scenes, scripts, shaders | `godot/` | yes |
| Two sound models (a Z80 processor, the FM chip) | other people's open-source code, `port/third_party/` | yes, unchanged, with their licences |
| The ROM itself | yours | no; read when the game starts (pictures, track data) |

So the library **contains the original game's code** in another form, and
the running game still **reads the ROM** for its data. Both facts shape
what an installer can be (see the end).

## Building and running from the repository (run)

You need:

- macOS with Apple's command-line tools (`xcode-select --install`), for `cc`
  and `make`.
- `uv` (runs the Python tools; it fetches Python itself).
- Godot 4.7 at `/Applications/Godot.app` (or set `GODOT_BIN` to its
  program).
- The RR206 ROM, as `Road Rash II (USA, Europe) (RR206).gen` in the
  repository's top folder. The build refuses any other file (it checks the
  SHA-256).

Then:

```sh
scripts/build-godot-extension.sh   # translate the ROM, build the library
scripts/run-godot-console.sh       # play
```

Close any Godot window before rebuilding; replacing the library under a
running window can crash it.

To check a build:

```sh
make -C port test                  # the port's C tests
uv run --group dev pytest          # the Python tools and the replay tests
```

Keys and pads are listed in `godot/README.md`.

## Making an app you can double-click (not yet tried)

Godot can "export" the project into a normal Mac app. Nothing below has
been run yet; the first three items are changes the project needs first.

**Needed before the first export**

1. **Where the app finds the ROM.** Today the window looks in the
   `RR2_ROM` environment variable, else next to the repository. An app has
   neither. It needs to look in one fixed place (for example
   `~/Library/Application Support/<app name>/`) and, if nothing is there,
   show a "choose your ROM file" window and remember the answer.
2. **Leave the development helper out.** `project.godot` loads the
   `godot_ai` add-on (an editor helper, not part of the game). The export
   must not include it.
3. **Godot's export templates.** Install them once from the Godot editor
   (Editor → Manage Export Templates); they are not on this machine yet.
4. **Both kinds of Mac (only if wanted).** The library is built for the
   machine it is built on (Apple Silicon here). For Intel Macs too, build
   it with `-arch arm64 -arch x86_64`.

**The export itself**

1. In the Godot editor: Project → Export → Add → macOS. This writes
   `godot/export_presets.cfg`. The native library is picked up from
   `rr2.gdextension`.
2. Export from the command line:

   ```sh
   /Applications/Godot.app/Contents/MacOS/Godot --headless --path godot \
       --export-release "macOS" godot/builds/RoadRashPort.app
   ```

   `godot/builds/` is already ignored by Git.
3. **Signing.** macOS will not run an unsigned app on Apple Silicon.
   - For your own machine: an "ad hoc" signature is enough, and Godot's
     export can apply it (the "Codesign: ad hoc" setting).
   - For other people's machines: an Apple Developer ID (99 USD a year),
     then Apple's "notarization" check. Without it they see a "damaged or
     unidentified developer" warning.

**The installer (a disk image)**

On a Mac the usual installer is a `.dmg`: the user opens it and drags the
app into Applications.

```sh
hdiutil create -volname "Road Rash Port" -srcfolder godot/builds/RoadRashPort.app \
    -ov -format UDZO godot/builds/RoadRashPort.dmg
```

Windows and Linux builds are possible later (Godot exports to both), but
the library and its build script are Mac-only today.

## What may and may not be handed to other people

- **An app built as above contains the game's translated code.** It is a
  copy of the original game in another form. It is fine on your own
  machines; it is not something to publish.
- **What can be shared** is what is in the repository: the tools and the
  port's own code, with no game code or data. Someone with their own ROM
  runs the two commands at the top and gets the same build. A friendlier
  version of that (one "builder" app that asks for the ROM and does the
  rest) is possible, but it would have to carry a C compiler or ask the
  user to install one.
- **The later game** (own name, art, tracks, sound, rewritten code) has
  none of these limits; it would be exported and signed exactly as above
  and shipped as a normal app.
