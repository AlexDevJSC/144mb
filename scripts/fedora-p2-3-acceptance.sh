#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# P2.3 physical Fedora gate: FUSE + Landlock must really run (no SKIP).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
if [[ ! -r /etc/os-release ]]; then
    echo "FAIL: cannot detect Fedora" >&2
    exit 2
fi
source /etc/os-release
if [[ "${ID:-}" != fedora ]]; then
    echo "FAIL: Fedora physical host required; detected ${ID:-unknown}" >&2
    exit 2
fi
if [[ ! -c /dev/fuse ]] || ! command -v fusermount3 >/dev/null; then
    echo "INCOMPLETE: FUSE character device or fusermount3 missing" >&2
    exit 3
fi
if ! command -v getenforce >/dev/null; then
    echo "INCOMPLETE: getenforce missing; SELinux state unverified" >&2
    exit 3
fi
if [[ "$(getenforce)" != Enforcing ]]; then
    echo "INCOMPLETE: SELinux must be Enforcing for this acceptance gate" >&2
    exit 3
fi
report_dir="${XDG_STATE_HOME:-$HOME/.local/state}/nt-native-linux/fedora-acceptance"
mkdir -p "$report_dir"
report="$report_dir/p2-3-$(date +%Y%m%dT%H%M%S).log"
{
    printf 'P2.3 physical Fedora FUSE+Landlock acceptance\n'
    printf 'Commit: %s\n' "$(git rev-parse HEAD)"
    printf 'Kernel: %s\n' "$(uname -r)"
    printf 'Fedora: %s\n' "$VERSION_ID"
    printf 'SELinux: %s\n' "$(getenforce)"
    printf 'Filesystem HOME: %s\n' "$(stat -f -c %T "$HOME")"
    printf 'Physical host: operator must verify; no automatic hardware claim\n'
} | tee "$report"
set +e
NT_DOS83_REQUIRE_PHYSICAL=1 make -j2 check-dos83-fuse 2>&1 | tee -a "$report"
code=${PIPESTATUS[0]}
set -e
if [[ "$code" != 0 ]]; then
    printf 'INCOMPLETE: P2.3 physical tests failed or skipped (exit %s)\n' "$code" | tee -a "$report"
    exit 3
fi
printf 'PASS automated physical-host FUSE+Landlock tests; manual review still required\n' | tee -a "$report"
printf 'Report: %s\n' "$report"
