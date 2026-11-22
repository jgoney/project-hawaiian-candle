#!/bin/sh
set -eu

EXPECTED_SHA256="c7a609019b1f052bcee6204a3a09ea77a5150a748de90b51382fedf4df207848"
DEFAULT_ROM="Road Rash II (USA, Europe) (RR206).gen"
DEFAULT_GHIDRA_HEADLESS="/opt/homebrew/Cellar/ghidra/12.0.4/libexec/support/analyzeHeadless"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
ROM_PATH=${1:-"$REPO_DIR/$DEFAULT_ROM"}
PROJECT_DIR=${2:-"$REPO_DIR/ghidra-projects"}
GHIDRA_HEADLESS_BIN=${GHIDRA_HEADLESS_BIN:-$DEFAULT_GHIDRA_HEADLESS}

if [ ! -f "$ROM_PATH" ]; then
    echo "error: RR206 ROM not found: $ROM_PATH" >&2
    exit 1
fi

ACTUAL_SHA256=$(shasum -a 256 -- "$ROM_PATH" | awk '{print $1}')
if [ "$ACTUAL_SHA256" != "$EXPECTED_SHA256" ]; then
    echo "error: refusing import; RR206 SHA-256 mismatch" >&2
    echo "expected: $EXPECTED_SHA256" >&2
    echo "actual:   $ACTUAL_SHA256" >&2
    exit 1
fi

mkdir -p "$PROJECT_DIR"

exec "$GHIDRA_HEADLESS_BIN" "$PROJECT_DIR" rr2-rr206 \
    -import "$ROM_PATH" \
    -overwrite \
    -loader BinaryLoader \
    -loader-baseAddr 0x00000000 \
    -processor 68000:BE:32:MC68020 \
    -cspec default \
    -scriptPath "$REPO_DIR/ghidra/scripts" \
    -preScript GenesisImport.java \
    -postScript ApplyRr206Annotations.java

