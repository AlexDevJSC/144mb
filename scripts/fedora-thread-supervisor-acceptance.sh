#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# No privilege escalation, SELinux adjustment or ptrace_scope changes.
set -uo pipefail
cd "$(dirname "$0")/.."
run_dir="${NT_SUPERVISOR_REPORT_DIR:-build/supervisor-fedora-acceptance}"
mkdir -p "$run_dir" || exit 2
facts="$run_dir/environment.txt"
log="$run_dir/test.log"
report="$run_dir/result.txt"
{
  printf 'REVISION='; git rev-parse HEAD 2>/dev/null || printf 'UNAVAILABLE\n'
  printf 'DATE_UTC='; date -u +%Y-%m-%dT%H:%M:%SZ
  printf 'KERNEL='; uname -srvm
  printf 'ARCH='; uname -m
  printf 'FEDORA='; cat /etc/fedora-release 2>/dev/null || printf 'NOT_FEDORA\n'
  printf 'SELINUX='; getenforce 2>/dev/null || printf 'NOT_AVAILABLE\n'
  printf 'YAMA_PTRACE_SCOPE='; cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null || printf 'NOT_AVAILABLE\n'
  printf 'EUID='; id -u
  printf 'CC='; "${CC:-cc}" --version 2>/dev/null | head -1 || true
} > "$facts"
if (( EUID == 0 )); then
  printf 'RESULT=FAIL\nREASON=Do not run as root\n' | tee "$report"
  exit 2
fi
printf 'ENVIRONMENT=%s\n' "$facts"
set +e
bash scripts/check-thread-supervisor.sh 2>&1 | tee "$log"
rc=${PIPESTATUS[0]}
set -e
case "$rc" in
  0)  result=PASS ;;
  77) result=INCOMPLETE ;;
  *)  result=FAIL ;;
esac
{
  printf 'RESULT=%s\n' "$result"
  printf 'EXIT_CODE=%s\n' "$rc"
  printf 'LOG=%s\n' "$log"
  printf 'ENVIRONMENT=%s\n' "$facts"
} | tee "$report"
exit "$rc"
