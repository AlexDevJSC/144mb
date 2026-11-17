#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Controlled PE fixture only. This is deliberately NOT the production
# pe-isolated-run sandbox: the harness needs GCC/glibc under /usr and ptrace.
set -uo pipefail
cd "$(dirname "$0")/.."
build_dir="${BUILD_DIR:-build}"
runner="$PWD/$build_dir/supervisor-pe-thread-test"
fixture="$PWD/$build_dir/supervisor-pe-fixture.exe"

if [[ "$(uname -m)" != x86_64 ]]; then
  echo 'INCOMPLETE unsupported architecture (requires x86_64)' >&2
  exit 77
fi
if ! command -v bwrap >/dev/null 2>&1; then
  echo 'INCOMPLETE Bubblewrap is not installed' >&2
  exit 77
fi
if [[ ! -x "$runner" || ! -r "$fixture" ]]; then
  echo 'FAIL missing built PE fixture or supervisor test; run make check-pe-thread-supervisor first' >&2
  exit 2
fi

# The namespace contains only /usr (system toolchain/runtime), /etc (glibc
# config), a read-only copy of this checkout, proc/dev, and ephemeral /tmp.
# Supervisor+tracee are both children *inside* the same Bubblewrap PID/user
# namespace; do not attach through the namespace boundary or grant CAP_SYS_PTRACE.
# /lib and /lib64 are usr-merge symlinks on Ubuntu 24.04 and Fedora 45.
# The known, freshly compiled test PE is the only guest executable allowed.
work_dir="/work"
log="$(mktemp)"
trap 'rm -f "$log"' EXIT
set +e
timeout 25s bwrap --unshare-user --unshare-pid --unshare-ipc --unshare-net \
  --new-session --die-with-parent \
  --ro-bind /usr /usr \
  --ro-bind /etc /etc \
  --ro-bind "$PWD" "$work_dir" \
  --symlink usr/bin /bin \
  --symlink usr/lib /lib \
  --symlink usr/lib64 /lib64 \
  --symlink usr/sbin /sbin \
  --proc /proc --dev /dev --tmpfs /tmp \
  --chdir "$work_dir" \
  -- "$work_dir/$build_dir/supervisor-pe-thread-test" \
     "$work_dir/$build_dir/supervisor-pe-fixture.exe" >"$log" 2>&1
result=$?
set -e
cat "$log"
if (( result == 0 )); then
  if ! grep -q '^PASS supervisor PE32+:' "$log"; then
    echo 'FAIL sandbox exited without positive PE supervision evidence' >&2
    exit 1
  fi
  echo 'PASS Bubblewrap: supervisor and controlled PE tracee inside user/PID namespaces'
  exit 0
fi
if (( result == 77 )); then
  echo 'INCOMPLETE Bubblewrap PE ptrace policy rejects execution' >&2
  exit 77
fi
if grep -Eiq '(creating new namespace failed|user namespaces.*not permitted|operation not permitted|permission denied|unprivileged user namespaces|setting up uid map)' "$log"; then
  echo 'INCOMPLETE sandbox namespaces/ptrace denied by host policy' >&2
  exit 77
fi
echo "FAIL Bubblewrap controlled PE test exit=$result" >&2
exit 1
