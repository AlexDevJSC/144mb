#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Destructive synthetic matrix. Use ONLY with a disposable mounted FUSE view.

No fake PASS: the caller must verify the exact live fuse.nt-dos83 mount.
Exercising data, names, leases and edge cases in addition to C selftests.
"""
import concurrent.futures
import errno
import os
from pathlib import Path
import subprocess
import sys


def denied(action, expected):
    try:
        action()
    except OSError as exc:
        assert exc.errno in expected, (exc, expected)
    else:
        raise AssertionError(f"Operation unexpectedly succeeded: {action}")


def matrix(lower, view, mediator):
    lower, view = Path(lower), Path(view)
    assert os.path.ismount(view), "Not a mounted filesystem"
    assert os.statvfs(view).f_blocks >= 0

    # Private share records may exist in the backing tree; never let a
    # FUSE client read, enumerate or rewrite them.
    assert ":ntnative-file-leases" not in os.listdir(view)
    denied(lambda: os.stat(view / ":ntnative-file-leases"),
           {errno.EACCES, errno.ENOENT})

    # readdir must use cookies rather than endlessly restarting at offset 0.
    directory = view / "page"
    directory.mkdir()
    for i in range(257):
        (directory / f"long_Document_{i:04d}.data").write_bytes(bytes([i % 256]))
    entries = sorted(p.name for p in directory.iterdir())
    assert len(entries) == 257 and len(set(entries)) == 257
    for i, p in enumerate(sorted(directory.iterdir())):
        assert p.read_bytes() == bytes([i % 256])
    for p in directory.iterdir():
        p.unlink()
    directory.rmdir()

    # Basic metadata, durable sync and POSIX alias behavior.
    f = view / "linux-posix-case.data"
    with f.open("wb") as handle:
        handle.write(b"long-data")
        handle.flush()
        os.fsync(handle.fileno())
    os.chmod(f, 0o600)
    assert f.stat().st_mode & 0o777 == 0o600
    os.setxattr(f, "user.ntfs_test", b"ABC")
    assert os.getxattr(f, "user.ntfs_test") == b"ABC"
    os.removexattr(f, "user.ntfs_test")
    assert f.read_bytes() == b"long-data"
    alias = view / "linux-posix-link.data"
    os.link(f, alias)
    assert alias.stat().st_ino == f.stat().st_ino
    alias.unlink()

    # Trusted marker cannot be controlled through the FUSE view. It is set
    # through a DOS-granted operator command on the backing inode.
    subprocess.run([mediator, "--root", str(lower), "--mark",
                    "C:/linux-posix-case.data"], check=True)
    assert os.getxattr(lower / f.name, "user.ntnative.fs_compat") == b"1"
    denied(lambda: os.getxattr(f, "user.ntnative.fs_compat"), {errno.ENODATA})
    denied(lambda: os.setxattr(f, "user.ntnative.fs_compat", b"0"),
           {errno.EACCES, errno.EPERM})
    with f.open("r+b") as stream:
        assert stream.read(4) == b"long"
        stream.seek(0)
        stream.write(b"LONG")
    assert f.read_bytes() == b"LONG-data"

    # Malformed private metadata is fail-closed, not silently POSIX.
    os.setxattr(lower / f.name, "user.ntnative.fs_compat", b"invalid")
    denied(lambda: os.open(f, os.O_WRONLY | os.O_TRUNC),
           {errno.EINVAL})
    assert (lower / f.name).read_bytes() == b"LONG-data"
    os.setxattr(lower / f.name, "user.ntnative.fs_compat", b"1")
    f.unlink()

    # Case-sensitive Linux paths with DOS aliases, name collisions and
    # concurrent renames in one directory must not produce duplicate aliases.
    sources = [view / f"PendingLongIdentifier_{i:03d}.txt" for i in range(32)]
    target = view / "matrix-final"
    target.mkdir()
    for src in sources:
        src.write_bytes(b"x")
    def move(item):
        dst = target / ("Moved" + item.name)
        os.rename(item, dst)
        return dst
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        outputs = list(pool.map(move, sources))
    markers = []
    for item in outputs:
        xattrs = [os.getxattr(lower / "matrix-final" / item.name, x)
                  for x in os.listxattr(lower / "matrix-final" / item.name)
                  if x.startswith("user.ntnative.dos83.")]
        assert len(xattrs) == 1, (item, xattrs)
        markers.append(xattrs[0])
    assert len(set(markers)) == len(markers)
    for item in outputs:
        item.unlink()
    target.rmdir()
    print("PASS FileSystem synthetic matrix: paging, metadata, opt-in NT, corruption denial, hardlinks and 32 concurrent renames")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit("Usage: dos83_contract_matrix.py LOWER FUSE_VIEW MEDIATOR")
    matrix(*sys.argv[1:])
