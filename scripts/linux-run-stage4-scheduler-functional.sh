#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Compare *existing* Win32 priority/affinity fixture contracts on Linux.
# This is a hosted Linux PE observation, never physical Fedora acceptance.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
report="${1:-build/stage4-linux-priority-affinity.txt}"
mkdir -p "$(dirname "$report")"
cat > "$report" <<EOF
SCHEMA=ntnative.stage4.priority-affinity.functional.v1
PLATFORM=LINUX_GUEST_PE
COMMIT=$(git rev-parse HEAD)
KERNEL=$(uname -r)
PHYSICAL_FEDORA_VERIFIED=NO
NT_SCHEDULER_PARITY_VERIFIED=NO
EOF
failure=0
# Each target compiles a real MinGW PE and executes it through our runtime.
# A test's own zero exit code means it met *its* assumptions, not Windows
# semantic parity. Keep separate stdout/stderr for audit of each fixture.
run_contract() {
    local name="$1" target="$2" fixture="$3"
    local log="build/stage4-linux-functional-${name,,}.log"
    local status=FAIL
    if SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 \
        make -j2 "$target" >"$log" 2>&1 &&
        grep -Fq "build/$fixture" "$log" &&
        grep -Fq 'PASS trusted external PE32+ fixture mapped and executed on Linux' "$log"; then
        status=PASS
    else
        failure=1
    fi
    printf '%s_RESULT=%s\n' "$name" "$status" >> "$report"
    printf '%s_LOG=%s\n' "$name" "$log" >> "$report"
    printf 'STAGE4_LINUX_%s=%s\n' "$name" "$status"
    if [[ "$status" == FAIL ]]; then
        tail -35 "$log" >&2
    fi
}
run_contract THREAD_GROUP check-pe-thread-groups thread-group-affinity-fixture.exe
run_contract PROCESS_AFFINITY check-pe-process-affinity process-affinity-mask-fixture.exe
run_contract PRIORITY_CLASS check-pe-priority-class priority-class-fixture.exe
run_contract PRIORITY_BOOST_BOUNDARY check-pe-priority-boost priority-boost-boundary-fixture.exe
cat "$report"
exit "$failure"
