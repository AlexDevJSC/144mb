#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Hardware acceptance gate for the NT/Win32 stage-4 process/thread profile.
# Run from a clean checkout on an AMD64 Fedora workstation.
set -Eeuo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

if [[ $(uname -m) != x86_64 ]]; then
    echo "FAIL: this runtime only supports x86_64 guest PE32+." >&2
    exit 2
fi
if ! grep -Eq '^ID=fedora$|^ID="fedora"$' /etc/os-release; then
    echo "FAIL: final acceptance requires a native Fedora host, not an Ubuntu container." >&2
    exit 2
fi

for tool in make cc git getenforce systemd-detect-virt x86_64-w64-mingw32-gcc timeout bwrap; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "INCOMPLETE: missing required physical-QA tool: $tool" >&2
        exit 3
    fi
done
if [[ $(getenforce) != Enforcing ]]; then
    echo "INCOMPLETE: Fedora physical acceptance requires SELinux Enforcing." >&2
    exit 3
fi
if systemd-detect-virt --quiet; then
    echo "INCOMPLETE: virtualization/container detected; physical acceptance not demonstrated." >&2
    exit 3
fi
printf 'STAGE4 SHA=%s KERNEL=%s SELINUX=%s\n' \
  "$(git rev-parse HEAD)" "$(uname -r)" "$(getenforce)"
printf 'NOTE: physical-host detection is a preflight, not proof of Windows parity.\n'

log=$(mktemp)
cleanup() { rm -f -- "$log"; }
trap cleanup EXIT

echo "[1/4] Linux runtime, real pthreads and 48-cycle lifetime tests"
make -j2 check
echo "[2/4] Native Windows PE/AMD64 thread, APC, TLS and process contracts"
SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 \
  make -j2 check-pe-core-objects check-pe-apc check-pe-async-special-apc check-pe-async-special-apc-seh check-pe-hardware-seh \
           check-pe-suspended-thread check-pe-thread-groups check-pe-priority-class check-pe-priority-boost check-pe-process-affinity \
           check-pe-native-core check-pe-teb-tls check-pe-nt-io

echo "[3/4] Undefined-behavior sanitizer: process/thread and PE tests"
SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 \
  make -j2 BUILD_DIR=build-stage4-ubsan \
       CFLAGS='-O1 -g -fsanitize=undefined -fno-sanitize-recover=all' \
       check check-pe-core-objects check-pe-apc check-pe-async-special-apc check-pe-async-special-apc-seh check-pe-hardware-seh \
       check-pe-suspended-thread check-pe-thread-groups check-pe-priority-class check-pe-priority-boost check-pe-process-affinity \
       check-pe-native-core check-pe-teb-tls

echo "[4/4] Real Bubblewrap-isolated PE and child-process validation (no SKIP accepted)"
if ! SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 \
     make check-pe-isolated 2>&1 | tee "$log"; then
    echo "FAIL: isolated Win64 child-process/loader tests failed." >&2
    exit 1
fi
if grep -q 'SKIP external-PE sandbox test' "$log"; then
    echo "INCOMPLETE: Bubblewrap/namespace policy skipped the isolated PE suite." >&2
    echo "This is not a full Fedora acceptance result. Do not bypass sandboxing." >&2
    exit 3
fi
if ! grep -q 'PASS PE child launched inside the same Linux sandbox' "$log"; then
    echo "INCOMPLETE: there is no observed child-process execution success." >&2
    exit 3
fi

echo "PASS: implemented stage-4 runtime/PE/UBSan and isolated child tests ran with SELinux Enforcing on this nonvirtualized Fedora host."
echo "LIMITATION: this does NOT establish 100% Windows NT process/thread compatibility, privileged priority transitions or real multicpu-group scheduling."
echo "Remaining contracts are listed in docs/nt-stage4-process-threads.md."
