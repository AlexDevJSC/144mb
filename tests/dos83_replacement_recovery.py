#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Hand-authored N83WAL02 replacement records; no FUSE/Fedora required.

Prove backward-compatible v1 replay (other tests), v2 preimage replay,
surviving hardlinks OUTSIDE grants, and refusal to overwrite foreign xattrs.
"""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

from dos83_wal_recovery import MASK, entry_key, hash64, run

MAGIC = b"N83WAL02"
HEADER = 160


def record(root, source, dest, src_alias, victim, victim_alias):
    src_st, victim_st = source.stat(), victim.stat()
    source_st, dest_st, root_st = source.parent.stat(), dest.parent.stat(), root.stat()
    old = os.fsencode(source.relative_to(root).as_posix()) + b"\0"
    new = os.fsencode(dest.relative_to(root).as_posix()) + b"\0"
    raw = bytearray(HEADER + len(old) + len(new))
    raw[:8] = MAGIC
    ids = (root_st.st_dev, root_st.st_ino, src_st.st_dev, src_st.st_ino,
           source_st.st_dev, source_st.st_ino, dest_st.st_dev, dest_st.st_ino,
           victim_st.st_dev, victim_st.st_ino)
    for i, value in enumerate(ids):
        raw[16 + 8*i:24 + 8*i] = int(value).to_bytes(8, "little")
    raw[96:100] = (7).to_bytes(4, "little")  # staged | victim | preclean
    raw[100:104] = (0).to_bytes(4, "little")  # destination source preimage absent
    raw[104:108] = len(old).to_bytes(4, "little")
    raw[108:112] = len(new).to_bytes(4, "little")
    raw[126:126 + len(src_alias)] = src_alias
    raw[140:140 + len(victim_alias)] = victim_alias
    raw[154:158] = len(victim_alias).to_bytes(4, "little")
    raw[HEADER:HEADER + len(old)] = old
    raw[HEADER + len(old):] = new
    raw[8:16] = hash64(raw[16:]).to_bytes(8, "little")
    return raw


def scenario(tool, phase, *, fault_tool=None, replay_phase=None):
    with tempfile.TemporaryDirectory(prefix="nt-fs4-v2-recovery-") as temp:
        base = Path(temp)
        root = base / "C"
        root.mkdir()
        left, right = root / "left", root / "right"
        left.mkdir()
        right.mkdir()
        source = left / "longSourceOriginal.txt"
        dest = right / "longDestinationVictim.txt"
        surviving = base / "hardlink-OUTSIDE-grant.txt"
        source.write_bytes(b"source")
        dest.write_bytes(b"victim")
        os.link(dest, surviving)
        src_dos = "C:/left/" + source.name
        dst_dos = "C:/right/" + dest.name
        run(tool, root, "--refresh", src_dos)
        run(tool, root, "--refresh", dst_dos)
        src_key = entry_key(left, source.name)
        dst_key = entry_key(right, dest.name)
        src_alias = os.getxattr(source, src_key)
        victim_alias = os.getxattr(dest, dst_key)

        journal = root / ":ntnative-file-leases" / "dos83-rename-v1.wal"
        journal.write_bytes(record(root, source, dest, src_alias, dest, victim_alias))
        os.chmod(journal, 0o600)
        with journal.open("rb") as stream:
            os.fsync(stream.fileno())
        if phase != "before-cleanup":
            os.removexattr(dest, dst_key)
        if phase in ("staged", "committed"):
            os.setxattr(source, dst_key, src_alias)
        if phase == "committed":
            os.replace(source, dest)
        if phase == "foreign":
            os.setxattr(surviving, dst_key, b"FOREIGN~8.TXT\0")
        if phase == "corrupt":
            raw = bytearray(journal.read_bytes())
            raw[145] ^= 0x40
            journal.write_bytes(raw)
        should_fail = phase in ("foreign", "corrupt")
        if replay_phase:
            assert fault_tool is not None
            killed = subprocess.run(
                [fault_tool, "--root", str(root), "--recover"],
                env=dict(os.environ, NT_DOS83_TEST_CRASH=replay_phase),
                text=True, capture_output=True, timeout=15)
            assert killed.returncode == -signal.SIGKILL, (
                phase, replay_phase, killed.returncode, killed.stderr)
            assert journal.stat().st_size > 0, (phase, replay_phase)
        result = run(tool, root, "--recover", success=not should_fail)
        if should_fail:
            assert journal.stat().st_size > 0
            if phase == "foreign":
                assert os.getxattr(surviving, dst_key) == b"FOREIGN~8.TXT\0"
            assert source.read_bytes() == b"source"
            assert surviving.read_bytes() == b"victim"
            assert run(tool, root, "--replace", src_dos, dst_dos,
                       success=False).returncode != 0
            print("PASS v2 fail closed:", phase)
            return
        run(tool, root, "--recover")
        assert journal.stat().st_size == 0
        if phase == "committed":
            assert not source.exists()
            assert dest.read_bytes() == b"source"
            assert os.getxattr(dest, dst_key) == src_alias
            assert src_key not in os.listxattr(dest)
            assert dst_key not in os.listxattr(surviving)
        else:
            assert source.read_bytes() == b"source"
            assert dest.read_bytes() == b"victim"
            assert os.getxattr(dest, dst_key) == victim_alias
            assert os.getxattr(source, src_key) == src_alias
            assert dst_key not in os.listxattr(source)
        print("PASS v2 victim preimage recovery:", phase)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "Usage: dos83_replacement_recovery.py MEDIATOR FAULT_MEDIATOR")
    normal, injected = (str(Path(x).resolve()) for x in sys.argv[1:])
    for phase in ("before-cleanup", "cleaned", "staged", "committed",
                  "foreign", "corrupt"):
        scenario(normal, phase)
    for state, hook in (
        ("cleaned", "replay-after-victim-restore"),
        ("staged", "replay-after-victim-restore"),
        ("staged", "replay-after-source-restore"),
        ("committed", "replay-after-source-restore"),
    ):
        scenario(normal, state, fault_tool=injected, replay_phase=hook)


if __name__ == "__main__":
    main()
