#!/bin/sh
# Record a reference trace window (boot or race) for the native port checks.
set -eu

EXPECTED_ROM_SHA256="c7a609019b1f052bcee6204a3a09ea77a5150a748de90b51382fedf4df207848"
DEFAULT_ROM="Road Rash II (USA, Europe) (RR206).gen"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
WINDOW=${1:-}
ROM_PATH=${2:-"$REPO_DIR/$DEFAULT_ROM"}
MAME_ROOT="$REPO_DIR/generated/mame"

case "$WINDOW" in
    boot) PLAYBACK=""; START_PC="000206" ;;
    race | race-start | race-early | race-66 | race-822 | race-953) PLAYBACK="full-throttle"; START_PC="00DF48" ;;
    *)
        echo "usage: $0 <boot|race|race-start|race-early|race-66|race-822|race-953> [ROM]" >&2
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
    echo "error: refusing trace; RR206 SHA-256 mismatch" >&2
    exit 1
fi
if [ -n "$PLAYBACK" ] && [ ! -f "$MAME_ROOT/$PLAYBACK.inp" ]; then
    echo "error: input recording not found: $MAME_ROOT/$PLAYBACK.inp" >&2
    exit 1
fi

OUT_DIR="$REPO_DIR/generated/port/traces/$WINDOW"
RUN_DIR="$OUT_DIR/mame-run"
rm -rf "$OUT_DIR"
mkdir -p "$RUN_DIR" "$MAME_ROOT/cfg" "$MAME_ROOT/nvram"

cd "$REPO_DIR"
uv run python -c "
import sys
from rr2port.mame_cmd import WINDOWS, build_trace_cmd
sys.stdout.write(build_trace_cmd(WINDOWS[sys.argv[1]], sys.argv[2]))
" "$WINDOW" "$OUT_DIR" > "$OUT_DIR/trace.cmd"

cd "$RUN_DIR"
set -- genesis \
    -cart "$ROM_PATH" \
    -ctrl1 mdpad \
    -ctrl2 mdpad \
    -debug \
    -debugger osx \
    -debugscript "$OUT_DIR/trace.cmd" \
    -log \
    -window \
    -sound none \
    -nothrottle \
    -skip_gameinfo \
    -cfg_directory "$MAME_ROOT/cfg" \
    -nvram_directory "$MAME_ROOT/nvram" \
    -input_directory "$MAME_ROOT"
if [ -n "$PLAYBACK" ]; then
    set -- "$@" -playback "$PLAYBACK.inp"
fi
# MAME 0.289 can hang on the script's "quit" after a long trace, so the end of
# the window is taken from the trace itself and MAME is stopped from here.
mame "$@" >/dev/null 2>&1 &
MAME_PID=$!
trap 'kill -9 "$MAME_PID" 2>/dev/null || true' INT TERM
WAITED=0
while kill -0 "$MAME_PID" 2>/dev/null && [ "$WAITED" -lt 1800 ]; do
    if [ -f "$OUT_DIR/raw-trace.txt" ] && tail -c 200 "$OUT_DIR/raw-trace.txt" | grep -q RR2END; then
        sleep 5 # let the last RAM image be written
        break
    fi
    sleep 2
    WAITED=$((WAITED + 2))
done
kill -9 "$MAME_PID" 2>/dev/null || true
wait "$MAME_PID" 2>/dev/null || true
trap - INT TERM
if ! tail -c 200 "$OUT_DIR/raw-trace.txt" 2>/dev/null | grep -q RR2END; then
    echo "error: MAME stopped before the end of the trace window" >&2
    exit 1
fi

cd "$REPO_DIR"
uv run python -m rr2port.trace "$OUT_DIR/raw-trace.txt" "$RUN_DIR/error.log" "$OUT_DIR"

FIRST_PC=$(head -n 1 "$OUT_DIR/trace.txt" | awk '{print $2}')
if [ "$FIRST_PC" != "$START_PC" ]; then
    echo "error: trace starts at $FIRST_PC, expected $START_PC" >&2
    exit 1
fi
for image in ram-start.bin ram-end.bin; do
    if [ "$(wc -c < "$OUT_DIR/$image" | tr -d ' ')" != "65536" ]; then
        echo "error: $image is not 65536 bytes" >&2
        exit 1
    fi
done

{
    echo "rom_sha256=$ACTUAL_ROM_SHA256"
    echo "mame_version=$(mame -version | head -n 1)"
    echo "window=$WINDOW"
    echo "playback=${PLAYBACK:-none}"
} > "$OUT_DIR/meta.txt"
rm -f "$OUT_DIR/raw-trace.txt"
rm -rf "$RUN_DIR"
echo "wrote $OUT_DIR"
