# Initial RR206 findings

## Scope and confidence convention

This pass establishes reproducible, read-only analysis infrastructure. It does
not attempt a whole-ROM decompilation. A **verified** statement is directly
supported by bytes, control flow, or hardware references in the known-good
RR206 image. A **hypothesis** is a semantic interpretation that still needs a
runtime watchpoint, trace, or additional cross-reference analysis.

## ROM identity and cartridge header (verified)

| Property | RR205 | RR206 (primary) |
| --- | --- | --- |
| Size | `0x100000` | `0x100000` |
| CRC-32 | `7B29C209` | `0876E992` |
| SHA-1 | `6fa6420a2abcf5c2c4620c19b1f2a831996af481` | `9afea43ed627671b50dd4a2abdd043b235414b91` |
| SHA-256 | `8f7d310ce92832c0e3209ff86650eb983ce8fe81ff0d2dcd12083a44958f41bd` | `c7a609019b1f052bcee6204a3a09ea77a5150a748de90b51382fedf4df207848` |
| Serial/revision text | `GM T-50496 -XX` | `GM T-50496 -02` |
| Stored checksum | `0x846A` | `0x203A` |
| Calculated standard checksum | `0x846A` | `0x101D` |

RR206's mismatch is reported but never repaired. Its identity matches the known
good v1.2 SHA-1/CRC. Both headers declare ROM `0x00000000-0x000FFFFF`, work RAM
`0x00FF0000-0x00FFFFFF`, regions `UEJ`, and a `SEGA GENESIS` system string.

## Reset path (verified unless noted)

1. Vector 1 enters at `0x200` with SSP `0xFFFFF6`.
2. The bootstrap reads console/version I/O and conditionally writes ASCII
   `SEGA` to TMSS.
3. It initializes VDP registers, requests/resets the Z80, copies a short Z80
   bootstrap, clears work RAM through the zeroed address register, clears VDP
   memory, and initializes PSG registers.
4. At `0x2FA` it continues with VDP status and additional initialization calls.
5. It writes callback `0x35A` to `0xFF0300`, enables interrupts, and waits until
   the callback slot is cleared.
6. At `0x354` it jumps to `0xCBCC8`.

`0xCBCC8` is annotated `hyp_frontend_state_loop`. Its mechanics are verified:
it repeatedly schedules functions through `0xFF0300`, waits for completion, and
at `0xCBDD0-0xCBDEE` transforms `0xFFF59E` into indices for tables at `0x52F8`
and `0xCC274`, then performs an indirect jump. The interpretation as a
front-end or high-level game-state dispatcher is a **hypothesis**.

## Interrupt dispatch (verified)

- `0x11B8` is vector 30, the level-6/VBlank handler. It saves all general
  registers, loads `A0` from `0xFF0300`, performs a housekeeping call unless the
  callback equals `0xE038`, calls `(A0)` when nonzero, restores registers, and
  executes `RTE`. This makes `0xFF0300` the verified per-frame callback slot.
- `0x14BE` is vector 28, the level-4/HBlank handler. It writes VDP control/data
  ports and consumes RAM-backed scanline command data through pointers around
  `0xFF2796`/`0xFF27DA`. More precise raster-effect semantics remain open.

## Controller input (verified)

- `0x10F4` reads controller port 1. It requests the Z80 bus, sets control
  register `0xA10009`, toggles TH through data register `0xA10003`, combines two
  reads, inverts the result, releases the Z80 bus, and returns the active-high
  three-button state in `D0.b`.
- `0x1156` performs the equivalent sequence for port 2 through `0xA1000B` and
  `0xA10005`.
- `0xE8E2` reads both ports and stores raw bytes at `0xFF062A`/`0xFF062B` before
  invoking per-port decoders.
- `0xE684` chooses a port using `0xFF062E`, stores the raw byte at `0xFF062C`,
  and decodes its bits into gameplay state. Direction bits 2/3 adjust the signed
  bounded word at `0xFF27AA`. Button bit 4 drives `0xFF056C`; button bit 6 changes
  bit 2 at `0xFF0ACA`. Exact in-game button assignments and field semantics are
  still **hypotheses** because control remapping and mode state have not yet been
  traced.

## Physics target status

The acceleration selector at `0x1A730` and the speed integrator at `0x175C4`
are now identified. The timestep at `0xFF1C08` is verified as frames elapsed
since the previous game-loop pass (see `bike-physics.md`); race logic runs at
about 11 Hz. The next targets are a CPU-overclock test of that design and the
collision/slope paths that modify the ordinary integration result.

The static chain is unusually direct:

1. `0xE684` converts controller button bit 4 into state at `0xFF056C`.
2. `0x1A730` reads `0xFF056C` at `0x1A82C` and `0x1A8B6`.
3. It selects a vehicle table through `0x2744A` using `A1+0x16`, treats
   `A1+0x18` as a table/gear index, repeatedly compares `A1+0x12` against
   thresholds, applies positive/negative adjustments, and writes the result to
   `A1+0x14` at `0x1A948`.

The full-race RAM capture and deterministic input replay verify that player
1 uses the object at `0xFF0A62`, `A1+0x12` is speed state, `A1+0x14` is its
signed motion/acceleration delta, and `A1+0x18` is gear. During clean
acceleration the delta exactly matches the selected bike's per-gear table. The
later Wild Thing replay dynamically verifies A/braking as `-0x40` plus
ordinary drag and verifies that nitro exactly doubles every per-gear base delta.
At `0x17606`–`0x17634`, the integrator multiplies signed delta by `0xFF1C08`,
adds it to speed, and clamps a negative result to zero. Collision, airborne,
and road-grade modifiers remain to be separated.

## Reproducibility notes

- `uv run rr2-rom-info ROM` parses and validates headers, vectors, checksums,
  and hashes without opening the input for writing.
- `./scripts/import-ghidra-rr206.sh` verifies RR206 SHA-256, imports it raw at
  address zero, creates the hardware map, and applies evidence-tagged symbols.
- Ghidra project data and all ROM/archive extensions are ignored by Git.
- `ReconReport.java`, `ReconRange.java`, `ReconRefs.java`, and
  `ReconInstructionSearch.java` provide compact headless reconnaissance without
  committing generated listings.
