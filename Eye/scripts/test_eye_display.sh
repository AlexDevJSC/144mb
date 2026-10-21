#!/bin/sh
set -eu
# From any current working directory; script lives under Eye/scripts/.
cd "$(dirname "$0")/../.."
exe="${TMPDIR:-/tmp}/eye_display_test_$$"
trap 'rm -f "$exe"' EXIT HUP INT TERM
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -I BONES/modules/eye \
   BONES/modules/eye/eye_display.c Eye/tests/test_eye_display.c -lm -o "$exe"
"$exe"
