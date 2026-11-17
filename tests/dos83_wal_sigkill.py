#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual SIGKILL of a TEST-ONLY mediator at durable syscall boundaries.
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


def replacement_hardlink_scenario(clean_mediator, fault_mediator, phase):
    """Overwritten victim with surviving hardlink OUTSIDE the grant.

    A stale destination-entry xattr on the surviving hardlink was formerly
    irrecoverable: once rename committed, victim's destination path vanished.
    v2 pre-cleans and syncs the victim before its last original name is lost.
    """
    with tempfile.TemporaryDirectory(prefix="nt-fs4-victim-hardlink-") as temp:
        base = Path(temp)
        root = base / "C"
        root.mkdir()
        before, after = root / "before", root / "after"
        before.mkdir()
        after.mkdir()
        external = base / "surviving-link-outside-grant.dat"
        source = before / "longOriginalReplacementFile.txt"
        victim = after / "longVictimReplacementFile.txt"
        source.write_bytes(b"SOURCE-UNIQUE-PAYLOAD")
        victim.write_bytes(b"VICTIM-SURVIVING-HARDLINK")
        os.link(victim, external)
        original_victim_stat = victim.stat()
        original_source_stat = source.stat()
        from_dos = "C:/before/" + source.name
        to_dos = "C:/after/" + victim.name
        run(clean_mediator, root, "--refresh", from_dos)
        run(clean_mediator, root, "--refresh", to_dos)
        source_key = entry_key(before, source.name)
        dest_key = entry_key(after, victim.name)
        src_alias = os.getxattr(source, source_key)
        victim_alias = os.getxattr(victim, dest_key)
        assert os.getxattr(external, dest_key) == victim_alias

        env = dict(os.environ, NT_DOS83_TEST_CRASH=phase)
        killed = run(fault_mediator, root, "--replace",
                     from_dos, to_dos, env=env, success=False)
        assert killed.returncode == -signal.SIGKILL, (
            "victim", phase, killed.returncode, killed.stderr
        )
        journal = root / ":ntnative-file-leases" / "dos83-rename-v1.wal"
        assert journal.is_file()
        assert (journal.stat().st_size > 0) == (phase != "after-wal-clear")

        run(clean_mediator, root, "--recover")
        run(clean_mediator, root, "--recover")
        assert journal.stat().st_size == 0

        # The surviving inode must never be mistaken for the renamed file.
        assert external.read_bytes() == b"VICTIM-SURVIVING-HARDLINK"
        assert (external.stat().st_dev, external.stat().st_ino) == (
            original_victim_stat.st_dev, original_victim_stat.st_ino)
        committed = phase not in ("after-wal", "after-victim-cleanup", "after-staging")
        if committed:
            assert not source.exists(), phase
            assert victim.read_bytes() == b"SOURCE-UNIQUE-PAYLOAD", phase
            assert (victim.stat().st_dev, victim.stat().st_ino) == (
                original_source_stat.st_dev, original_source_stat.st_ino), phase
            assert dest_key not in os.listxattr(external), phase
            assert os.getxattr(victim, dest_key) == src_alias, phase
            assert source_key not in os.listxattr(victim), phase
            assert external.stat().st_nlink == 1
        else:
            assert source.read_bytes() == b"SOURCE-UNIQUE-PAYLOAD", phase
            assert victim.read_bytes() == b"VICTIM-SURVIVING-HARDLINK", phase
            assert os.getxattr(external, dest_key) == victim_alias, phase
            assert os.getxattr(source, source_key) == src_alias, phase
            assert dest_key not in os.listxattr(source), phase
            assert external.stat().st_nlink == 2
        print("PASS replacement + survivor hardlink + SIGKILL:", phase)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: dos83_wal_sigkill.py NORMAL_MEDIATOR FAULT_MEDIATOR")
    regular, injected = (str(Path(x).resolve()) for x in sys.argv[1:])
    for phase in PHASES:
        scenario(regular, injected, phase)
        replacement_hardlink_scenario(regular, injected, phase)
    replacement_hardlink_scenario(regular, injected, "after-victim-cleanup")


if __name__ == "__main__":
    main()
