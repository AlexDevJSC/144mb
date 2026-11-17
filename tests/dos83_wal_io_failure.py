#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Durable WAL I/O uncertainty: a failed fsync must poison the process.

The first move may already have committed physically; an apparently empty
journal must NEVER allow the second operation in the same process.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run(binary, root, *args, env=None):
    return subprocess.run([binary, "--root", str(root), *args],
                          text=True, capture_output=True, timeout=15, env=env)


def scenario(normal, fault, phase):
    with tempfile.TemporaryDirectory(prefix="nt-fs4-wal-eio-") as temp:
        root = Path(temp)
        a, b = root / "firstLongName.dat", root / "firstMovedName.dat"
        c, d = root / "secondLongName.dat", root / "secondMovedName.dat"
        a.write_bytes(b"FIRST-DO-NOT-DAMAGE")
        c.write_bytes(b"SECOND-DO-NOT-DAMAGE")
        env = dict(os.environ, NT_DOS83_TEST_IO_ERROR=phase)
        result = run(fault, root, "--sequence-move",
                     "C:/" + a.name, "C:/" + b.name,
                     "C:/" + c.name, "C:/" + d.name, env=env)
        assert result.returncode != 0, (phase, result.stderr)
        assert "first=0 second=0" in result.stderr, (phase, result.stderr)
        # Most importantly: no second mutation when the first fsync failed.
        assert c.read_bytes() == b"SECOND-DO-NOT-DAMAGE", phase
        assert not d.exists(), phase
        committed = phase == "wal-clear-fsync"
        if committed:
            assert not a.exists()
            assert b.read_bytes() == b"FIRST-DO-NOT-DAMAGE"
        else:
            assert a.read_bytes() == b"FIRST-DO-NOT-DAMAGE"
            assert not b.exists()
        journal = root / ":ntnative-file-leases" / "dos83-rename-v1.wal"
        assert journal.exists()
        if phase == "wal-clear-fsync":
            assert journal.stat().st_size == 0  # danger: looks clean in page cache
        if phase == "wal-write-fsync":
            assert journal.stat().st_size > 0
        recovered = run(normal, root, "--recover")
        assert recovered.returncode == 0, (phase, recovered.stderr)
        assert run(normal, root, "--recover").returncode == 0
        assert journal.stat().st_size == 0
        if not committed:
            assert run(normal, root, "--move",
                       "C:/" + a.name, "C:/" + b.name).returncode == 0
        assert run(normal, root, "--move",
                   "C:/" + c.name, "C:/" + d.name).returncode == 0
        assert b.read_bytes() == b"FIRST-DO-NOT-DAMAGE"
        assert d.read_bytes() == b"SECOND-DO-NOT-DAMAGE"
        print("PASS poisoned WAL blocks next operation + fresh recovery:", phase)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: dos83_wal_io_failure.py NORMAL FAULT_MEDIATOR")
    normal, fault = (str(Path(x).resolve()) for x in sys.argv[1:])
    for phase in ("wal-open-fsync", "wal-write-fsync", "wal-clear-fsync"):
        scenario(normal, fault, phase)


if __name__ == "__main__":
    main()
