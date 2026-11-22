#!/bin/sh
# Dump MAME's video chip memory as the Nth VBlank handler entry of the boot
# sequence begins. Usage: run-mame-vdp-state-rr206.sh N [ROM]
set -eu

EXPECTED_ROM_SHA256="c7a609019b1f052bcee6204a3a09ea77a5150a748de90b51382fedf4df207848"
DEFAULT_ROM="Road Rash II (USA, Europe) (RR206).gen"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
COUNT=${1:-}
ROM_PATH=${2:-"$REPO_DIR/$DEFAULT_ROM"}
MAME_ROOT="$REPO_DIR/generated/mame"

case "$COUNT" in
    *[!0-9]* | "")
        echo "usage: $0 N [ROM]" >&2
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
ROM_DIR=$(CDPATH= cd -- "$(dirname -- "$ROM_PATH")" && pwd)
ROM_PATH="$ROM_DIR/$(basename -- "$ROM_PATH")"
ACTUAL_ROM_SHA256=$(shasum -a 256 -- "$ROM_PATH" | awk '{print $1}')
if [ "$ACTUAL_ROM_SHA256" != "$EXPECTED_ROM_SHA256" ]; then
    echo "error: refusing run; RR206 SHA-256 mismatch" >&2
    exit 1
fi

OUT_DIR="$REPO_DIR/generated/port/pictures"
RUN_DIR="$OUT_DIR/mame-run-state"
rm -rf "$RUN_DIR"
mkdir -p "$RUN_DIR" "$MAME_ROOT/cfg" "$MAME_ROOT/nvram"
RR2_STATE_OUT=$(printf '%s/state-v%04d.bin' "$OUT_DIR" "$COUNT")
RR2_STATE_RAM_OUT=$(printf '%s/state-v%04d-ram.bin' "$OUT_DIR" "$COUNT")
export RR2_STATE_OUT RR2_STATE_RAM_OUT
rm -f "$RR2_STATE_OUT" "$RR2_STATE_RAM_OUT"

cd "$REPO_DIR"
uv run python -c "
import sys
from rr2port.mame_cmd import build_stop_cmd
sys.stdout.write(build_stop_cmd(int(sys.argv[1])))
" "$COUNT" > "$RUN_DIR/stop.cmd"

# The debugger stops at the Nth entry; the Lua script then dumps and exits.
# MAME is run in the background with a time limit in case it does not exit.
cd "$RUN_DIR"
mame genesis \
    -cart "$ROM_PATH" \
    -ctrl1 mdpad \
    -ctrl2 mdpad \
    -debug \
    -debugger osx \
    -debugscript "$RUN_DIR/stop.cmd" \
    -autoboot_script "$REPO_DIR/mame/rr206-vdp-state.lua" \
    -autoboot_delay 0 \
    -window \
    -sound none \
    -nothrottle \
    -skip_gameinfo \
    -cfg_directory "$MAME_ROOT/cfg" \
    -nvram_directory "$MAME_ROOT/nvram" >/dev/null 2>&1 &
MAME_PID=$!
WAITED=0
while kill -0 "$MAME_PID" 2>/dev/null && [ "$WAITED" -lt 180 ]; do
    if [ -f "$RR2_STATE_RAM_OUT" ]; then
        sleep 2
        break
    fi
    sleep 1
    WAITED=$((WAITED + 1))
done
kill -9 "$MAME_PID" 2>/dev/null || true
wait "$MAME_PID" 2>/dev/null || true
cd "$REPO_DIR"
rm -rf "$RUN_DIR"

if [ ! -f "$RR2_STATE_OUT" ] || [ ! -f "$RR2_STATE_RAM_OUT" ]; then
    echo "error: MAME did not write the state for entry $COUNT" >&2
    exit 1
fi
echo "wrote $RR2_STATE_OUT"
