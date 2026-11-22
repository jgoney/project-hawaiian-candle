# RR206 memory map

This map describes the 68000 address space used by the reproducible Ghidra
import. Hardware ranges are standard Sega Genesis mappings; game-specific RAM
symbols come from static RR206 references.

## Platform map (verified)

| Range | Ghidra block | Purpose | Evidence |
| --- | --- | --- | --- |
| `0x000000-0x0FFFFF` | `ROM` | 1 MiB cartridge ROM | File size and header range `0x00000000-0x000FFFFF` |
| `0xA00000-0xA01FFF` | `Z80_RAM` | Shared Z80 RAM/window | Genesis hardware map; startup copies Z80 bootstrap bytes here |
| `0xA10000-0xA1001F` | `IO_REGISTERS` | Version and controller I/O | Direct references in reset and pad routines |
| `0xA11100-0xA11101` | `Z80_BUS_REQUEST` | Z80 bus request | Reset and pad readers write/test this register |
| `0xA11200-0xA11201` | `Z80_RESET` | Z80 reset control | Reset path writes this register |
| `0xA14000-0xA14003` | `TMSS` | TMSS security register | Reset path conditionally writes ASCII `SEGA` |
| `0xC00000-0xC0001F` | `VDP_PORTS` | VDP data/control/counter ports; PSG is at `0xC00011` | Reset and interrupt references |
| `0xFF0000-0xFFFFFF` | `WORK_RAM` | 64 KiB 68000 work RAM | Cartridge header and Genesis hardware map |

Important individual I/O addresses:

| Address | Symbol | Meaning |
| --- | --- | --- |
| `0xA10003` | `IO_PORT1_DATA` | Controller port 1 data |
| `0xA10005` | `IO_PORT2_DATA` | Controller port 2 data |
| `0xA10009` | `IO_PORT1_CONTROL` | Controller port 1 control |
| `0xA1000B` | `IO_PORT2_CONTROL` | Controller port 2 control |
| `0xC00000` | `VDP_DATA_PORT` | VDP data port |
| `0xC00004` | `VDP_CONTROL_PORT` | VDP control/status port |

## Exception vectors (verified)

The vector table is 64 big-endian longwords at `0x000000-0x0000FF`.

| Vector | Slot | RR206 target | Annotation |
| --- | --- | --- | --- |
| Initial SSP | `0x000000` | `0x00FFFFF6` | Initial stack pointer |
| Reset | `0x000004` | `0x00000200` | `reset_entry` |
| Level 4 | `0x000070` | `0x000014BE` | `hblank_handler` |
| Level 6 | `0x000078` | `0x000011B8` | `vblank_handler` |

Other exception vectors are parsed by `rr2-rom-info --all-vectors` and seeded
as entry points by `GenesisImport.java` when their targets are aligned and fall
inside ROM.

## Game-specific work RAM

The access behavior in the last column is verified statically. Semantic names
prefixed `hyp_` are hypotheses and should be confirmed with emulator watchpoints.

| Address | Ghidra symbol | Static access behavior |
| --- | --- | --- |
| `0xFF0300` | `frame_callback` | VBlank loads this longword and calls it when nonzero; foreground code schedules callbacks here and waits for it to clear |
| `0xFF0304` | not named | Mode/player-count-like word used by HBlank, input, and vehicle logic; exact meaning unknown |
| `0xFF056C` | `hyp_primary_throttle_state` | Selected-input routine changes it from button bit 4; vehicle logic at `0x1A730` tests it against `0x00F0` |
| `0xFF1C06` | frame accumulator | Incremented every frame at `0xE096` by the race VBlank callback `0xE038`; copied to `0xFF1C08` and cleared by `0xDF3E` |
| `0xFF1C08` | physics timestep | Frames since the previous game-loop pass (runtime-verified); multiplied by vehicle delta at `0x1761E`–`0x1762A` before updating speed |
| `0xFF1C0A` | not named | Pass count latched from `0xFF1C0E` by `0xDF3E`; looks like an updates-per-second counter |
| `0xFF254E` | not named | Nonzero suppresses the per-frame accumulator increment; possibly pause |
| `0xFF0546-0xFF056B` | `player_1_bike_tuning` | Selected 38-byte bike tuning record copied from the ROM pointer table at `0x278BA` |
| `0xFF0570-0xFF0595` | `player_2_bike_tuning` | Second-player copy of the selected tuning record |
| `0xFF062A` | `controller_1_raw` | Raw active-high byte returned by port 1 reader |
| `0xFF062B` | `controller_2_raw` | Raw active-high byte returned by port 2 reader |
| `0xFF062C` | `selected_controller_raw` | Raw byte for the currently selected controller |
| `0xFF062E` | not named | Selects whether `0xE684` reads controller port 1 or 2 |
| `0xFF0ACA` | not named | Gameplay control flag byte; input bits 4 and 6 change bits 1 and 2 |
| `0xFF27AA` | `hyp_primary_steering_state` | D-pad bits 2/3 drive a bounded signed value toward left/right limits |
| `0xFFF59E` | `hyp_frontend_state_index` | Shifted right and used to index tables at `0x52F8` and `0xCC274` |

## Race picture (from `analysis/race-picture.md`)

What these hold is read from the code and tested in section 4 of that
document unless marked otherwise.

| Address | Meaning |
| --- | --- |
| `0xFF0A0A` (long) | Camera's place on the track: segment number, then fraction of the segment |
| `0xFF0A02` | Player's sideways position; `0xFF0A08` camera height |
| `0xFFD828` | Ring of 256 track segments, 2 bytes each: curve, hill |
| `0xFF1CA0` | Number of road segments projected (32) |
| `0xFF1E20`, `0xFF1DBC`, `0xFF1E84`, `0xFF1F4C`, `0xFFD2B8` | Per projected segment: screen line, centre, left edge, right edge, clipped screen line |
| `0xFF27DE`, `0xFF2D1E` | Per screen line: left and right road edge |
| `0xFFF2B0`, `0xFFF2B2` | Top and bottom line of the 3D view; `0xFFF2BC` horizon reference line; `0xFFF2BE` screen centre |
| `0xFFCEB8` | Draw records, 22 bytes each; `0xFFDC42` how many |
| `0xFF08D6` | Order in which the draw records are drawn, ended by 0xFFFF |
| `0xFF06D6` | Per draw record: distance from the camera as a long (segments, fraction) |
| `0xFF1CDC`, `0xFF1CE0`, `0xFF1CE4` | Pointers to the road point arrays used for objects: screen line, centre, clipped line |
| `0xFF1CE8` | Pointer to the road's world position per road point, 4 bytes each: sideways, height |
| `0xFFDC30`, `0xFFDC34` | Next free tile in the sprite-picture buffer; next free sprite (while a pass is drawn) |
| `0xFF27CE` | Pointer to the sprite-picture buffer being built; `0xFFCE86` its first tile number |
| `0xFFEC6E`, `0xFFEF6E` | The scaler's step lists for the picture being drawn: across, down |
| `0xFF2EDE`, `0xFF309E` | Per screen line: centre-dash start x and width (read, not tested) |
| vehicle object `+4`, `+0x30`, `+0x32` | Track position (long), sideways position, height |
| `0xFF04F8` | List of vehicle object addresses, ended by 0xFFFF |
| `0xFFD352` | Current position in the roadside-object list (4-byte entries) |
| `0xFF3A40`, `0xFF40C2` | The two staged copies of the per-line shifts (896 bytes) and sprite list (from +0x400) |
| `0xFF4742`, `0xFF7A0A` | The two staged copies of the sprite pictures |
| `0xFFACD2`, `0xFFBB52` | The two staged copies of the dashboard sheet |
| `0xFF1C1E` | Staged colours (64 words) |
| `0xFF279A`, `0xFF27A6` | Pointers to the shift table and sprite list being built |
| ROM `0x27C84` | Perspective table: scale for each distance |
| ROM `0x56A0A` | Table of pointers to object pictures |
| ROM `0x7C58` | Sprite shapes: for each height and width in tiles, 8 bytes: height in pixels, size bits, tiles used, width in pixels |
| ROM `0x16E2C` | Irregular spacing for copies of a roadside object |

## Player vehicle object layout

Function `0x1A730` receives a vehicle object in `A1`. The player-1 instance is
verified at `0xFF0A62` by the full-race RAM capture and deterministic debugger
replay. Unknown flag semantics remain explicitly marked as hypotheses.

| Offset | Hypothesized field | Verified operation |
| --- | --- | --- |
| `+0x12` | speed / forward-motion magnitude | Runtime-verified against gear deltas and the exact top-speed threshold |
| `+0x14` | signed motion / acceleration delta | Runtime-verified to equal the selected bike's per-gear base delta during clean acceleration |
| `+0x16` | vehicle/model index | Selects a longword table entry through the pointer table at `0x2744A` |
| `+0x18` | gear | Runtime-verified from 0 through 5 and indexed into the live tuning record |
| `+0x26` | timestep-scaled forward motion | Integrator stores unsigned `speed * 0xFF1C08` as a long at `0x17646` |
| `+0x3E` | variable grade-like term | Used only when flag bit 5 is set; exact road/slope meaning remains hypothetical |
| `+0x68` | vehicle state/control flags | Bit 1 exactly follows B/acceleration; bit 2 exactly follows A/brake; other bits select forced drag and variable modifiers |
