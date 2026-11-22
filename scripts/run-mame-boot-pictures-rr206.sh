#!/bin/sh
# Record reference pictures of the boot sequence at chosen VBlank handler entries.
# Usage: run-mame-boot-pictures-rr206.sh [ROM] [COUNT ...]   (default: 60, 120, ... 1200)
set -eu

EXPECTED_ROM_SHA256="c7a609019b1f052bcee6204a3a09ea77a5150a748de90b51382fedf4df207848"
DEFAULT_ROM="Road Rash II (USA, Europe) (RR206).gen"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
ROM_PATH=${1:-"$REPO_DIR/$DEFAULT_ROM"}
[ "$#" -gt 0 ] && shift
if [ "$#" -eq 0 ]; then
    set -- 60 120 180 240 300 360 420 480 540 600 660 720 780 840 900 960 1020 1080 1140 1200
fi
MAME_ROOT="$REPO_DIR/generated/mame"

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
RUN_DIR="$OUT_DIR/mame-run"
rm -rf "$OUT_DIR"
mkdir -p "$RUN_DIR" "$MAME_ROOT/cfg" "$MAME_ROOT/nvram"

cd "$REPO_DIR"
uv run python -c "
import sys
from rr2port.mame_cmd import build_picture_cmd
sys.stdout.write(build_picture_cmd([int(count) for count in sys.argv[1:]]))
" "$@" > "$RUN_DIR/pictures.cmd"

# MAME 0.289 hangs on exit after a debugger "snap", so it is run in the
# background and stopped once the last picture exists (or after a time limit).
LAST_PICTURE=""
for count in "$@"; do
    LAST_PICTURE=$(printf 'boot-v%04d.png' "$count")
done

cd "$RUN_DIR"
mame genesis \
    -cart "$ROM_PATH" \
    -ctrl1 mdpad \
    -ctrl2 mdpad \
    -debug \
    -debugger osx \
    -debugscript "$RUN_DIR/pictures.cmd" \
    -window \
    -sound none \
    -nothrottle \
    -skip_gameinfo \
    -cfg_directory "$MAME_ROOT/cfg" \
    -nvram_directory "$MAME_ROOT/nvram" \
    -snapshot_directory "$OUT_DIR" &
MAME_PID=$!
WAITED=0
while [ ! -f "$OUT_DIR/$LAST_PICTURE" ] && [ "$WAITED" -lt 600 ] && kill -0 "$MAME_PID" 2>/dev/null; do
    sleep 1
    WAITED=$((WAITED + 1))
done
sleep 2
kill -9 "$MAME_PID" 2>/dev/null || true
wait "$MAME_PID" 2>/dev/null || true

cd "$REPO_DIR"
rm -rf "$RUN_DIR"
for count in "$@"; do
    name=$(printf 'boot-v%04d.png' "$count")
    if [ ! -f "$OUT_DIR/$name" ]; then
        echo "error: MAME did not write $name" >&2
        exit 1
    fi
done
echo "wrote $# pictures to $OUT_DIR"
