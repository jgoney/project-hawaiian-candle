#!/bin/sh
# Replay an input recording in MAME and log every write the original makes to
# the FM chip, with a recording of MAME's own sound to listen to.
# Writes generated/sound/<capture-name>.mame-fm.log (compare with the port's
# using rr2-fm-compare) and generated/sound/<capture-name>.mame.wav.
# Usage: run-mame-fm-log-rr206.sh <capture-name> [ROM]
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

OUT_DIR="$REPO_DIR/generated/sound"
RUN_DIR="$OUT_DIR/mame-run-$CAPTURE_NAME"
rm -rf "$RUN_DIR"
mkdir -p "$RUN_DIR" "$MAME_ROOT/cfg" "$MAME_ROOT/nvram"

RR2_FM_LOG="$RUN_DIR/fm.log"
export RR2_FM_LOG

# MAME exits when playback ends. It runs in the background with a time limit,
# and is stopped if it does not go by itself.
cd "$RUN_DIR"
mame genesis \
    -cart "$ROM_PATH" \
    -ctrl1 mdpad \
    -ctrl2 mdpad \
    -playback "$CAPTURE_NAME.inp" \
    -exit_after_playback \
    -autoboot_script "$REPO_DIR/mame/rr206-fm-log.lua" \
    -autoboot_delay 0 \
    -wavwrite "$RUN_DIR/sound.wav" \
    -window \
    -sound none \
    -nothrottle \
    -skip_gameinfo \
    -cfg_directory "$MAME_ROOT/cfg" \
    -nvram_directory "$MAME_ROOT/nvram" \
    -input_directory "$MAME_ROOT" >"$RUN_DIR/mame.out" 2>&1 &
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

if [ ! -s "$RUN_DIR/fm.log" ]; then
    echo "error: MAME logged no FM writes; see $RUN_DIR/mame.out" >&2
    exit 1
fi
mv "$RUN_DIR/fm.log" "$OUT_DIR/$CAPTURE_NAME.mame-fm.log"
if [ -s "$RUN_DIR/sound.wav" ]; then
    mv "$RUN_DIR/sound.wav" "$OUT_DIR/$CAPTURE_NAME.mame.wav"
else
    echo "warning: MAME wrote no sound recording" >&2
fi
rm -rf "$RUN_DIR"
echo "wrote $OUT_DIR/$CAPTURE_NAME.mame-fm.log ($(wc -l < "$OUT_DIR/$CAPTURE_NAME.mame-fm.log" | tr -d ' ') writes)"
