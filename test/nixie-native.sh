#!/usr/bin/env bash
# Native (Mac/Linux) test of the nixie render core: every glyph pair × every fade
# step, RLE integrity, and all 86,400 clock times through the tick() transition
# logic. No hardware needed; ~seconds. Re-run after tools/gen_nixie_glyphs.py.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../firmware/custom-fw/src"
OUT="${TMPDIR:-/tmp}/nixie_native"
CXX="${CXX:-clang++}"
"$CXX" -std=c++17 -O2 -Wall -I "$SRC" "$HERE/nixie_native.cpp" -o "$OUT"
"$OUT"
