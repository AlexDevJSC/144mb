#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify D: WAL preflight across a *different* physical root from C:.

The host diagnostic accepts a second explicitly granted directory. It must
recover a legitimate pending D: transaction and refuse a corrupt D: WAL,
without treating a clean C: as sufficient. No actual NTFS is claimed.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from dos83_wal_recovery import entry_key, make_record


def call(tool, c_root, *args, expected=0):
    result = subprocess.run([tool, "--root", str(c_root), *map(str, args)],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, timeout=15)
    if result.returncode != expected:
        raise AssertionError((args, expected, result.returncode, result.stderr))
    return result


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Usage: dos83_wal_all_grants.py MEDIATOR")
    tool = str(Path(sys.argv[1]).resolve())

    for corrupted in (False, True):
        with tempfile.TemporaryDirectory(prefix="nt-wal-multigrant-") as tmp:
            root = Path(tmp)
            c_root = root / "c"
            d_root = root / "d"
            c_root.mkdir()
            d_root.mkdir()
            left = d_root / "left"
            right = d_root / "right"
            left.mkdir()
            right.mkdir()
            source = left / "longNameForOtherGrant.txt"
            destination = right / "longNameAfterGrantRename.txt"
            source.write_bytes(b"D physical root must be recovered independently")
            physical_inode = source.stat()

            call(tool, d_root, "--refresh", "C:/left/" + source.name)
            alias = os.getxattr(source, entry_key(left, source.name))
            dest_key = entry_key(right, destination.name)
            record = make_record(d_root, source, destination, alias)
            registry = d_root / ":ntnative-file-leases"
            registry.mkdir(mode=0o700, exist_ok=True)
            wal = registry / "dos83-rename-v1.wal"
            if corrupted:
                record[16] ^= 0x57  # checksum now invalid; must fail closed
            wal.write_bytes(record)
            os.chmod(wal, 0o600)
            os.setxattr(source, dest_key, alias)

            result = call(tool, c_root, "--recover-with-drive", d_root,
                          expected=3 if corrupted else 0)
            if corrupted:
                assert wal.stat().st_size > 0
                assert source.read_bytes().startswith(b"D physical root")
                assert os.getxattr(source, dest_key) == alias
                print("PASS reject corrupt WAL on D: even if C: clean")
                continue

            assert wal.stat().st_size == 0
            assert source.exists() and not destination.exists()
            assert dest_key not in os.listxattr(source)
            assert (source.stat().st_dev, source.stat().st_ino) == (
                physical_inode.st_dev, physical_inode.st_ino)
            assert result.returncode == 0
            call(tool, c_root, "--recover-with-drive", d_root)
            print("PASS D: grant preflight recovered independent pending WAL")


if __name__ == "__main__":
    main()
