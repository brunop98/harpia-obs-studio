#!/bin/bash
# Pass the libobs build's library directory as OBS_LIB_DIR.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
H="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$H/.." && pwd)"
WORK="${1:-$(mktemp -d)}"
mkdir -p "$WORK"
: "${OBS_LIB_DIR:?Set OBS_LIB_DIR to the built libobs directory}"
g++ -std=c++17 -O0 -fPIC -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I"$H" -I"$ROOT/libobs" -I"$OBS_LIB_DIR/../config" $(pkg-config --cflags Qt6Core) \
    "$HERE/recordingstop_test.cpp" "$H/core/RecordingController.cpp" \
    -L"$OBS_LIB_DIR" -Wl,-rpath,"$OBS_LIB_DIR" -lobs \
    -o "$WORK/recordingstop_test" $(pkg-config --libs Qt6Core)
"$WORK/recordingstop_test"
