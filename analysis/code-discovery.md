# RR206 code discovery

Date: 2026-11-10. This note records how the translator finds RR206's code from
`port/annotations/rr206.toml` alone, without looking at what an emulator ran.
Statements are **verified** or a **hypothesis**, as in the other notes.

## Result (verified)

| | Spike | Now |
| --- | ---: | ---: |
| Instructions discovered from annotations | 19,931 | 24,239 |
| Code bytes | 87,104 | 106,354 |
| Indirect jump and call sites | 9 | 11 |
| Unresolved sites | 9 | 0 |
| Traced addresses not discovered (boot + race windows) | 375 | 0 |

- Both trace windows pass `rr-trace-check` with code translated from the
  annotation file only: boot `insns=5381187 interrupts=434 hw=243507`, race
  `insns=1506888 interrupts=884 handoffs=21 hw=55866`. The `--trace-seed`
  option used during the spike has been removed from `rr2-translate`.
- MAME was run over all three input recordings (`full-throttle`,
  `wild-thing-controls`, `controller-test`) with a breakpoint on every
  indirect site, logging where each jump went, in two rounds (the second after
  the tables below exposed two more sites). Every one of the 44 distinct
  site/target pairs observed was already found by the static annotations
  below, and the second round found no new code. The observed pairs are also kept in `[jump_tables]`.

Reproduce:

```sh
uv run rr2-code-report "Road Rash II (USA, Europe) (RR206).gen" \
  --annotations port/annotations/rr206.toml \
  --window generated/port/traces/boot --window generated/port/traces/race
```

## How the game reaches code indirectly

RR206 uses four mechanisms. Each has an annotation kind.

1. **Pointer tables in the ROM** (`[[pointer_tables]]`). An index selects a
   code address from a table. Three encodings occur: 32-bit absolute
   addresses, 32-bit offsets from a base, and pointers at the start of larger
   records.
2. **Callback slots** (`callback_slots`). A RAM longword holds a code pointer
   that is called later. Code installs it with `move.l #address,<slot>`.
3. **Callback registrars** (`callback_registrars`). A routine takes a code
   pointer in A0 and stores it for later. Callers load it with
   `movea.l #address,a0` directly before the call.
4. **Inline branch tables.** `jmp d(An,Dn)` into a run of `bra.w`
   instructions. The targets are listed in `[jump_tables]`.

## Sites

| Site | Instruction | Mechanism | Targets known from | Count | Status |
| --- | --- | --- | --- | ---: | --- |
| `0x0011DC` | `jsr (a0)` in the VBlank handler | callback slot `0xFF0300` | immediates moved into the slot, plus registrar callers | 9 observed | verified: all 9 observed targets are found statically |
| `0x001420` | `jmp (a0)` | pointer in `0xFFF2C2`, set by the registrars `0x11E4` and `0x1224` | `movea.l #imm,a0` before calls to the registrars | 3 observed | verified for the observed targets; registrar callers that load A0 another way would be missed (hypothesis: there are none) |
| `0x00E130` | `jmp 0(a0,d0.w)` | inline table of six `bra.w` at `0xE134` | read from the code | 6 | verified: the first branch target is `0xE14C`, the address right after the sixth entry |
| `0x018D6E`, `0x018EB4` | `jsr (a4)` | table at `0x1EEA4`, index × 4 | pointer table, 13 longs | 13 | verified count: entry 14 is a small integer, the start of following data |
| `0x019A26`, `0x019BCC`, `0x01F820` | `jsr (a6)` | handler pointers at `0x27546`, read through pointers into vehicle behaviour records | pointer table, 154 longs | 154 | hypothesis for the extent: every long from `0x27546` to `0x277AD` is an even address inside discovered code regions, and `0x277AE` begins data of a different shape. The record layout is not worked out (154 is not a multiple of the five-pointer pattern that repeats through most of it). Verified: all 11 targets MAME observed at these sites are in the table |
| `0x019D1C` | `jsr (a3)` | table at `0x2747E`, 8-byte records, pointer first | pointer table, stride 8 | 16 | verified count: record 17 does not start with a code address. All 16 pointers are `0x19D22` |
| `0x02C416` | `jmp (a0)` | table at `0x2C418` of long offsets from `0x2BCA4` | pointer table, long offsets | 28 | verified count: entry 29 is the first instruction after the table |
| `0x0CBDEE` | `jmp (a1)` | front-end state table at `0xCC274` | pointer table, 4 longs | 4 | verified count: entry 5 is not a code address, and packed data follows |

The table at `0x52F8` (four longs, three null) runs parallel to `0xCC274`; the
dispatcher at `0xCBDDC` copies its entry into `0xFFF97A`. The front end
compares that slot with constants and calls the routines directly, so nothing
jumps through it in discovered code. It is annotated anyway so that
`0x90CE` is treated as code.

## Corrections to earlier notes

- `bike-physics.md` says the race VBlank callback (`0xE038`) calls `0x16CE`
  and `0x179E` every frame. **Verified false for RR206:** those calls sit at
  `0xE064`–`0xE078`, directly after an unconditional `bra.w` at `0xE060`, and
  nothing branches into them. The race trace window never executes `0xE064`
  or `0x179E`. `0x16CE` is reached by other paths; `0x179E` is not reached at
  all in discovered code.

## Remaining risk

**Unreferenced pockets that look like code (hypothesis: dead code).** A scan
of undiscovered ROM for `jsr` instructions whose target is a discovered
routine finds 23, in seven gaps:

| Gap | Bytes | Calls into known code |
| --- | ---: | ---: |
| `0x00164A`–`0x00167F` | 54 | 8 in `0x165A`–`0x1AA6` (this range spans several gaps) |
| `0x004EEA`–`0x005073` | 394 | 1 |
| `0x00AB30`–`0x00AD4D` | 542 | 7 |
| `0x011284`–`0x01131B` | 152 | 3 |
| `0x0180EE`–`0x018217` | 298 | 1 |
| `0x01C938`–`0x01C94F` | 24 | 1 |
| `0x026D8C`–`0x026E25` | 154 | 2 |

No long pointer to the start of any of these gaps exists in the ROM, and each
follows an `rts` or an unconditional branch. The `0xE064` case above shows the
ROM does contain dead code. They are left untranslated: if the game ever
reaches one, the port stops with the address, which is the signal to
investigate.

**What three recordings cannot show.** The recordings cover two single-player
races and a controller test. Front-end screens, two-player mode, combat, cops
and crashes are barely exercised. The static tables do not depend on the
recordings, but any fifth mechanism that only those parts of the game use
would show up as a new unresolved site or as a fatal "untranslated address".
Wider recordings are the check.

**Callback registrars.** Only the `movea.l #imm,a0` immediately before the
call is recognised. Ten call sites of `0x11E4` do not have that shape; they
re-register a pointer that is already in A0 or was saved earlier. If any of
them introduces a new address, it is missed (hypothesis: none does, since all
observed callbacks are accounted for).
