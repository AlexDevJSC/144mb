#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir="${BUILD_DIR:-build}"
mkdir -p "$build_dir"
# CFLAGS is intentionally split into independent compiler arguments.
read -r -a custom_flags <<< "${CFLAGS:--O2 -g}"
"${CC:-cc}" -std=c17 -Wall -Wextra -Wpedantic -Werror \
  -Iinclude -Isrc/supervisor "${custom_flags[@]}" \
  src/supervisor/nt_thread_supervisor.c tests/thread_supervisor_test.c \
  -pthread -o "$build_dir/nt-thread-supervisor-test"
# 0 = demonstrated on this host; 77 = not testable under this ptrace policy.
# Never rewrite 77 into success.
timeout 20s "$build_dir/nt-thread-supervisor-test"
