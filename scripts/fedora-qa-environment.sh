#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Fedora physical QA gate: never transform host limitations into a PASS.
# Privileged RLIMIT_NICE is opt-in; tests run after dropping privileges.
set -Eeuo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

install_audio=0
priority=0
require_multigroup=0
for arg in "$@"; do
    case "$arg" in
        --install-audio) install_audio=1 ;;
        --with-priority) priority=1 ;;
        --require-multigroup) require_multigroup=1 ;;
        --help)
            cat <<'USAGE'
Usage: bash scripts/fedora-qa-environment.sh [--install-audio] [--with-priority] [--require-multigroup]
  --install-audio       Explicitly install fluidsynth-libs with sudo dnf
  --with-priority       Grant a disposable test RLIMIT_NICE using sudo,
                        then drop to your UID/GID BEFORE executing test code
  --require-multigroup  Reject a physical host lacking two CPU groups
USAGE
            exit 0 ;;
        *) printf 'Unknown option: %s\n' "$arg" >&2; exit 2 ;;
    esac
done

if [[ ! -r /etc/os-release ]]; then
    echo "FAIL: /etc/os-release unavailable" >&2
    exit 2
fi
source /etc/os-release
if [[ "${ID:-}" != fedora ]] || [[ "$(uname -m)" != x86_64 ]]; then
    echo "FAIL: this acceptance gate requires Fedora x86_64" >&2
    exit 2
fi
for tool in make rpm python3 x86_64-w64-mingw32-gcc bwrap timeout; do
    if ! command -v "$tool" >/dev/null; then
        printf 'INCOMPLETE prerequisite: %s not installed\n' "$tool" >&2
        exit 3
    fi
done

if [[ "$install_audio" == 1 ]]; then
    if ! command -v sudo >/dev/null; then
        echo "INCOMPLETE: sudo unavailable for explicitly requested installation" >&2
        exit 3
    fi
    sudo dnf install -y fluidsynth-libs
fi
if ! rpm -q fluidsynth-libs >/dev/null 2>&1; then
    echo "INCOMPLETE dependency: fluidsynth-libs not installed." >&2
    echo "Run: bash scripts/fedora-qa-environment.sh --install-audio" >&2
    exit 3
fi

echo "[1/4] PE direct-import preflight (must not skip)"
make -j2 check-pe-preflight
echo "[2/4] Native FluidSynth ABI (must not skip)"
make -j2 check-audio-required

echo "[3/4] Bubblewrap isolated PE and real child process (must not skip)"
log=$(mktemp)
trap 'rm -f -- "$log"' EXIT
if ! SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 \
     make -j2 check-pe-isolated 2>&1 | tee "$log"; then
    echo "FAIL: runtime's isolated PE/child execution failed" >&2
    exit 1
fi
if grep -q 'SKIP external-PE sandbox test' "$log" ||
   ! grep -q 'PASS PE child launched inside the same Linux sandbox' "$log"; then
    echo "INCOMPLETE: namespace policy prevented real isolated child test" >&2
    exit 3
fi

echo "[4/4] Scheduler rights and CPU topology"
if [[ "$priority" == 1 ]]; then
    for tool in sudo prlimit setpriv; do
        if ! command -v "$tool" >/dev/null; then
            printf 'INCOMPLETE: %s required for opted-in reversible priority tests\n' "$tool" >&2
            exit 3
        fi
    done
    make -j2 build/process-priority-multithread-test build/dynamic-priority-boost-test
    uid=$(id -u)
    gid=$(id -g)
    if [[ "$uid" == 0 ]]; then
        echo "FAIL: run this script as a normal user, not root" >&2
        exit 2
    fi
    # prlimit is privileged, the actual tests are not. Never execute an
    # untrusted PE or the full runtime as root.
    sudo prlimit --nice=40:40 -- setpriv --reuid="$uid" --regid="$gid" --init-groups -- \
        env NT_REQUIRE_POSITIVE_MULTITHREAD_PRIORITY=1 ./build/process-priority-multithread-test
    sudo prlimit --nice=40:40 -- setpriv --reuid="$uid" --regid="$gid" --init-groups -- \
        env NT_NATIVE_EXPERIMENTAL_BOOST=1 NT_REQUIRE_POSITIVE_BOOST=1 ./build/dynamic-priority-boost-test
    echo "PASS reversible priority transitions in nonroot disposable clients"
else
    echo "PENDING privilege-dependent priority checks; opt in with --with-priority"
fi

if ! python3 - "$require_multigroup" <<'PY'
import os
import sys
cpus = os.sched_getaffinity(0)
groups = sorted({cpu // 64 for cpu in cpus})
print(f"Host CPU allowed: {len(cpus)}; 64-CPU groups exposed: {groups}")
if len(groups) < 2:
    print("HOST_LIMITED: cannot physically verify cross-group affinity/inheritance")
    sys.exit(3 if sys.argv[1] == "1" else 0)
print("PASS host exposes multiple physical CPU groups for separate runtime tests")
PY
then
    echo "INCOMPLETE: real multicgroup hardware/CPU allowance missing" >&2
    exit 3
fi
echo "PASS mandatory Fedora PE imports, isolated child and FluidSynth ABI"
echo "NOTE: a four-CPU host cannot validate multicgroup scheduling; do not report it as PASS."
