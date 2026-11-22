#!/bin/sh
# Translate the ROM and build the native library the Godot project loads.
# Usage: build-godot-extension.sh [ROM]
set -eu

EXPECTED_ROM_SHA256="c7a609019b1f052bcee6204a3a09ea77a5150a748de90b51382fedf4df207848"
DEFAULT_ROM="Road Rash II (USA, Europe) (RR206).gen"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
ROM_PATH=${1:-"$REPO_DIR/$DEFAULT_ROM"}
GODOT_BIN=${GODOT_BIN:-/Applications/Godot.app/Contents/MacOS/Godot}

if [ ! -f "$ROM_PATH" ]; then
    echo "error: RR206 ROM not found: $ROM_PATH" >&2
    exit 1
fi
ACTUAL_ROM_SHA256=$(shasum -a 256 -- "$ROM_PATH" | awk '{print $1}')
if [ "$ACTUAL_ROM_SHA256" != "$EXPECTED_ROM_SHA256" ]; then
    echo "error: refusing build; RR206 SHA-256 mismatch for $ROM_PATH" >&2
    exit 1
fi
if [ ! -x "$GODOT_BIN" ]; then
    echo "error: Godot was not found at $GODOT_BIN (set GODOT_BIN)" >&2
    exit 1
fi

cd "$REPO_DIR"
uv run rr2-translate "$ROM_PATH" --annotations port/annotations/rr206.toml \
    -o generated/port/rr206_code.c

# Godot's C interface header comes from the installed Godot, so it always matches it.
mkdir -p generated/port/godot
(cd generated/port/godot && "$GODOT_BIN" --headless --dump-gdextension-interface >/dev/null 2>&1)
if [ ! -f generated/port/godot/gdextension_interface.h ]; then
    echo "error: Godot did not write gdextension_interface.h" >&2
    exit 1
fi

make -C port console-lib godot-extension
# Godot only sees a new extension after it has scanned the project once.
"$GODOT_BIN" --headless --path godot --import >/dev/null 2>&1 || true
echo "built $REPO_DIR/godot/bin/librr2.dylib"
