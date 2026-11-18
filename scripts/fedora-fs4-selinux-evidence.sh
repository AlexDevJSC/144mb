#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Read-only, administrator-owned SELinux evidence for FileSystem4 on Fedora.
# It never changes policy, labels, DAC, files in C:, or audit configuration.
set -euo pipefail

if [[ "${1:-}" != "--physical-confirmed" || $# != 1 ]]; then
    echo 'Usage: sudo bash scripts/fedora-fs4-selinux-evidence.sh --physical-confirmed' >&2
    exit 2
fi
if [[ "${CI:-}" == true || "${GITHUB_ACTIONS:-}" == true ]]; then
    echo 'REFUSED: physical operator-only SELinux inspection, never CI' >&2
    exit 3
fi
if (( EUID != 0 )); then
    echo 'INCOMPLETE: run this read-only audit as root to access AVC records' >&2
    exit 3
fi
if [[ ! -r /etc/os-release ]]; then
    echo 'INCOMPLETE: /etc/os-release unavailable' >&2
    exit 3
fi
source /etc/os-release
if [[ "${ID:-}" != fedora || "$(uname -m)" != x86_64 ]] ||
   ! command -v getenforce >/dev/null ||
   [[ "$(getenforce)" != Enforcing ]] ||
   [[ ! -r /sys/fs/selinux/enforce ]] ||
   [[ "$(cat /sys/fs/selinux/enforce)" != 1 ]]; then
    echo 'INCOMPLETE: actual Fedora x86-64 SELinux Enforcing required' >&2
    exit 3
fi

umask 077
directory=/var/log/ntnative-storage
install -d -m 0700 "$directory"
report="$(mktemp "$directory/fs4-selinux-XXXXXXXX.log")"
chmod 0600 "$report"
{
    echo 'FileSystem4 SELinux diagnostic EVIDENCE (not security acceptance)'
    printf 'Timestamp UTC: %s\n' "$(date -u +%FT%TZ)"
    printf 'Kernel: %s\n' "$(uname -r)"
    printf 'Fedora: %s\n' "${VERSION_ID:-unknown}"
    printf 'SELinux enforcement: %s\n' "$(getenforce)"
    printf 'SELinux kernel enforce flag: %s\n' "$(cat /sys/fs/selinux/enforce)"
    echo 'Report contains system audit records: review and redact before sharing.'
} >>"$report"

capture() {
    local name="$1"
    shift
    printf '\n=== %s ===\n' "$name" >>"$report"
    if ! timeout 45s "$@" >>"$report" 2>&1; then
        printf 'INCOMPLETE: command unavailable/failed; human review needed (%s)\n' "$name" >>"$report"
    fi
}

capture 'SELinux status and policy mode' sestatus
capture 'Service identity and systemd context' systemctl show ntnative-storage.service -p ActiveState -p MainPID -p User -p Group -p ExecMainStatus
capture 'Socket state' systemctl show ntnative-storage.socket -p ActiveState -p Listen
capture 'Filesystem labels (backing, view, socket)' ls -Zd /var/lib/ntnative-storage /var/lib/ntnative-storage/C /run/ntnative-storage /run/ntnative-storage/view /run/ntnative-storage/control.sock
capture 'File/parent ownership and permissions' stat -c '%n %U:%G %a %F' /var/lib/ntnative-storage/C /run/ntnative-storage/view /run/ntnative-storage/control.sock
capture 'Path context versus SELinux default policy' matchpathcon -V /var/lib/ntnative-storage/C /run/ntnative-storage/view /run/ntnative-storage/control.sock
capture 'FUSE kernel mount identity' findmnt -T /run/ntnative-storage/view -o TARGET,SOURCE,FSTYPE,OPTIONS
capture 'Audit subsystem state' auditctl -s
capture 'AVC and USER_AVC records for current day (may include other processes)' ausearch -m AVC,USER_AVC -ts today -i
capture 'SELinux process labels and relevant service processes' bash -c "ps -eZ -o label,pid,user,comm,args | grep -E '(^LABEL|ntnative[-_]storage|nt-dos83-fuse|nt-fs-supervisor)' || true"
capture 'FileSystem4 policy file-context rules' bash -c "semanage fcontext -l | grep -E 'ntnative|dos83' || true"

{
    echo
    echo 'MANUAL REVIEW REQUIRED: determine whether each relevant denial is'
    echo 'SELinux TE/MLS or DAC; correlate scontext, tcontext, tclass,'
    echo 'permissive=, requested permissions and service PID with test timestamps.'
    echo 'No AVC does NOT prove confinement or correct labels.'
    echo 'Run a separate second-UID negative test, PE/FUSE concurrency,'
    echo 'controlled crash/reboot and disposable-volume power-loss test.'
    echo 'This report is NOT a Fedora physical PASS and NOT NTFS parity.'
} >>"$report"
printf 'Evidence saved (root-only, mode 0600): %s\n' "$report"
printf 'MANUAL REVIEW PENDING: labels, AVC, policy enforcement, second UID and power cut.\n'
