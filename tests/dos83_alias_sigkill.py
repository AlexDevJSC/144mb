#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real SIGKILL of lazy DOS8.3 metadata generation plus fresh process retry.

No Fedora/FUSE needed. A process crash is NOT a power cut, even with fsync.
"""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

from dos83_wal_recovery import entry_key

PHASES = ("alias-after-write", "alias-after-fsync")


def launch(mediator, root, dos, env=None):
    return subprocess.run([mediator, "--root", str(root), "--refresh", dos],
                          text=True, capture_output=True, env=env, timeout=15)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: dos83_alias_sigkill.py NORMAL FAULT")
    normal, fault = (str(Path(x).resolve()) for x in sys.argv[1:])
    for phase in PHASES:
        with tempfile.TemporaryDirectory(prefix="nt-fs4-alias-durability-") as temp:
            root = Path(temp)
            source = root / "oneLongNameForDurability.txt"
            collision = root / "oneLongNameForDurabilityBackup.txt"
            source.write_bytes(b"stable bytes")
            collision.write_bytes(b"other stable bytes")
            path = "C:/" + source.name
            env = dict(os.environ, NT_DOS83_TEST_CRASH=phase)
            killed = launch(fault, root, path, env)
            assert killed.returncode == -signal.SIGKILL, (
                phase, killed.returncode, killed.stderr)
            # Even if the process died before it could sync, a new process
            # must fsync the existing alias, not assume success from xattr.
            fresh = launch(normal, root, path)
            assert fresh.returncode == 0, (phase, fresh.stderr)
            key = entry_key(root, source.name)
            alias = os.getxattr(source, key)
            assert alias.endswith(b"\0") and b"~" in alias
            assert source.read_bytes() == b"stable bytes"
            again = launch(normal, root, path)
            assert again.returncode == 0, (phase, again.stderr)
            assert os.getxattr(source, key) == alias
            competitor = launch(normal, root, "C:/" + collision.name)
            assert competitor.returncode == 0, (phase, competitor.stderr)
            assert os.getxattr(collision, entry_key(root, collision.name)) != alias
            print("PASS lazy DOS83 fsync/retry/SIGKILL:", phase)


if __name__ == "__main__":
    main()
