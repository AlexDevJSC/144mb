#!/bin/sh
# SPDX-License-Identifier: MIT
# Deterministic smoke: opted-in mediation plus a paired external rename.
set -eu
tool=$(realpath "$1")
root=$(mktemp -d /tmp/dos83-mediator.XXXXXX)
watcher=""
cleanup() {
    if [ -n "$watcher" ]; then
        kill "$watcher" 2>/dev/null || :
        wait "$watcher" 2>/dev/null || :
    fi
    rm -rf -- "$root"
}
trap cleanup EXIT HUP INT TERM
mkdir "$root/sub"
printf 'A' > "$root/longOriginalFile.txt"
"$tool" --root "$root" --refresh 'C:/longOriginalFile.txt'
"$tool" --root "$root" --move 'C:/longOriginalFile.txt' 'C:/sub/renamedLongFile.txt'
test -f "$root/sub/renamedLongFile.txt"
test ! -e "$root/longOriginalFile.txt"
"$tool" --root "$root" --scan
printf 'B' > "$root/hostOriginalName.txt"
"$tool" --root "$root" --refresh 'C:/hostOriginalName.txt'
"$tool" --root "$root" --watch >"$root/watch.log" 2>&1 &
watcher=$!
ready=0
i=0
while [ "$i" -lt 100 ]; do
    if grep -q '^READY nt-dos83-mediator' "$root/watch.log"; then
        ready=1
        break
    fi
    if ! kill -0 "$watcher" 2>/dev/null; then
        break
    fi
    sleep 0.05
    i=$((i + 1))
done
if [ "$ready" -ne 1 ]; then
    cat "$root/watch.log" >&2
    echo "FAIL mediator did not start" >&2
    exit 1
fi
mv "$root/hostOriginalName.txt" "$root/hostRenamedFile.txt"
reconciled=0
i=0
while [ "$i" -lt 100 ]; do
    if grep -q '^RECONCILED hostOriginalName.txt -> hostRenamedFile.txt' "$root/watch.log"; then
        reconciled=1
        break
    fi
    sleep 0.05
    i=$((i + 1))
done
if [ "$reconciled" -ne 1 ]; then
    cat "$root/watch.log" >&2
    echo "FAIL mediator did not reconcile external rename" >&2
    exit 1
fi
"$tool" --root "$root" --refresh 'C:/hostRenamedFile.txt'
kill "$watcher"
wait "$watcher"
watcher=""
# Explicit NT-compatible opt-in is rooted, refuses directories, symlinks and
# ../ escape paths. No operating-system-wide xattr trust is implied.
printf 'C' > "$root/explicitNTFile.dat"
"$tool" --root "$root" --mark 'C:/explicitNTFile.dat'
python3 - "$root/explicitNTFile.dat" <<'PY'
import os, sys
assert os.getxattr(sys.argv[1], "user.ntnative.fs_compat") == b"1"
PY
ln -s /etc/passwd "$root/hostile-nt-link"
if "$tool" --root "$root" --mark 'C:/hostile-nt-link' 2>/dev/null ||
   "$tool" --root "$root" --mark 'C:/sub' 2>/dev/null ||
   "$tool" --root "$root" --mark 'C:/../outside' 2>/dev/null; then
    echo "FAIL NT file marker bypassed its grant" >&2
    exit 1
fi
echo "PASS NT-compatible marker grant/symlink/directory/traversal denials"
echo "PASS DOS 8.3 mediated rename, scan, and eventual inotify reconciliation"
