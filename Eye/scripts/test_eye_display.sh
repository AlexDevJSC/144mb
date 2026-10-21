#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -Imodules/eye \
   modules/eye/eye_display.c tests/test_eye_display.c -lm -o /tmp/eye_display_test
/tmp/eye_display_test
