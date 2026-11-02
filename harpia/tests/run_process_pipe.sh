#!/bin/bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
WORK="${1:-$(mktemp -d)}"
mkdir -p "$WORK"
gcc -std=c11 -D_POSIX_C_SOURCE=200809L -I"$ROOT/libobs" \
    "$HERE/processpipe_test.c" "$ROOT/libobs/util/pipe-posix.c" -o "$WORK/processpipe_test"
# Unused creation/argument routines are discarded; the pipe I/O is production code.
gcc -std=c11 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I"$HERE/support/process_pipe" -I"$ROOT/libobs" \
    "$HERE/processpipe_windows_test.c" -o "$WORK/processpipe_windows_test"
"$WORK/processpipe_test"
"$WORK/processpipe_windows_test"
