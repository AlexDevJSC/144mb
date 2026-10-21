#!/bin/sh
# Gated runner for PR11 actual house fixture, not a simulated collision.
set -eu
if [ "${BRAIN_SOUL_PHASE1_APPROVED:-}" != "yes" ]; then
  echo "BLOQUEADO: se necesita aprobacion explicita de Soul (BRAIN_SOUL_PHASE1_APPROVED=yes)." >&2
  exit 3
fi
ROOT="$(cd "$(dirname "$0")/../../../../" && pwd)"
cd "$ROOT/BONES"
if ! grep -q 'BONES_HOUSE_COLLIDERS' src/demo/test_house.h; then
  echo "BLOQUEADO: esta revision de Bones no incluye casa corregida PR11." >&2
  exit 4
fi
mkdir -p build
${CC:-cc} -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc/demo \
  "$ROOT/Brain/bridge/bones_0_4/tests/test_brain_house_fixture.c" \
  src/bones/core.c src/bones/world.c src/bones/physics.c \
  src/bones/scene.c src/bones/subsystems.c src/foundation/foundation.c \
  src/modkit/modkit.c src/demo/test_house.c modules/brain/modkit.c \
  -o build/test_brain_house_fixture -lm -ldl
./build/test_brain_house_fixture
