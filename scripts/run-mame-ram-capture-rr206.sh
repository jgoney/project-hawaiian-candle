#!/bin/sh
set -eu

EXPECTED_SHA256="c7a609019b1f052bcee6204a3a09ea77a5150a748de90b51382fedf4df207848"
DEFAULT_ROM="Road Rash II (USA, Europe) (RR206).gen"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
ROM_PATH=${1:-"$REPO_DIR/$DEFAULT_ROM"}
CAPTURE_NAME=${2:-ram-capture}
MAME_ROOT="$REPO_DIR/generated/mame"
LUA_SCRIPT="$REPO_DIR/mame/rr206-ram-diff.lua"

case "$CAPTURE_NAME" in
    *[!A-Za-z0-9_-]* | "")
        echo "error: capture name may contain only letters, numbers, underscores, and hyphens" >&2
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
ACTUAL_SHA256=$(shasum -a 256 -- "$ROM_PATH" | awk '{print $1}')
if [ "$ACTUAL_SHA256" != "$EXPECTED_SHA256" ]; then
    echo "error: refusing launch; RR206 SHA-256 mismatch" >&2
    exit 1
fi

mkdir -p \
    "$MAME_ROOT/cfg" \
    "$MAME_ROOT/nvram" \
    "$MAME_ROOT/state" \
    "$MAME_ROOT/snap" \
    "$MAME_ROOT/input"

RR2_CAPTURE_PREFIX="$MAME_ROOT/$CAPTURE_NAME"
export RR2_CAPTURE_PREFIX

cd "$MAME_ROOT"
exec mame genesis \
    -cart "$ROM_PATH" \
    -ctrl1 mdpad \
    -ctrl2 mdpad \
    -autoboot_script "$LUA_SCRIPT" \
    -autoboot_delay 0 \
    -record "$CAPTURE_NAME.inp" \
    -window \
    -skip_gameinfo \
    -cfg_directory "$MAME_ROOT/cfg" \
    -nvram_directory "$MAME_ROOT/nvram" \
    -state_directory "$MAME_ROOT/state" \
    -snapshot_directory "$MAME_ROOT/snap" \
    -input_directory "$MAME_ROOT"
