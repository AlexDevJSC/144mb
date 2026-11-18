#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Kill test-only FUSE backend after lease, link, fsync and alias.
This verifies kernel-visible SIGKILL recovery, NOT power-loss durability.
No physical mount or SELinux acceptance can be inferred from this harness.
"""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

from dos83_wal_recovery import entry_key

PHASES = ("post-lease", "post-link", "post-fsync", "post-alias")


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run(*argv, env=None):
    return subprocess.run(argv, env=env, capture_output=True,
                          text=True, timeout=30)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: fs4_fuse_stage_sigkill.py FUSE MEDIATOR")
    fuse, mediator = (str(Path(p).resolve()) for p in sys.argv[1:])
    for phase in PHASES:
        with tempfile.TemporaryDirectory(prefix="fs4-stage-kill-") as temp:
            root = Path(temp) / "C"
            root.mkdir(mode=0o700)
            name = "FS4StagedLeaseTest.txt"
            target = root / name
            killed = run(fuse, "--selftest", str(root),
                         env=dict(os.environ, NT_FS4_FUSE_TEST_CRASH=phase))
            require(killed.returncode == -signal.SIGKILL,
                    (phase, killed.returncode, killed.stderr[-2000:]))
            # A freshly launched process must recover the empty WAL,
            # including after the staged inode was linked but not aliased.
            for _ in range(2):
                result = run(mediator, "--root", str(root), "--recover")
                require(result.returncode == 0,
                        (phase, result.returncode, result.stderr))
            if phase == "post-lease":
                require(not target.exists(),
                        "lease-only inode was prematurely published")
            else:
                require(target.is_file(), (phase, "published inode disappeared"))
                require(target.stat().st_size == 0,
                        (phase, "unexpected content"))
                result = run(mediator, "--root", str(root), "--refresh",
                             "C:/" + name)
                require(result.returncode == 0,
                        (phase, "alias not recoverable", result.stderr))
                require(b"~" in os.getxattr(target, entry_key(root, name)),
                        (phase, "DOS83 alias not recovered"))
                # The dead FUSE callback cannot retain a live share lease:
                # a new broker worker needs to be able to reopen the inode.
                result = run(mediator, "--root", str(root), "--truncate",
                             "C:/" + name)
                require(result.returncode == 0,
                        (phase, "orphaned lease", result.stderr))
            print("PASS FUSE staged SIGKILL/reopen/recovery:", phase)


if __name__ == "__main__":
    main()
