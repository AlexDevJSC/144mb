#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Core 4 manual acceptance; purposely NOT invoked by CI or by the author.
# Run unprivileged on the physical Fedora host after reviewing the changes.
set -euo pipefail
cd "$(dirname "$0")/.."
rounds="${1:-100}"
case "$rounds" in
  ''|*[!0-9]*) echo "Usage: $0 [positive-repetition-count]" >&2; exit 64 ;;
esac
if [ "$rounds" -lt 1 ] || [ "$rounds" -gt 10000 ]; then
  echo "Repetition count must be between 1 and 10000" >&2
  exit 64
fi

printf '%s\n' '[Core4] Compiling the manual regression targets'
make -j2 build/supervisor-control-ipc-test \
          build/supervisor-context-rollback-ok-test \
          build/supervisor-context-rollback-fatal-test \
          build/runtime-supervisor-win32-test \
          build/broker-transport-failure-test \
          build/guest-boundary-import-test \
          build/guest-boundary-cet-test \
          build/pe-async-supervisor-host \
          build/async-supervisor-fixture.exe

printf '%s\n' '[Core4] Protocol corruption, FXSAVE, boundary and Windows PE regression'
timeout 35s build/broker-transport-failure-test
timeout 20s build/supervisor-context-rollback-ok-test
timeout 20s build/supervisor-context-rollback-fatal-test
timeout 20s build/guest-boundary-import-test
timeout 10s build/guest-boundary-cet-test
timeout 30s build/runtime-supervisor-win32-test
timeout 30s build/pe-async-supervisor-host build/async-supervisor-fixture.exe

printf '[Core4] Repeating kernel ptrace/IPC lifecycle %s times\n' "$rounds"
for ((i=1; i<=rounds; ++i)); do
  if ! timeout 35s build/supervisor-control-ipc-test; then
    printf '[Core4] FAILURE at repetition %d / %d\n' "$i" "$rounds" >&2
    exit 1
  fi
  if (( i % 25 == 0 || i == rounds )); then
    printf '[Core4] completed %d / %d\n' "$i" "$rounds"
  fi
done
printf '%s\n' '[Core4] PASS of the selected fixtures only.'
printf '%s\n' '[Core4] This is not Windows compatibility certification or Bubblewrap acceptance.'
