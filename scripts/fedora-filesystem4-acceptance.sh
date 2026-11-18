#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# FileSystem4 operator-owned physical Fedora acceptance. Never fake NTFS parity.
set -euo pipefail

usage() {
    echo 'Usage: bash scripts/fedora-filesystem4-acceptance.sh --physical-confirmed [--windows-reference FILE.json]' >&2
}
physical=0
reference=""
while (($#)); do
    case "$1" in
        --physical-confirmed) physical=1; shift ;;
        --windows-reference)
            if (($# < 2)); then usage; exit 2; fi
            reference=$2; shift 2 ;;
        *) usage; exit 2 ;;
    esac
done
if ((physical != 1)); then usage; exit 2; fi
if [[ "$(printenv GITHUB_ACTIONS || true)" == true ||
      "$(printenv CI || true)" == true ]]; then
    echo 'REFUSED: physical owner acceptance, not CI' >&2
    exit 3
fi
if ((EUID == 0)); then
    echo 'REFUSED: do not run FileSystem4 acceptance as root' >&2
    exit 3
fi
if [[ ! -r /etc/os-release ]]; then
    echo 'REFUSED: cannot detect host OS' >&2
    exit 3
fi
source /etc/os-release
if [[ "$ID" != fedora || "$(uname -m)" != x86_64 ]]; then
    echo 'REFUSED: physical Fedora x86-64 required' >&2
    exit 3
fi
if ! command -v getenforce >/dev/null ||
   [[ "$(getenforce)" != Enforcing ]]; then
    echo 'INCOMPLETE: SELinux Enforcing required; do not disable SELinux' >&2
    exit 3
fi
if [[ ! -c /dev/fuse ]] || ! command -v fusermount3 >/dev/null ||
   ! command -v bwrap >/dev/null; then
    echo 'INCOMPLETE: /dev/fuse, fusermount3 and bubblewrap are required' >&2
    exit 3
fi
if [[ -n "$reference" && ! -r "$reference" ]]; then
    echo 'INCOMPLETE: missing Windows reference' >&2
    exit 3
fi
if [[ -n "$reference" ]]; then
    python3 "$(dirname "$0")/fs4_windows_reference_validate.py" "$reference" || exit 3
fi
repo="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo"
if ! git rev-parse --verify HEAD >/dev/null 2>&1; then
    echo 'REFUSED: code revision cannot be identified' >&2
    exit 3
fi
if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
    echo 'INCOMPLETE: tracked changes, use a reproducible commit' >&2
    exit 3
fi

umask 077
state_home="$(printenv XDG_STATE_HOME || true)"
if [[ -z "$state_home" ]]; then state_home="$HOME/.local/state"; fi
report_dir="$state_home/nt-native-linux/fedora-acceptance"
mkdir -p "$report_dir"
stamp="$(date -u +%Y%m%dT%H%M%SZ)"
report="$report_dir/filesystem4-$stamp.log"
scratch="$(mktemp)"
trap 'rm -f -- "$scratch"' EXIT

{
    echo 'FileSystem4 AUTOMATED physical Fedora acceptance; not NTFS parity'
    printf 'Commit: %s\n' "$(git rev-parse HEAD)"
    printf 'Kernel: %s\n' "$(uname -r)"
    printf 'Fedora: %s\n' "$VERSION_ID"
    printf 'SELinux: %s\n' "$(getenforce)"
    printf 'HOME filesystem: %s\n' "$(stat -f -c %T "$HOME")"
    if [[ -n "$reference" ]]; then
        printf 'Windows reference SHA256: %s\n' "$(sha256sum "$reference")"
        echo 'Windows reference presence does NOT prove comparison parity'
    else
        echo 'Windows comparison evidence: missing'
    fi
} | tee "$report"

check() {
    title="$1"; shift
    printf '\n=== %s ===\n' "$title" | tee -a "$report"
    set +e
    "$@" > "$scratch" 2>&1
    code=$?
    set -e
    cat "$scratch" | tee -a "$report"
    if ((code != 0)); then
        printf 'INCOMPLETE: %s exited %d\n' "$title" "$code" | tee -a "$report"
        exit 3
    fi
}
check 'Filesystem regressions with WAL, PE and FUSE' \
    make -j2 all check check-pe check-dos83-wal check-dos83-fuse \
         check-dos83-mediator check-fs-supervisor check-fs-backing-audit
check 'Mandatory mounted FUSE+Landlock under SELinux Enforcing' \
    bash scripts/fedora-p2-3-acceptance.sh
check 'Dedicated UID service + mounted FUSE + v3 HANDLE IPC under Enforcing' \
    python3 scripts/fedora-fs4-service-acceptance.py --physical-confirmed
check 'FileSystem3 physical compatibility and PE isolation gate' \
    bash scripts/fedora-filesystem-acceptance.sh --physical-confirmed

# Integrated end-to-end smoke on disposable C:, never personal saved data:
# a REAL PE guest executes under Bubblewrap while supervisor mounts FUSE.
test_state="$(mktemp -d /tmp/fs4-pe-state.XXXXXX)"
test_view="$(mktemp -d /tmp/fs4-pe-view.XXXXXX)"
cleanup_pe_fixture() {
    if mountpoint -q "$test_view"; then
        echo "CRITICAL: FUSE view still mounted; manual cleanup: $test_view" >&2
        return 0
    fi
    rm -rf -- "$test_state" "$test_view"
}
trap 'cleanup_pe_fixture; rm -f -- "$scratch"' EXIT
mkdir -p "$test_state/nt-native-linux/C:"
chmod 0700 "$test_state/nt-native-linux" "$test_state/nt-native-linux/C:"
check 'Integrated supervised PE/FUSE on disposable C: profile' \
    env XDG_STATE_HOME="$test_state" \
    python3 scripts/nt-fs-supervisor.py \
      --lower "$test_state/nt-native-linux/C:" \
      --view "$test_view" --pe -- "$repo/build/nt-counter.exe"

# These are the runtime's Win32 W entrypoints, not host POSIX operations.
# Save observations before comparing with a real Windows NTFS reference.
nt144_report="$report_dir/nt144-reference-$stamp.json"
if ! "$repo/build/fs4-ntfs-probe" --root \
       "$test_state/nt-native-linux/C:" > "$nt144_report"; then
    echo 'INCOMPLETE: NT144 Win32 probe failed (inspect disposable state)' | tee -a "$report"
    exit 3
fi
echo "NT144 probe: $nt144_report" | tee -a "$report"
if [[ -n "$reference" ]]; then
    diff_report="$report_dir/ntfs-comparison-$stamp.json"
    set +e
    python3 scripts/fs4_ntfs_diff.py "$reference" "$nt144_report" > "$diff_report"
    diff_exit=$?
    set -e
    cat "$diff_report" | tee -a "$report"
    if ((diff_exit != 0)); then
        echo 'INCOMPLETE: Windows comparison has differences or unimplemented APIs' | tee -a "$report"
        exit 3
    fi
fi

# Do not count a critical isolation SKIP as success. Unrelated hardware
# groups/CPU affinity SKIPs are recorded but not a filesystem failure.
if grep -Eiq 'SKIP.*(FUSE|Landlock|Bubblewrap|sandbox|isolated|large junction xattr)' "$report"; then
    echo 'INCOMPLETE: critical FUSE/PE/junction test skipped' | tee -a "$report"
    exit 3
fi
{
    echo 'PASS: automated Fedora filesystem regression only.'
    echo 'MANUAL PENDING: SIGKILL and power-cycle recovery, another UID denial,'
    echo 'SELinux AVC review, concurrent FUSE/PE operations, Windows comparison.'
    echo 'No universal service UID isolation or 100% NTFS claim.'
    printf 'Evidence: %s\n' "$report"
} | tee -a "$report"
if [[ -z "$reference" ]]; then
    echo 'INCOMPLETE: Windows NTFS reference not supplied' | tee -a "$report"
    exit 3
fi
# Exit zero is AUTOMATED part only. Human must review Windows diff.
exit 0
