#!/bin/sh
# Replay an input recording and log where every known indirect jump or call goes.
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

OUT_DIR="$REPO_DIR/generated/port/indirect"
RUN_DIR="$OUT_DIR/mame-run-$CAPTURE_NAME"
rm -rf "$RUN_DIR"
mkdir -p "$RUN_DIR" "$MAME_ROOT/cfg" "$MAME_ROOT/nvram"

cd "$REPO_DIR"
uv run python -c "
import sys
from pathlib import Path
from rr2port.discover import discover, indirect_sites, load_annotations
from rr2port.indirect import build_indirect_cmd
rom = Path(sys.argv[1]).read_bytes()
a = load_annotations(Path('port/annotations/rr206.toml'))
insns = discover(rom, a.entries, a.jump_tables, callback_slots=a.callback_slots,
                 callback_registrars=a.callback_registrars, pointer_tables=a.pointer_tables)
sys.stdout.write(build_indirect_cmd({pc: insns[pc] for pc in indirect_sites(insns)}))
" "$ROM_PATH" > "$RUN_DIR/indirect.cmd"

cd "$RUN_DIR"
mame genesis \
    -cart "$ROM_PATH" \
    -ctrl1 mdpad \
    -ctrl2 mdpad \
    -playback "$CAPTURE_NAME.inp" \
    -exit_after_playback \
    -debug \
    -debugger osx \
    -debugscript "$RUN_DIR/indirect.cmd" \
    -log \
    -window \
    -sound none \
    -nothrottle \
    -skip_gameinfo \
    -cfg_directory "$MAME_ROOT/cfg" \
    -nvram_directory "$MAME_ROOT/nvram" \
    -input_directory "$MAME_ROOT"

cd "$REPO_DIR"
uv run python -c "
import sys
from pathlib import Path
from rr2port.indirect import read_indirect_log
observed = read_indirect_log(Path(sys.argv[1]))
lines = [f'{site:06X} {target:06X}' for site in sorted(observed) for target in sorted(observed[site])]
Path(sys.argv[2]).write_text(''.join(line + '\n' for line in lines))
print(f'{len(observed)} sites, {len(lines)} site/target pairs')
" "$RUN_DIR/error.log" "$OUT_DIR/$CAPTURE_NAME.txt"
rm -rf "$RUN_DIR"
echo "wrote $OUT_DIR/$CAPTURE_NAME.txt"
