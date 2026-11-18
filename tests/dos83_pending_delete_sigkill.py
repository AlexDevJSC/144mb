#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Durable deferred deletion: actual SIGKILL/ENOSPC-style fsync EIO.

Verify orphaned FILE_DELETE_PENDING gates can be found by startup scanning,
even when their physical pathname was already unlinked before process death.
Never delete a surviving hardlink outside the authorized root.
"""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

from dos83_wal_recovery import entry_key

PHASES = ("after-pending-xattr-cleanup", "after-pending-unlink",
          "after-pending-parent-fsync", "after-pending-clear")


def run(binary, root, *args, env=None, success=True):
    result = subprocess.run([binary, "--root", str(root), *args],
                            env=env, text=True, capture_output=True, timeout=15)
    if success and result.returncode:
        raise AssertionError((args, result.returncode, result.stderr))
    return result


def scenario(normal, crash, phase):
    with tempfile.TemporaryDirectory(prefix="fs4-pending-delete-crash-") as temp:
        base = Path(temp)
        root = base / "C"
        root.mkdir()
        file = root / "longDeletePendingVictimName.txt"
        outside = base / "other-physical-hardlink.bin"
        file.write_bytes(b"DATA MUST SURVIVE ON OTHER LINK")
        os.link(file, outside)
        identity = file.stat()
        dos = "C:/" + file.name
        run(normal, root, "--refresh", dos)
        key = entry_key(root, file.name)
        assert os.getxattr(outside, key).endswith(b"\0")

        fault = run(crash, root, "--deferred-delete", dos,
                    env=dict(os.environ, NT_DOS83_TEST_CRASH=phase),
                    success=False)
        assert fault.returncode == -signal.SIGKILL, (
            phase, fault.returncode, fault.stderr)
        registry = root / ":ntnative-file-leases" / (
            f"{identity.st_dev:x}-{identity.st_ino:x}")
        assert registry.is_file(), phase
        # Pending marker is durable until the namespace is committed.
        current = registry.read_bytes()
        assert len(current) >= 8200, (phase, len(current))
        expected_pending = phase != "after-pending-clear"
        assert int.from_bytes(current[8196:8200], "little") == (
            1 if expected_pending else 0), phase

        # Recovery MUST work without any open handle to the original path.
        run(normal, root, "--recover")
        run(normal, root, "--recover")
        assert not file.exists(), phase
        assert outside.read_bytes() == b"DATA MUST SURVIVE ON OTHER LINK", phase
        assert outside.stat().st_nlink == 1, phase
        assert key not in os.listxattr(outside), phase
        after = registry.read_bytes()
        assert int.from_bytes(after[8196:8200], "little") == 0, phase
        print("PASS pending-delete SIGKILL/startup scan/other hardlink:", phase)


def eio_scenario(normal, crash):
    with tempfile.TemporaryDirectory(prefix="fs4-pending-delete-eio-") as temp:
        root = Path(temp)
        a, b = root / "firstPendingVictim.txt", root / "secondPendingVictim.txt"
        a.write_bytes(b"first")
        b.write_bytes(b"second")
        result = run(
            crash, root, "--two-deferred-deletes", "C:/" + a.name,
            "C:/" + b.name,
            env=dict(os.environ, NT_DOS83_TEST_IO_ERROR="pending-parent-fsync"),
            success=False)
        assert result.returncode != 0, result.stderr
        assert "deferred delete 1: marked=1" in result.stderr, result.stderr
        assert "deferred delete 2: marked=0" in result.stderr, result.stderr
        assert not a.exists(), result.stderr
        assert b.read_bytes() == b"second", result.stderr
        run(normal, root, "--recover")
        run(normal, root, "--recover")
        assert not a.exists()
        assert run(normal, root, "--deferred-delete", "C:/" + b.name).returncode == 0
        assert not b.exists()
        print("PASS pending-delete fsync EIO blocks follow-up mutation; startup reap")


def main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: dos83_pending_delete_sigkill.py NORMAL FAULT")
    normal, crash = (str(Path(x).resolve()) for x in sys.argv[1:])
    for phase in PHASES:
        scenario(normal, crash, phase)
    eio_scenario(normal, crash)


if __name__ == "__main__":
    main()
