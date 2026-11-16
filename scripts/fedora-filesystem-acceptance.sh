#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Execute MANUALLY on physical Fedora x86_64; CI is not physical approval.
set -euo pipefail

if [[ $# -ne 1 ]] || [[ "$1" != "--physical-confirmed" ]]; then
    printf 'Usage: bash scripts/fedora-filesystem-acceptance.sh --physical-confirmed\n'
    printf 'Pass the flag only after verifying this is your physical Fedora workstation.\n'
    exit 2
fi
if [[ ! -r /etc/os-release ]]; then
    printf 'FAIL: cannot determine distribution\n' >&2
    exit 2
fi
# shellcheck disable=SC1091
source /etc/os-release
if [[ "$ID" != "fedora" || "$(uname -m)" != "x86_64" ]]; then
    printf 'FAIL: Fedora x86_64 required\n' >&2
    exit 2
fi
report_dir="$HOME/.local/state/nt-native-linux/fedora-acceptance"
mkdir -p "$report_dir"
stamp="$(date -u +%Y%m%dT%H%M%SZ)"
report="$report_dir/filesystem3-$stamp.log"
incomplete=0
{
    printf 'Filesystem3 native physical Fedora acceptance (AUTOMATED PART)\n'
    printf 'Fedora %s, kernel %s, architecture %s\n' "$VERSION_ID" "$(uname -r)" "$(uname -m)"
    printf 'Home filesystem type: %s\n' "$(stat -f -c %T "$HOME")"
    if command -v getenforce >/dev/null 2>&1; then
        printf 'SELinux mode: %s\n' "$(getenforce)"
    else
        printf 'SELinux mode: unavailable (manual review required)\n'
    fi
    printf 'Commit: '
    git rev-parse HEAD || printf 'unknown\n'
} | tee "$report"

if ! make -j2 check check-pe 2>&1 | tee -a "$report"; then
    printf 'FAIL: native or PE regression\n' | tee -a "$report"
    exit 1
fi
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    if ! make -j2 check-pe-import 2>&1 | tee -a "$report"; then
        printf 'FAIL: external PE regression\n' | tee -a "$report"
        exit 1
    fi
else
    printf 'INCOMPLETE: MinGW-w64 not installed; PE imports unverified\n' | tee -a "$report"
    incomplete=1
fi
if command -v bwrap >/dev/null 2>&1; then
    if ! make check-pe-isolated 2>&1 | tee -a "$report"; then
        printf 'FAIL: sandbox PE regression\n' | tee -a "$report"
        exit 1
    fi
    if grep -Eiq '(^|[[:space:]])SKIP([[:space:]:]|$)' "$report"; then
        printf 'INCOMPLETE: at least one test reported SKIP (see report)\n' | tee -a "$report"
        incomplete=1
    fi
else
    printf 'INCOMPLETE: Bubblewrap missing; PE isolation unverified\n' | tee -a "$report"
    incomplete=1
fi
{
    printf '\nAutomated checks completed; manual physical approval is NOT implied.\n'
    printf 'Also test real PE apps, D:/UNC grants, Unicode, aliases after reboot,\n'
    printf 'hostile symlinks, multiple processes, crash recovery and SELinux denial.\n'
    printf 'Review and record results per docs/filesystem3-hardening.md.\n'
    printf 'Report: %s\n' "$report"
} | tee -a "$report"
if [[ "$incomplete" -ne 0 ]]; then
    printf 'INCOMPLETE: automated tests contain SKIPs or missing dependencies.\n' | tee -a "$report"
    exit 3
fi
printf 'PASS: automated physical-host tests only; manual acceptance NOT granted.\n' | tee -a "$report"
