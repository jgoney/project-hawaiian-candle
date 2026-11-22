#!/bin/sh
# Replay an input recording and save work RAM at every game-loop handoff.
# Usage: run-mame-handoff-capture-rr206.sh <capture-name> [ROM]
set -eu

EXPECTED_ROM_SHA256="c7a609019b1f052bcee6204a3a09ea77a5150a748de90b51382fedf4df207848"
DEFAULT_ROM="Road Rash II (USA, Europe) (RR206).gen"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
CAPTURE_NAME=${1:-}
ROM_PATH=${2:-"$REPO_DIR/$DEFAULT_ROM"}
MAME_ROOT="$REPO_DIR/generated/mame"

case "$CAPTURE_NAME" in
    *[!A-Za-z0-9_-]* | "")
        echo "usage: $0 <capture-name> [ROM]" >&2
        exit 1
        ;;
esac
if ! command -v mame >/dev/null 2>&1; then
    echo "error: mame is not installed or not on PATH" >&2
    exit 1
fi
if [ ! -f "$ROM_PATH" ]; then
    echo "error: RR206 ROM not found: $ROM_PATH" >&2
    exit 1
fi
if [ ! -f "$MAME_ROOT/$CAPTURE_NAME.inp" ]; then
    echo "error: input recording not found: $MAME_ROOT/$CAPTURE_NAME.inp" >&2
    exit 1
fi
ROM_DIR=$(CDPATH= cd -- "$(dirname -- "$ROM_PATH")" && pwd)
ROM_PATH="$ROM_DIR/$(basename -- "$ROM_PATH")"
ACTUAL_ROM_SHA256=$(shasum -a 256 -- "$ROM_PATH" | awk '{print $1}')
if [ "$ACTUAL_ROM_SHA256" != "$EXPECTED_ROM_SHA256" ]; then
    echo "error: refusing run; RR206 SHA-256 mismatch" >&2
    exit 1
fi

OUT_DIR="$REPO_DIR/generated/port/handoffs"
RUN_DIR="$OUT_DIR/mame-run-$CAPTURE_NAME"
PARTIAL="$RUN_DIR/handoffs.bin"
rm -rf "$RUN_DIR"
mkdir -p "$RUN_DIR" "$MAME_ROOT/cfg" "$MAME_ROOT/nvram"

cd "$REPO_DIR"
uv run python -c "
import sys
from rr2port.mame_cmd import build_handoff_stop_cmd
sys.stdout.write(build_handoff_stop_cmd())
" > "$RUN_DIR/stop.cmd"

RR2_HANDOFF_OUT=$PARTIAL
export RR2_HANDOFF_OUT

# MAME exits when playback ends. It runs in the background with a time limit,
# and is stopped if it does not go by itself.
cd "$RUN_DIR"
mame genesis \
    -cart "$ROM_PATH" \
    -ctrl1 mdpad \
    -ctrl2 mdpad \
    -playback "$CAPTURE_NAME.inp" \
    -exit_after_playback \
    -debug \
    -debugger osx \
    -debugscript "$RUN_DIR/stop.cmd" \
    -autoboot_script "$REPO_DIR/mame/rr206-handoff-ram.lua" \
    -autoboot_delay 0 \
    -log \
    -window \
    -sound none \
    -nothrottle \
    -skip_gameinfo \
    -cfg_directory "$MAME_ROOT/cfg" \
    -nvram_directory "$MAME_ROOT/nvram" \
    -input_directory "$MAME_ROOT" >/dev/null 2>&1 &
MAME_PID=$!
trap 'kill -9 "$MAME_PID" 2>/dev/null || true' INT TERM
WAITED=0
while kill -0 "$MAME_PID" 2>/dev/null && [ "$WAITED" -lt 1800 ]; do
    sleep 2
    WAITED=$((WAITED + 2))
done
kill -9 "$MAME_PID" 2>/dev/null || true
wait "$MAME_PID" 2>/dev/null || true
trap - INT TERM

cd "$REPO_DIR"
COUNT=$(uv run python -c "
import sys
from pathlib import Path
from rr2port.handoffs import handoff_count
print(handoff_count(Path(sys.argv[1])))
" "$PARTIAL")
TICK_LOG="$MAME_ROOT/$CAPTURE_NAME.timestep.log"
if [ -f "$TICK_LOG" ]; then
    EXPECTED=$(grep -c '^RR2TICK,[0-9]' "$TICK_LOG" || true)
    if [ "$COUNT" != "$EXPECTED" ]; then
        echo "error: captured $COUNT handoffs, but $TICK_LOG has $EXPECTED" >&2
        exit 1
    fi
fi
# Pad reads in order, with an "H" line at each handoff.
grep -E '^RR2(PAD |HANDOFF)' "$RUN_DIR/error.log" | sed -e 's/^RR2PAD //' -e 's/^RR2HANDOFF.*/H/' \
    > "$OUT_DIR/$CAPTURE_NAME.pads"
# CPU state at each handoff: MAME's cycle count, SR, D0-D7, A0-A7.
grep '^RR2HANDOFF ' "$RUN_DIR/error.log" | cut -c12- > "$OUT_DIR/$CAPTURE_NAME.regs"
if [ "$(wc -l < "$OUT_DIR/$CAPTURE_NAME.regs" | tr -d ' ')" != "$COUNT" ]; then
    echo "error: register log does not have one line per handoff" >&2
    exit 1
fi
if [ ! -s "$OUT_DIR/$CAPTURE_NAME.pads" ]; then
    echo "error: no controller reads were logged" >&2
    exit 1
fi
mv "$PARTIAL" "$OUT_DIR/$CAPTURE_NAME.bin"
mv "$PARTIAL.timesteps" "$OUT_DIR/$CAPTURE_NAME.timesteps"
rm -rf "$RUN_DIR"
echo "wrote $OUT_DIR/$CAPTURE_NAME.bin ($COUNT handoffs)"
