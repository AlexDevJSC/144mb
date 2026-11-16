#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Strict on-machine Fedora KDE + real multicpu-group validation.
# This does NOT run in GitHub's Fedora container or certify Windows parity.
set -Eeuo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
report_dir="${1:-build/stage4-physical-multigroup}"
mkdir -p -- "$report_dir"
report_dir=$(cd -- "$report_dir" && pwd)
log="$report_dir/fedora-stage4.log"
facts="$report_dir/fedora-stage4-facts.txt"

fail() { printf 'INCOMPLETE: %s\n' "$*" >&2; exit 3; }
[[ $(uname -m) == x86_64 ]] || fail "only x86_64 Fedora is supported"
grep -Eq '^ID="?fedora"?$' /etc/os-release || fail "must execute on the Fedora host, not a container from another distribution"
case "${XDG_CURRENT_DESKTOP:-}" in
    *KDE*|*Plasma*|*PLASMA*|*plasma*) ;;
    *) fail "launch from a local Fedora KDE Plasma graphical session" ;;
esac
case "${XDG_SESSION_TYPE:-}" in
    wayland|x11) ;;
    *) fail "a real Wayland or X11 Plasma session is required" ;;
esac
command -v systemd-detect-virt >/dev/null || fail "systemd-detect-virt missing"
if systemd-detect-virt --quiet; then
    fail "VM or container detected; physical Fedora validation cannot be claimed"
fi
for tool in python3 git make cc bwrap x86_64-w64-mingw32-gcc; do
    command -v "$tool" >/dev/null || fail "missing $tool"
done

# Validate the allowed CPUs, not merely the host's /proc/cpuinfo or nproc.
# Linux affinity is per-TID and a container can hide its actual entitlement.
set +e
python3 - "$facts" <<'PY'
import os
import platform
import subprocess
import sys

path = sys.argv[1]
allowed = sorted(os.sched_getaffinity(0))
# Windows guest groups are compact ordinal blocks of allowed CPUs, not
# host Linux CPU IDs divided by 64. Keep both for diagnostics.
host_bins = sorted({cpu // 64 for cpu in allowed})
guest_groups = (len(allowed) + 63) // 64
with open(path, "w", encoding="utf-8") as result:
    result.write("SCHEMA=ntnative.stage4.scheduler.v1\n")
    result.write("PLATFORM=FEDORA_KDE_PHYSICAL\n")
    result.write(f"ACTIVE_CPUS={len(allowed)}\n")
    result.write(f"GROUPS={guest_groups}\n")
    result.write("GROUP_IDS=" + ",".join(map(str, range(guest_groups))) + "\n")
    result.write("LINUX_HOST_CPU64_BINS=" + ",".join(map(str, host_bins)) + "\n")
    result.write(f"CPUS_ALLOWED={','.join(map(str, allowed))}\n")
    result.write("KERNEL=" + platform.release() + "\n")
    # These are host scheduler diagnostics, NOT a Windows NT quantum.
    result.write(f"LINUX_NICE={os.getpriority(os.PRIO_PROCESS, 0)}\n")
    result.write(f"LINUX_SCHED_POLICY={os.sched_getscheduler(0)}\n")
    try:
        interval = os.sched_rr_get_interval(0)
        result.write(f"LINUX_RR_INTERVAL_SECONDS={interval:.9f}\n")
    except OSError as exc:
        result.write(f"LINUX_RR_INTERVAL_ERROR={exc.errno}\n")
    result.write("GIT_COMMIT=" + subprocess.check_output(
        ["git", "rev-parse", "HEAD"], text=True).strip() + "\n")
    result.write("PHYSICAL_HARDWARE_VERIFIED=HEURISTIC_ONLY\n")
    result.write("PROCESSOR_GROUP_TOPOLOGY=COMPACT_ALLOWED_CPU_ORDINALS\n")
    result.write("DYNAMIC_BOOST=OPT_IN_REVERSIBLE_NICE_NOT_NT_EQUIVALENT\n")
    result.write("QUANTUM=NOT_NT_PARITY_VERIFIED\n")
    result.write("EXTERNAL_THREADS=QUERY_ONLY_AUTHENTICATED_GUEST_CHILDREN\n")
if len(allowed) < 65 or guest_groups < 2:
    print("INCOMPLETE: need >=65 allowed CPUs for at least two compact guest groups", file=sys.stderr)
    sys.exit(3)
print(f"PRECHECK: {len(allowed)} CPUs across {guest_groups} compact guest groups; host bins {host_bins}")
PY
preflight=$?
set -e
if [[ "$preflight" -ne 0 ]]; then exit "$preflight"; fi

echo "Checking stock kernel, Fedora session and Bubblewrap; no sudo or sandbox bypass."
# The normal acceptance script also checks that Bubblewrap actually launched
# the PE/child suite without the known CI SKIP escape.
if ! bash scripts/fedora-check-stage4.sh 2>&1 | tee "$log"; then
    fail "stage4 runtime/PE/UBSan/Bubblewrap tests failed; inspect $log"
fi
if ! grep -Eq '^PASS group affinity: .*cross-group=yes' "$log"; then
    fail "no successful cross-group kernel transition observed, even though topology is adequate"
fi
if ! grep -q '^PASS multigroup process policy: inherited secondary group denied, restored and committed' "$log"; then
    fail "no verified inherited secondary-group rejection and subsequent process-policy commit"
fi
if grep -q 'SKIP external-PE sandbox test' "$log"; then
    fail "Bubblewrap isolation was skipped"
fi
if ! grep -q '^PASS: stage4 tests completed without mandatory SKIP' "$log"; then
    fail "base stage4 acceptance evidence absent"
fi
{
    echo "MULTIGROUP=PASS"
    echo "PROCESS_GROUP_POLICY=PASS"
    echo "THREAD_GROUP=PASS"
    echo "LEGACY_MASK_PRIMARY_GROUP=PASS"
    echo "ISOLATED_PE=PASS"
    if grep -q 'PASS privileged multithread priority transition' "$log"; then
        echo "PRIORITY_MULTITHREAD_POSITIVE=PASS"
    else
        echo "PRIORITY_MULTITHREAD_POSITIVE=INCOMPLETE"
    fi
    if grep -q 'PASS dynamic priority: real Linux nice wake boost' "$log"; then
        echo "DYNAMIC_BOOST_POSITIVE=PASS"
    else
        echo "DYNAMIC_BOOST_POSITIVE=INCOMPLETE"
    fi
} >> "$facts"
echo "PASS: real Fedora KDE multigroup transitions in current physical session."
echo "EVIDENCE=$facts"
echo "LOG=$log"
echo "LIMITATIONS: this is not Windows 11 multicpu primary-group or dynamic boost parity."
