#!/usr/bin/env bash
# Native (Mac/Linux) test of firmware/custom-fw/src/esp1_frame.h: CRC16, COBS (fuzzed), the epoch
# rule, BLIT length checks and the UART framer incl. both lost-delimiter traces. No hardware; ~1 s.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../firmware/custom-fw/src"
OUT="${TMPDIR:-/tmp}/esp1_frame_native"
CXX="${CXX:-clang++}"
"$CXX" -std=c++17 -O2 -Wall -I "$SRC" "$HERE/esp1_frame_native.cpp" -o "$OUT"
"$OUT"
