#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Malformed per-entry short-name xattrs must never be treated as 'missing'.

A rename without a recoverable source alias would silently lose DOS identity.
The runtime must refuse before renameat2 and leave the content/inode intact.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from dos83_wal_recovery import entry_key


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Usage: dos83_wal_bad_source.py MEDIATOR")
    binary = str(Path(sys.argv[1]).resolve())
    bad = (
        b"BAD",
        b"SHORT~1.TXT\x00X",
        b"ABCDEFGHIJKLMN",
        b"LONG~1.TXT" + b"X" * 20,
    )
    for value in bad:
        with tempfile.TemporaryDirectory(prefix="fs4-corrupt-source-") as temp:
            root = Path(temp)
            (root / "from").mkdir()
            (root / "to").mkdir()
            source = root / "from" / "originalVeryLongIdentity.txt"
            target = root / "to" / "renamedVeryLongIdentity.txt"
            source.write_bytes(b"NEVER LOSE MY IDENTITY")
            before = source.stat()
            args = [binary, "--root", temp]
            initial = subprocess.run(args + [
                "--refresh", "C:/from/originalVeryLongIdentity.txt"],
                capture_output=True, timeout=10)
            assert initial.returncode == 0, initial.stderr
            key = entry_key(source.parent, source.name)
            assert b"~" in os.getxattr(source, key)
            os.setxattr(source, key, value)
            result = subprocess.run(args + [
                "--move", "C:/from/originalVeryLongIdentity.txt",
                "C:/to/renamedVeryLongIdentity.txt"],
                capture_output=True, timeout=10)
            assert result.returncode != 0, (value, result.stderr)
            assert not target.exists(), value
            assert source.read_bytes() == b"NEVER LOSE MY IDENTITY", value
            assert (before.st_dev, before.st_ino) == (
                source.stat().st_dev, source.stat().st_ino), value
            assert os.getxattr(source, key) == value, value
            wal = root / ":ntnative-file-leases" / "dos83-rename-v1.wal"
            assert wal.is_file() and wal.stat().st_size == 0
            print("PASS malformed source alias refuses rename:", repr(value))


if __name__ == "__main__":
    main()
