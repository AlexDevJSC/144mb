#!/bin/sh
# Optional cross-module smoke test with actual Eye sources, NOT an Eye mock.
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
if [ ! -f "$ROOT/Eye/src/eye2.c" ] || [ ! -f "$ROOT/Eye/include/eye2.h" ]; then
  echo "BLOQUEADO: Eye/src/eye2.c e include/eye2.h deben estar en el checkout." >&2
  exit 4
fi
mkdir -p "$ROOT/Brain/build"
${CC:-cc} -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
    -I"$ROOT/Eye/include" -I"$ROOT/Brain/include" \
    "$ROOT/Brain/tests/test_eye1_eye2_boundary.c" \
    "$ROOT/Brain/src/brain_visual.c" \
    "$ROOT/Eye/src/eye.c" "$ROOT/Eye/src/eye2.c" "$ROOT/Eye/src/eye3.c" "$ROOT/Eye/src/eye_tone.c" \
    -o "$ROOT/Brain/build/test_eye1_eye2_boundary" -lm
"$ROOT/Brain/build/test_eye1_eye2_boundary"
