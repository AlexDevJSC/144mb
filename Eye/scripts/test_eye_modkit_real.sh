#!/bin/sh
set -eu
# Invoke at the repository root, not within Eye/. No extra eye*.c linking:
# EYE's modkit.c includes them as a unity build (one translation unit).
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
  BONES/modules/eye/modkit.c BONES/third_party/eye/sw_renderer.c \
  Eye/tests/test_modkit_eye_real.c -lm -o /tmp/eye_modkit_real_test
/tmp/eye_modkit_real_test
