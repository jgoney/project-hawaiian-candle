# Per-frame RAM capture

## Purpose

This workflow records the complete 64 KiB 68000 work RAM state at every
completed emulated frame without storing redundant full snapshots. It is
observational and does not write to emulated RAM or the ROM.

## Capture

```sh
./scripts/run-mame-ram-capture-rr206.sh \
  "Road Rash II (USA, Europe) (RR206).gen" \
  full-throttle
```

Exit MAME normally to finalize the capture. Files are written under the ignored
`generated/mame/` directory:

- `full-throttle.baseline.bin`: initial 65,536-byte RAM image;
- `full-throttle.diffs.bin`: one record per frame containing changed offsets;
- `full-throttle.meta.txt`: capture format and totals;
- `full-throttle.inp`: MAME input recording for replay.

Validate and summarize the result:

```sh
uv run rr2-ram-capture-info generated/mame/full-throttle
```

Rank all aligned big-endian RAM words by Pearson correlation with the candidate
player-speed word at work-RAM offset `0x0A74`:

```sh
uv run rr2-ram-capture-info generated/mame/full-throttle \
  --correlate-word 0x0A74 --min-changes 200 --top 30
```

## Binary diff format

The file starts with ASCII `RR2DIFF1`. Each frame then contains big-endian
`u32 frame_number`, `u32 changed_byte_count`, followed by that many
`u16 RAM_offset`, `u8 new_value` records. Applying records in order to the
baseline reconstructs the exact sampled RAM image for every frame.

## Interpretation boundary

Verified: each record represents 68000 work RAM at MAME's completed-frame
notification boundary.

Limitation: values written and overwritten entirely between two frame
boundaries are not present. Targeted write taps or debugger watchpoints are the
next layer when such transient state matters.
