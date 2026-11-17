#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual SIGKILL of a TEST-ONLY mediator at four syscall boundaries.
This proves process-crash replay, not disk-cache persistence on power loss.
The normal mediator performs replay and must not contain fault hooks.
"""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

from dos83_wal_recovery import entry_key

PHASES = ("after-wal", "after-staging", "after-rename", "after-cleanup",
          "after-source-parent-fsync", "after-both-parents-fsync",
          "after-wal-clear")
CONTENT = b"DOS83 WAL original inode and bytes preserved by SIGKILL fixture"


def run(binary, root, *args, env=None, success=True):
    result = subprocess.run([binary, "--root", str(root), *args],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, env=env, timeout=15)
    if success and result.returncode != 0:
        raise AssertionError((args, result.returncode, result.stderr))
    return result


def scenario(clean_mediator, fault_mediator, phase):
    with tempfile.TemporaryDirectory(prefix="nt-fs4-real-sigkill-") as temp:
        root = Path(temp)
        before, after = root / "before", root / "after"
        before.mkdir()
        after.mkdir()
        source = before / "longOriginalIdentityForRealCrash.txt"
        target = after / "longRenamedIdentityForRealCrash.txt"
        source.write_bytes(CONTENT)
        src_identity = source.stat()
        from_dos = "C:/before/" + source.name
        to_dos = "C:/after/" + target.name

        run(clean_mediator, root, "--refresh", from_dos)
        from_key = entry_key(before, source.name)
        to_key = entry_key(after, target.name)
        alias = os.getxattr(source, from_key)

        env = dict(os.environ, NT_DOS83_TEST_CRASH=phase)
        killed = run(fault_mediator, root, "--move",
                     from_dos, to_dos, env=env, success=False)
        assert killed.returncode == -signal.SIGKILL, (
            phase, killed.returncode, killed.stderr
        )
        wal = root / ":ntnative-file-leases" / "dos83-rename-v1.wal"
        expect_pending = phase != "after-wal-clear"
        assert wal.is_file()
        assert (wal.stat().st_size > 0) == expect_pending, phase

        # The recovery binary is the *production* mediator. A second process
        # must resolve the transaction using only durable on-disk evidence.
        run(clean_mediator, root, "--recover")
        run(clean_mediator, root, "--recover")  # idempotent
        assert wal.stat().st_size == 0, phase

        renamed = phase not in ("after-wal", "after-staging")
        current = target if renamed else source
        missing = source if renamed else target
        assert current.read_bytes() == CONTENT, phase
        assert not missing.exists(), phase
        stat = current.stat()
        assert (stat.st_dev, stat.st_ino) == (
            src_identity.st_dev, src_identity.st_ino), phase

        if renamed:
            assert os.getxattr(current, to_key) == alias, phase
            assert from_key not in os.listxattr(current), phase
            run(clean_mediator, root, "--move", to_dos, from_dos)
            assert source.read_bytes() == CONTENT, phase
        else:
            assert os.getxattr(current, from_key) == alias, phase
            assert to_key not in os.listxattr(current), phase
            run(clean_mediator, root, "--move", from_dos, to_dos)
            assert target.read_bytes() == CONTENT, phase
        print("PASS actual SIGKILL + production recovery:", phase)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: dos83_wal_sigkill.py NORMAL_MEDIATOR FAULT_MEDIATOR")
    regular, injected = (str(Path(x).resolve()) for x in sys.argv[1:])
    for phase in PHASES:
        scenario(regular, injected, phase)


if __name__ == "__main__":
    main()
