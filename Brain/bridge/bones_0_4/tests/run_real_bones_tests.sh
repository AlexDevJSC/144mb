#!/bin/sh
# From the checkout with BONES/ and Brain/ as sibling directories.
# Do NOT interpret passes on old Bones as validation of the playtest fixes.
set -eu
REPO_ROOT="$(cd "$(dirname "$0")/../../../../" && pwd)"
BONES_ROOT="$REPO_ROOT/BONES"
BRAIN_ROOT="$REPO_ROOT/Brain"
TEST="$BRAIN_ROOT/bridge/bones_0_4/tests/test_brain_real_bones.c"
if [ ! -f "$BONES_ROOT/include/bones/modkit_host.h" ] || [ ! -f "$TEST" ]; then
    echo "ERROR: se requieren carpetas BONES/ y Brain/ del mismo checkout" >&2
    exit 2
fi
cd "$BONES_ROOT"
mkdir -p build
${CC:-cc} -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -Iinclude \
    "$TEST" \
    src/bones/core.c src/bones/world.c src/bones/physics.c \
    src/bones/scene.c src/bones/subsystems.c src/foundation/foundation.c \
    src/modkit/modkit.c modules/brain/modkit.c \
    -o build/test_brain_real_bones -lm -ldl
./build/test_brain_real_bones
