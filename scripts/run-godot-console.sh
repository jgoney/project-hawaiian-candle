#!/bin/sh
# Run the natively translated game in a Godot window.
# Usage: run-godot-console.sh [ROM]
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
GODOT_BIN=${GODOT_BIN:-/Applications/Godot.app/Contents/MacOS/Godot}

if [ ! -x "$GODOT_BIN" ]; then
    echo "error: Godot was not found at $GODOT_BIN (set GODOT_BIN)" >&2
    exit 1
fi
if [ ! -f "$REPO_DIR/godot/bin/librr2.dylib" ]; then
    echo "error: native library not built; run scripts/build-godot-extension.sh" >&2
    exit 1
fi
if [ "$#" -gt 0 ]; then
    RR2_ROM=$1
    export RR2_ROM
fi
export GODOT_AI_DISABLE_TELEMETRY=true
exec "$GODOT_BIN" --path "$REPO_DIR/godot" res://scenes/console.tscn
