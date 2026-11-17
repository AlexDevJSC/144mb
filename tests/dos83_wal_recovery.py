#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""FileSystem4: simulate durable on-disk interruption states of a DOS 8.3
rename and require verified, fail-closed WAL replay. All fixtures disposable.
A simulated crash snapshot is not a real power-cut or NTFS equivalence test.
"""
import os
import pathlib
import subprocess
import sys
import tempfile

MASK = (1 << 64) - 1
MAGIC = b"N83WAL01"
HEADER = 140
REGISTRY = ":ntnative-file-leases"
JOURNAL = "dos83-rename-v1.wal"


def hash64(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & MASK
    return value


def entry_key(parent, leaf):
    st = parent.stat()
    h1 = 0xCBF29CE484222325
    h2 = 0x84222325CBF29CE4
    for byte in os.fsencode(leaf):
        if 65 <= byte <= 90:
            byte += 32
        h1 = ((h1 ^ byte) * 0x100000001B3) & MASK
        h2 = ((h2 ^ (byte ^ 0xA5)) * 0x100000001B3) & MASK
    return (f"user.ntnative.dos83.{st.st_dev:016x}.{st.st_ino:016x}."
            f"{h1:016x}.{h2:016x}")


def make_record(root, source, dest, original_alias, *, victim=None):
    parent_from, parent_to = source.parent.stat(), dest.parent.stat()
    st_root, st_source = root.stat(), source.stat()
    before = source.relative_to(root).as_posix().encode("utf-8") + b"\x00"
    after = dest.relative_to(root).as_posix().encode("utf-8") + b"\x00"
    payload = bytearray(HEADER + len(before) + len(after))
    payload[:8] = MAGIC
    numbers = (st_root.st_dev, st_root.st_ino,
               st_source.st_dev, st_source.st_ino,
               parent_from.st_dev, parent_from.st_ino,
               parent_to.st_dev, parent_to.st_ino,
               victim.st_dev if victim else 0,
               victim.st_ino if victim else 0)
    for i, num in enumerate(numbers):
        payload[16 + 8*i:24 + 8*i] = num.to_bytes(8, "little")
    flags = 1 | (2 if victim else 0)
    payload[96:100] = flags.to_bytes(4, "little")
    payload[100:104] = (0).to_bytes(4, "little")  # old destination xattr absent
    payload[104:108] = len(before).to_bytes(4, "little")
    payload[108:112] = len(after).to_bytes(4, "little")
    payload[126:126 + len(original_alias)] = original_alias
    payload[HEADER:HEADER+len(before)] = before
    payload[HEADER+len(before):] = after
    payload[8:16] = hash64(payload[16:]).to_bytes(8, "little")
    return payload


def run(tool, root, *args, success=True):
    result = subprocess.run([tool, "--root", str(root), *args],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True)
    if success and result.returncode != 0:
        raise AssertionError(f"Unexpected failure {args}: {result.stderr}")
    if not success and result.returncode == 0:
        raise AssertionError(f"Unexpected success {args}")
    return result


def scenario(tool, crash_phase):
    with tempfile.TemporaryDirectory(prefix="nt-dos83-wal-") as temp:
        root = pathlib.Path(temp)
        left, right = root/"left", root/"right"
        left.mkdir()
        right.mkdir()
        source = left/"longJournalSourceName.txt"
        dest = right/"longJournalTargetName.txt"
        source.write_bytes(b"UNCHANGED ORIGINAL BYTES")
        run(tool, root, "--refresh", "C:/left/longJournalSourceName.txt")
        source_key = entry_key(left, source.name)
        dest_key = entry_key(right, dest.name)
        alias = os.getxattr(source, source_key)
        assert b"~" in alias and alias.endswith(b"\x00")
        record = make_record(root, source, dest, alias)
        journal = root/REGISTRY/JOURNAL
        journal.write_bytes(record)
        os.chmod(journal, 0o600)  # Production WAL rejects world/group-readable records.
        with journal.open("rb") as stream:
            os.fsync(stream.fileno())

        if crash_phase != "before_staging":
            os.setxattr(source, dest_key, alias)
            with source.open("rb") as stream:
                os.fsync(stream.fileno())

        if crash_phase in ("after_rename", "undecidable", "tampered_dest"):
            source.rename(dest)
        if crash_phase == "undecidable":
            dest.unlink()
        if crash_phase == "tampered_source":
            os.setxattr(source, dest_key, b"UNKNOWN~1.TXT\x00")
        if crash_phase == "tampered_dest":
            os.setxattr(dest, dest_key, b"UNKNOWN~1.TXT\x00")

        if crash_phase == "corrupt":
            data = bytearray(journal.read_bytes())
            data[45] ^= 0x17
            journal.write_bytes(data)

        expected_failure = crash_phase in (
            "corrupt", "undecidable", "tampered_source", "tampered_dest")
        result = run(tool, root, "--recover", success=not expected_failure)
        if expected_failure:
            assert journal.stat().st_size > 0
            assert "errno" not in result.stdout.lower()
            # No new rename may reuse a WAL that was rejected.
            another = left/"anotherLongJournalSource.txt"
            another.write_text("X")
            run(tool, root, "--move", "C:/left/anotherLongJournalSource.txt",
                "C:/right/anotherLongJournalTarget.txt", success=False)
            assert another.is_file()
            return

        assert journal.stat().st_size == 0
        current = dest if crash_phase == "after_rename" else source
        assert current.read_bytes() == b"UNCHANGED ORIGINAL BYTES"
        if crash_phase == "after_rename":
            assert os.getxattr(dest, dest_key) == alias
            assert source_key not in os.listxattr(dest)
            assert not source.exists()
        else:
            assert source_key in os.listxattr(source)
            assert dest_key not in os.listxattr(source)
            assert not dest.exists()
        assert run(tool, root, "--recover").returncode == 0
        print(f"PASS DOS83 WAL recovery: {crash_phase}")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Usage: dos83_wal_recovery.py MEDIATOR")
    tool = str(pathlib.Path(sys.argv[1]).resolve())
    for crash_phase in ("before_staging", "after_staging",
                        "after_rename", "corrupt", "undecidable",
                        "tampered_source", "tampered_dest"):
        scenario(tool, crash_phase)


if __name__ == "__main__":
    main()
