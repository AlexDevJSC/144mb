#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Unresolved WAL denies ALL tested Win32 namespace mutations, not only rename."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile


def call(binary, root, mode, path):
    return subprocess.run([binary, "--root", str(root), mode, "C:/" + path],
                          capture_output=True, text=True, timeout=15)


def case(binary, marker):
    with tempfile.TemporaryDirectory(prefix="fs4-unresolved-wal-") as tmp:
        root = Path(tmp)
        old = root / "longExistingImmutableFile.txt"
        old.write_bytes(b"STAY-UNCHANGED")
        removable = root / "longExistingMustNotDelete.txt"
        removable.write_bytes(b"NO-DELETE")
        empty = root / "longEmptyMustNotRemove"
        empty.mkdir()
        registry = root / ":ntnative-file-leases"
        registry.mkdir(mode=0o700)
        journal = registry / "dos83-rename-v1.wal"
        journal.write_bytes(marker)
        journal.chmod(0o600)

        for op, name in (
            ("--create", "longNoNewFileHere.txt"),
            ("--mkdir", "longNoNewDirectoryHere"),
            ("--truncate", old.name),
            ("--delete", removable.name),
            ("--rmdir", empty.name),
            ("--refresh", old.name),
        ):
            result = call(binary, root, op, name)
            assert result.returncode != 0, (op, result.stderr)
            assert old.read_bytes() == b"STAY-UNCHANGED", op
            assert removable.read_bytes() == b"NO-DELETE", op
            assert empty.is_dir(), op
            assert not (root / "longNoNewFileHere.txt").exists(), op
            assert not (root / "longNoNewDirectoryHere").exists(), op

        recover = subprocess.run([binary, "--root", str(root), "--recover"],
                                 text=True, capture_output=True, timeout=15)
        assert recover.returncode != 0
        assert journal.read_bytes() == marker
        print("PASS unresolved/corrupt WAL blocks namespace:", marker[:8])


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Usage: dos83_pending_wal_admission.py MEDIATOR")
    binary = str(Path(sys.argv[1]).resolve())
    for marker in (b"N83WAL02/INCOMPLETE",
                   b"UNKNOWN-CORRUPT-WAL", b"\x00" * 170):
        case(binary, marker)


if __name__ == "__main__":
    main()
