#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# No privilege escalation, SELinux adjustment or ptrace_scope changes.
set -uo pipefail
cd "$(dirname "$0")/.."
run_dir="${NT_SUPERVISOR_REPORT_DIR:-build/supervisor-fedora-acceptance}"
mkdir -p "$run_dir" || exit 2
facts="$run_dir/environment.txt"
log="$run_dir/test.log"
pe_log="$run_dir/pe-test.log"
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
native_rc=${PIPESTATUS[0]}
make -j2 check-pe-thread-supervisor 2>&1 | tee "$pe_log"
pe_rc=${PIPESTATUS[0]}
set -e
if (( native_rc != 0 && native_rc != 77 )) || (( pe_rc != 0 && pe_rc != 77 )); then
  result=FAIL; rc=1
elif (( native_rc == 77 || pe_rc == 77 )); then
  result=INCOMPLETE; rc=77
else
  result=PASS; rc=0
fi
{
  printf 'RESULT=%s\n' "$result"
  printf 'EXIT_CODE=%s\n' "$rc"
  printf 'NATIVE_EXIT_CODE=%s\n' "$native_rc"
  printf 'PE_EXIT_CODE=%s\n' "$pe_rc"
  printf 'NATIVE_LOG=%s\n' "$log"
  printf 'PE_LOG=%s\n' "$pe_log"
  printf 'ENVIRONMENT=%s\n' "$facts"
} | tee "$report"
exit "$rc"
