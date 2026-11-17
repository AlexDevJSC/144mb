#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Kill real mediated rename processes at syscall boundaries; then replay WAL.

Only disposable /tmp files are used. Not a power-loss, storage flush, SELinux
or Windows-NTFS proof: these tests verify real SIGKILL and process restart.
"""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

PHASES = (
    ("after_wal_fsync", False),
    ("after_staging_xattr", False),
    ("after_staged_inode_fsync", False),
    ("after_source_alias_cleanup", True),
    ("after_parent_fsync", True),
)
MAGIC_WAL = ":ntnative-file-leases/dos83-rename-v1.wal"
ORIGINAL = b"fs4-original-content-\x00\xff-2026"


def run(mediator, root, *args, env=None, expected=0):
    result = subprocess.run([mediator, "--root", str(root), *args],
                            env=env, capture_output=True, text=True)
    assert result.returncode == expected, (
        f"command {args} exited {result.returncode} not {expected}: "
        f"{result.stdout}\n{result.stderr}")
    return result


def aliases(path):
    return {name: os.getxattr(path, name)
            for name in os.listxattr(path)
            if name.startswith("user.ntnative.dos83.")}


def exercise(mediator, preload, phase, renamed):
    with tempfile.TemporaryDirectory(prefix="fs4-sigkill-") as temp:
        root = Path(temp)
        (root / "left").mkdir()
        (root / "right").mkdir()
        source = root / "left" / "longOriginalNameForCrashTest.txt"
        target = root / "right" / "longDestinationNameForCrashTest.txt"
        source.write_bytes(ORIGINAL)
        initial_inode = source.stat().st_ino
        run(mediator, root, "--refresh", "C:/left/longOriginalNameForCrashTest.txt")
        initial_alias = aliases(source)
        assert len(initial_alias) == 1, initial_alias
        env = os.environ.copy()
        env["LD_PRELOAD"] = preload
        env["NT_FS4_TEST_KILL_AT"] = phase
        run(mediator, root, "--move", "C:/left/longOriginalNameForCrashTest.txt",
            "C:/right/longDestinationNameForCrashTest.txt", env=env,
            expected=-signal.SIGKILL)
        journal = root / MAGIC_WAL
        assert journal.stat().st_size > 0, "crash did not leave a pending WAL"

        # Recovery MUST run in a new process, without the preload. Preserve
        # byte content, one physical inode, and exactly one usable DOS alias.
        run(mediator, root, "--recover")
        current = target if renamed else source
        old = source if renamed else target
        assert current.is_file() and not old.exists()
        assert current.stat().st_ino == initial_inode
        assert current.read_bytes() == ORIGINAL
        assert journal.stat().st_size == 0
        keys = aliases(current)
        assert len(keys) == 1, (phase, keys)
        if renamed:
            assert list(keys.values()) == list(initial_alias.values())
            assert not set(keys).intersection(initial_alias)
        else:
            assert keys == initial_alias
        run(mediator, root, "--recover")  # idempotent, record still zero
        followup = root / "left" / "anotherOriginalLongFileName.txt"
        followup.write_bytes(b"followup")
        run(mediator, root, "--move", "C:/left/anotherOriginalLongFileName.txt",
            "C:/right/anotherLongDestinationFile.txt")
        assert (root / "right" / "anotherLongDestinationFile.txt").read_bytes() == b"followup"
        print(f"PASS real SIGKILL + new-process WAL recovery: {phase}")


def main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: dos83_sigkill_recovery.py MEDIATOR PRELOAD_SO")
    mediator, preload = map(lambda p: str(Path(p).resolve()), sys.argv[1:])
    for phase, renamed in PHASES:
        exercise(mediator, preload, phase, renamed)


if __name__ == "__main__":
    main()
