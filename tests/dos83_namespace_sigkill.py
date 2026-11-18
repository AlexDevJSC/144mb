#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Win32 namespace durability: real SIGKILL of test-only mediator.

Confirm replay and inode/metadata invariants across create, truncate, mkdir,
rmdir and eager delete. This does not assert data rollback after truncate or
physical power interruption.
"""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

from dos83_wal_recovery import entry_key


def invoke(executable, root, operation, dos_path, phase=None):
    env = dict(os.environ)
    if phase:
        env["NT_DOS83_TEST_CRASH"] = phase
    return subprocess.run([executable, "--root", str(root), operation, dos_path],
                          env=env, text=True, capture_output=True, timeout=15)


def scenario(normal, fault, operation, phase):
    with tempfile.TemporaryDirectory(prefix="fs4-namespace-kill-") as temp:
        root = Path(temp) / "C"
        root.mkdir()
        leaf = "veryLongNamespaceEntryName.txt" if operation not in (
            "--mkdir", "--rmdir") else "veryLongNewFolderName"
        target = root / leaf
        survivor = Path(temp) / "survivingHardLink"
        initial_ino = None
        if operation in ("--truncate", "--rmdir", "--delete"):
            if operation == "--rmdir":
                target.mkdir()
            else:
                target.write_bytes(b"EXISTING-DATA")
                initial_ino = target.stat().st_ino
                if operation == "--delete":
                    os.link(target, survivor)
                    init = invoke(normal, root, "--refresh", "C:/" + leaf)
                    assert init.returncode == 0, init.stderr
        killed = invoke(fault, root, operation, "C:/" + leaf, phase)
        assert killed.returncode == -signal.SIGKILL, (
            operation, phase, killed.returncode, killed.stderr)
        for _ in range(2):
            recovered = subprocess.run([normal, "--root", str(root), "--recover"],
                                       text=True, capture_output=True, timeout=15)
            assert recovered.returncode == 0, (
                operation, phase, recovered.returncode, recovered.stderr)
        if operation in ("--create", "--truncate"):
            assert target.is_file(), (operation, phase)
            assert target.stat().st_size == 0, (operation, phase)
            if initial_ino is not None:
                assert target.stat().st_ino == initial_ino
            alias = invoke(normal, root, "--refresh", "C:/" + leaf)
            assert alias.returncode == 0, alias.stderr
            assert b"~" in os.getxattr(target, entry_key(root, leaf))
        elif operation == "--mkdir":
            assert target.is_dir(), phase
            assert invoke(normal, root, "--refresh", "C:/" + leaf).returncode == 0
            assert b"~" in os.getxattr(target, entry_key(root, leaf))
        elif operation in ("--rmdir", "--delete"):
            assert not target.exists(), (operation, phase)
            if operation == "--delete":
                assert survivor.read_bytes() == b"EXISTING-DATA"
                assert entry_key(root, leaf) not in os.listxattr(survivor)
                assert survivor.stat().st_nlink == 1
        print("PASS namespace SIGKILL+replay:", operation, phase)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: dos83_namespace_sigkill.py NORMAL TEST_FAULT")
    normal, fault = (str(Path(x).resolve()) for x in sys.argv[1:])
    matrix = (
        ("--create", ("after-file-create", "after-file-create-fsync",
                      "after-file-parent-fsync")),
        ("--truncate", ("after-file-truncate", "after-file-truncate-fsync")),
        ("--mkdir", ("after-mkdir", "after-mkdir-inode-fsync",
                     "after-mkdir-parent-fsync")),
        ("--rmdir", ("after-delete-entry", "after-delete-parent-fsync")),
        ("--delete", ("after-pending-xattr-cleanup", "after-pending-unlink",
                      "after-pending-parent-fsync", "after-pending-clear")),
    )
    for operation, phases in matrix:
        for phase in phases:
            scenario(normal, fault, operation, phase)


if __name__ == "__main__":
    main()
