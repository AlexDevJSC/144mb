#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Run a Linux program with its writable view restricted to the mediated FUSE
# mount. The physical backing tree is visible READ-ONLY, never as a writable
# alias. This is an opt-in guarantee for this child process and descendants.
set -euo pipefail

usage() {
    echo "Usage: $0 --lower BACKING --view MOUNTPOINT -- COMMAND [ARGS...]" >&2
    exit 2
}
[[ $# -ge 6 && "$1" == --lower && "$3" == --view && "$5" == -- ]] || usage
lower=$(realpath -e -- "$2")
view=$(realpath -e -- "$4")
shift 5
[[ $# -gt 0 ]] || usage
[[ -d "$lower" && -d "$view" ]] || { echo "Both paths must be directories" >&2; exit 2; }
if [[ "$lower" == "$view" || "$view/" == "$lower/"* || "$lower/" == "$view/"* ]]; then
    echo "Backing and view must be disjoint" >&2
    exit 2
fi
command -v findmnt >/dev/null || { echo "Missing findmnt" >&2; exit 2; }
command -v bwrap >/dev/null || { echo "Missing bubblewrap" >&2; exit 2; }
mountpoint -q -- "$view" || { echo "Refusing unmounted view" >&2; exit 3; }
type=$(findmnt -n -M "$view" -o FSTYPE || :)
[[ "$type" == "fuse.nt-dos83" ]] || {
    echo "Refusing unverified mount type: $type" >&2
    exit 3
}
# No writable host hierarchy or --dev-bind of host /dev is exposed. A new
# bind of the FUSE mount is the ONLY writable copy of the granted tree.
exec bwrap --unshare-user --unshare-pid --unshare-ipc --unshare-uts \
    --new-session --die-with-parent \
    --ro-bind / / \
    --proc /proc \
    --dev /dev \
    --bind "$view" "$view" \
    --chdir "$view" \
    -- "$@"
