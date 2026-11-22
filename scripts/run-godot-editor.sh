#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../godot" && pwd)
GODOT_BIN=/Applications/Godot.app/Contents/MacOS/Godot

if [ ! -x "$GODOT_BIN" ]; then
    echo "Godot was not found at $GODOT_BIN" >&2
    exit 1
fi

export GODOT_AI_DISABLE_TELEMETRY=true
exec "$GODOT_BIN" --editor --path "$PROJECT_DIR" "$@"
