#!/bin/sh
# Log HV counter reads and interrupt entries with MAME's beam position, over the
# same stretch of the full-throttle recording as the race trace window.
set -eu

EXPECTED_ROM_SHA256="c7a609019b1f052bcee6204a3a09ea77a5150a748de90b51382fedf4df207848"
DEFAULT_ROM="Road Rash II (USA, Europe) (RR206).gen"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
ROM_PATH=${1:-"$REPO_DIR/$DEFAULT_ROM"}
MAME_ROOT="$REPO_DIR/generated/mame"
CAPTURE_NAME=full-throttle

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

OUT_DIR="$REPO_DIR/generated/port/hv"
RUN_DIR="$OUT_DIR/mame-run"
rm -rf "$OUT_DIR"
mkdir -p "$RUN_DIR" "$MAME_ROOT/cfg" "$MAME_ROOT/nvram"

cd "$REPO_DIR"
uv run python -c "
import sys
from rr2port.mame_cmd import WINDOWS, build_hv_cmd
window = WINDOWS['race']
sys.stdout.write(build_hv_cmd(window.start, window.end_count))
" > "$RUN_DIR/hv.cmd"

cd "$RUN_DIR"
mame genesis \
    -cart "$ROM_PATH" \
    -ctrl1 mdpad \
    -ctrl2 mdpad \
    -playback "$CAPTURE_NAME.inp" \
    -debug \
    -debugger osx \
    -debugscript "$RUN_DIR/hv.cmd" \
    -log \
    -window \
    -sound none \
    -nothrottle \
    -skip_gameinfo \
    -cfg_directory "$MAME_ROOT/cfg" \
    -nvram_directory "$MAME_ROOT/nvram" \
    -input_directory "$MAME_ROOT"

cd "$REPO_DIR"
grep -E '^RR2(HV|INT) ' "$RUN_DIR/error.log" > "$OUT_DIR/race.txt"
rm -rf "$RUN_DIR"
echo "wrote $OUT_DIR/race.txt ($(wc -l < "$OUT_DIR/race.txt" | tr -d ' ') rows)"
