# Godot reconstruction track

> **Superseded 2026-11-10** by `docs/superpowers/specs/2026-11-10-native-port-design.md`.
> The asset pipeline and MCP policy sections still apply; the clean-room
> reconstruction approach does not.

## Boundary

The Godot project will be a clean-room behavioral reconstruction driven by
documented observations and extracted numeric tables. ROM-derived graphics,
audio, maps, screenshots, VRAM/CRAM dumps, and bulk conversions remain local
under ignored paths. Source code, schemas, extraction tools, tests, and asset
provenance manifests may be committed.

## Initial target

- Godot 4.7.1 Standard with GDScript and the Compatibility renderer.
- Internal presentation size: 320 by 224 pixels with nearest-neighbor scaling.
- Genesis-style A/B/C and directional input actions.
- A data-driven bike resource populated by the verified 38-byte tuning layout.
- A physics laboratory that compares Godot model output against the captured
  speed, gear, delta, braking, drag, nitro, and integration behavior.
- Placeholder visuals until extracted assets have provenance and palette
  validation.

## Asset pipeline

1. Replay a deterministic MAME `.inp` file to a named scene/frame.
2. Capture the displayed frame and VDP video RAM without modifying emulated
   state. MAME exposes the VDP `videoram` address space as `:gen_vdp`.
3. Capture or reconstruct CRAM palettes and the sprite/name tables active at
   the same frame.
4. Decode Genesis 4bpp tiles into indexed PNG sheets, retaining original tile
   indices and palette-line assignments.
5. Record a manifest containing RR206 hash, MAME version, input hash, emulated
   frame, memory range, output hash, and any inferred ROM source range.
6. Trace VDP transfer/decompression routines back to ROM so runtime dumps can
   become reproducible static extractors.

Priority scenes are the bike shop, rider/bike selection, HUD/font, one road
scene, player bike/rider animation, and a small set of roadside objects. Audio
and complete track data follow after the graphics transfer format is known.

## Reverse-engineering dependencies

The core integrator is verified at `0x175C4`: it adds signed object delta times
the runtime timestep at `0xFF1C08` to speed and clamps a negative result to
zero. The gray-box uses this exact arithmetic, but currently schedules a fixed
five-frame timestep at 12 Hz. The original timestep is now verified as frames
elapsed since the previous loop pass, averaging 5.43 (about 11 Hz) in
`full-throttle`. The displayed MPH scale, flag-bit-3 modifier meaning, and
road/collision adjustments remain the next dependencies for faithful motion.
Camera/road projection and object coordinates follow.

## MCP policy

Use at most one Godot MCP and pin its version. Keep its transports bound to
loopback, disable telemetry where supported, and enable it only while the Godot
editor is in active use. Begin with read-only project inspection, run, and
screenshot smoke tests before allowing scene/script mutation. Do not expose ROM
or ignored extracted-asset paths through an MCP resource server.
