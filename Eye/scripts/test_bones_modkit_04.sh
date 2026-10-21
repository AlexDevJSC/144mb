#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
mkdir -p "$ROOT/Eye/build"
${CC:-cc} -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
    "$ROOT/Eye/tests/test_bones_modkit_04.c" \
    "$ROOT/BONES/modules/eye/modkit.c" \
    "$ROOT/BONES/third_party/eye/sw_renderer.c" -lm \
    -o "$ROOT/Eye/build/test_bones_modkit_04"
"$ROOT/Eye/build/test_bones_modkit_04"
make -C "$ROOT/BONES" modkit_test
