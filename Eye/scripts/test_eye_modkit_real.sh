#!/bin/sh
set -eu
# Script lives under Eye/scripts and compiles ACTUAL Bones 0.4 source.
# NEVER add eye*.c separately: Bones/modules/eye/modkit.c is a unity build.
cd "$(dirname "$0")/../.."
exe="${TMPDIR:-/tmp}/eye_modkit_real_test_$$"
trap 'rm -f "$exe"' EXIT HUP INT TERM
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
  BONES/modules/eye/modkit.c BONES/third_party/eye/sw_renderer.c \
  Eye/tests/test_modkit_eye_real.c -lm -o "$exe"
"$exe"
