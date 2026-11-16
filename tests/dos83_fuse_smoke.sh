#!/bin/sh
# SPDX-License-Identifier: MIT
# P2.3 tests use a disposable backing tree. A kernel FUSE mount is optional
# in CI, but this script never reports an absent mount as an integration PASS.
set -eu
daemon=$(realpath "$1")
physical_required=${NT_DOS83_REQUIRE_PHYSICAL:-0}
tmp=$(mktemp -d /tmp/nt-dos83-fuse.XXXXXX)
pid=""
cleanup() {
    if [ -n "$pid" ]; then
        if mountpoint -q "$tmp/view"; then
            fusermount3 -u "$tmp/view" >/dev/null 2>&1 || :
        fi
        kill "$pid" >/dev/null 2>&1 || :
        wait "$pid" >/dev/null 2>&1 || :
    fi
    rm -rf -- "$tmp"
}
trap cleanup EXIT HUP INT TERM
mkdir "$tmp/lower" "$tmp/view"
printf X > "$tmp/lower/longOriginalFilename.txt"
ln -s /etc/passwd "$tmp/lower/hostile-link"
"$daemon" --selftest "$tmp/lower"
mediator=$(dirname "$daemon")/nt-dos83-mediator
"$mediator" --root "$tmp/lower" --refresh 'C:/longOriginalFilename.txt'
landlock=$(dirname "$daemon")/nt-dos83-landlock
"$landlock" --probe || echo "SKIP P2.3 Landlock ABI validation (kernel restriction)"
# A random existing directory must never be accepted as a mediated FUSE root.
if "$landlock" --view "$tmp/view" -- /bin/true 2>/dev/null; then
    echo "FAIL P2.3 Landlock accepted an ordinary unmounted directory" >&2
    exit 1
fi
echo "PASS P2.3 fail-closed unmounted-view validation"
if [ ! -c /dev/fuse ] || ! command -v fusermount3 >/dev/null 2>&1; then
    echo "SKIP P2.3 mounted-FUSE integration (/dev/fuse/fusermount3 unavailable)"
    if [ "$physical_required" = 1 ]; then exit 3; fi
    exit 0
fi
"$daemon" "$tmp/lower" "$tmp/view" -f >"$tmp/fuse.log" 2>&1 &
pid=$!
ready=0
i=0
while [ "$i" -lt 120 ]; do
    if mountpoint -q "$tmp/view"; then
        ready=1
        break
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
        break
    fi
    sleep 0.05
    i=$((i + 1))
done
if [ "$ready" -ne 1 ]; then
    cat "$tmp/fuse.log" >&2
    echo "SKIP P2.3 mounted-FUSE integration (mount denied by environment)"
    if [ "$physical_required" = 1 ]; then exit 3; fi
    exit 0
fi
# A Linux application can mutate the FUSE view, but its operations execute
# through the broker first. The old DOS alias on the backing inode survives.
python3 - "$tmp/lower/longOriginalFilename.txt" "$tmp/view" <<'PY'
import os, sys
backing, view = sys.argv[1:]
def alias(path):
    values = [(n, os.getxattr(path,n)) for n in os.listxattr(path)
              if n.startswith("user.ntnative.dos83.")]
    assert len(values) == 1, values
    return values[0][1]
before = alias(backing)
os.rename(os.path.join(view, "longOriginalFilename.txt"),
          os.path.join(view, "hostRenamedFile.txt"))
assert not os.path.lexists(backing)
moved = os.path.join(os.path.dirname(backing), "hostRenamedFile.txt")
assert alias(moved) == before
with open(os.path.join(view, "nativeCreatedFile.txt"), "wb") as f:
    f.write(b"native")
assert alias(os.path.join(os.path.dirname(backing), "nativeCreatedFile.txt"))
try:
    with open(os.path.join(view, "hostile-link"), "rb") as f:
        f.read()
except OSError:
    pass
else:
    raise AssertionError("hostile symlink was followed")
try:
    os.setxattr(os.path.join(view, "hostRenamedFile.txt"),
                "user.ntnative.dos83.injected", b"BAD")
except OSError:
    pass
else:
    raise AssertionError("guest modified private alias xattr")
# Real concurrent Linux processes rename distinct files into a common
# directory. The broker must allocate unique aliases without double claims.
import concurrent.futures, subprocess
os.mkdir(os.path.join(view, "parallel"))
def move_one(i):
    old = os.path.join(view, f"ConcurrentLongFileName{i:02d}.txt")
    new = os.path.join(view, "parallel", f"FinalLongFileName{i:02d}.txt")
    with open(old, "wb") as stream:
        stream.write(bytes([65 + i]))
    subprocess.run(["/bin/mv", old, new], check=True)
    return new
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
    outputs = list(pool.map(move_one, range(12)))
aliases = [alias(os.path.join(os.path.dirname(backing), "parallel",
                              os.path.basename(path))) for path in outputs]
assert len(set(aliases)) == len(aliases), aliases
print("PASS P2.3 real FUSE rename, create, xattr isolation, symlink denial and concurrent Linux writers")
PY
# Stronger perimeter: even a Linux program that knows the lower path
# must be unable to rename, create or truncate it after the Landlock policy.
if "$landlock" --probe >/dev/null 2>&1; then
    "$landlock" --view "$tmp/view" -- /bin/sh -c '
        set -eu
        printf "allowed" > "$1/landlock-created.txt"
        if (printf "FORBIDDEN" > "$2/direct-bypass.txt") 2>/dev/null; then
            exit 91
        fi
        if (printf "TRUNCATE" > "$2/hostRenamedFile.txt") 2>/dev/null; then
            exit 92
        fi
        if (mv "$2/hostRenamedFile.txt" "$2/direct-rename.txt") 2>/dev/null; then
            exit 93
        fi
        if (rm "$2/hostRenamedFile.txt") 2>/dev/null; then
            exit 94
        fi
        if (ln "$2/hostRenamedFile.txt" "$1/hardlink-from-lower.txt") 2>/dev/null; then
            exit 95
        fi
    ' sh "$tmp/view" "$tmp/lower"
    test -f "$tmp/lower/landlock-created.txt"
    test ! -e "$tmp/lower/direct-bypass.txt"
    test ! -e "$tmp/lower/direct-rename.txt"
    test ! -e "$tmp/lower/hardlink-from-lower.txt"
    test "$(wc -c < "$tmp/lower/hostRenamedFile.txt")" -eq 1
    : >"$tmp/lower/inherited-fd.txt"
    if (
        exec 3>>"$tmp/lower/inherited-fd.txt"
        "$landlock" --view "$tmp/view" -- /bin/sh -c 'printf BYPASS >&3'
    ) 2>/dev/null; then
        echo "FAIL P2.3 inherited descriptor bypass" >&2
        exit 1
    fi
    test ! -s "$tmp/lower/inherited-fd.txt"
    echo "PASS P2.3 Landlock denies backing writes/truncate/rename/unlink/hardlink and closes inherited FDs"
else
    echo "SKIP P2.3 strong Landlock isolation (kernel Landlock ABI < 3)"
    if [ "$physical_required" = 1 ]; then exit 3; fi
fi
# Bubblewrap can additionally give read-only mounting, but CI may disable
# user namespaces. A failure to initialize bwrap is never a security PASS.
if command -v bwrap >/dev/null 2>&1; then
    if bash scripts/dos83-fuse-confined.sh \
        --lower "$tmp/lower" --view "$tmp/view" -- \
        /bin/sh -c 'printf BWRAP > "$1/probe-bwrap.txt"' sh "$tmp/view" 2>"$tmp/bwrap.log"; then
        test -f "$tmp/lower/probe-bwrap.txt"
        if bash scripts/dos83-fuse-confined.sh \
            --lower "$tmp/lower" --view "$tmp/view" -- \
            /bin/sh -c 'printf BAD > "$1/bypass-bwrap.txt"' sh "$tmp/lower"; then
            echo "FAIL P2.3 bwrap direct lower bypass" >&2
            exit 1
        fi
        test ! -e "$tmp/lower/bypass-bwrap.txt"
        echo "PASS P2.3 Bubblewrap writable-view confinement"
    else
        echo "SKIP P2.3 Bubblewrap confinement (user/mount namespaces denied)"
    fi
fi
