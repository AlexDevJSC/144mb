#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""FileSystem4 hardlink IPC, inode/lease isolation, and real SIGKILL replay.

No PE code or executable from the outside is used. The --test broker may
be linked against a separately compiled guest_files crash object only.
"""
import os
from pathlib import Path
import signal
import sys
import tempfile
import threading
import unittest

NORMAL_BINARY = str(Path(sys.argv[1]).resolve()) if len(sys.argv) > 1 else ""
CRASH_BINARY = str(Path(sys.argv[2]).resolve()) if len(sys.argv) > 2 else ""
sys.path.insert(0, str(Path(__file__).resolve().parent))
from fs4_handle_broker_test import Broker, OPEN, READ, CLOSE, DELETE, RENAME, HARDLINK

SRC = b"C:/originalHardlinkReference.dat"
DST = b"C:/secondHardlinkReference.dat"
OTHER = b"C:/anotherHardlinkReference.dat"
PAYLOAD = SRC + b"\0" + DST


def new_root(directory):
    root = Path(directory) / "C"
    root.mkdir(mode=0o700)
    (root / "originalHardlinkReference.dat").write_bytes(b"SHARED-IDENTITY")
    return root


class HardlinkWALTest(unittest.TestCase):
    def test_create_shared_inode_lease_delete_and_rename(self):
        with tempfile.TemporaryDirectory(prefix="fs4-link-") as d:
            root = new_root(d)
            first = Broker(root, binary=NORMAL_BINARY)
            try:
                second = Broker(root, binary=NORMAL_BINARY)
            except BaseException:
                first.close()
                raise
            try:
                self.assertEqual(first.call(HARDLINK, PAYLOAD)[0], 0)
                a = root / "originalHardlinkReference.dat"
                b = root / "secondHardlinkReference.dat"
                self.assertEqual((a.stat().st_dev, a.stat().st_ino),
                                 (b.stat().st_dev, b.stat().st_ino))
                self.assertEqual(a.stat().st_nlink, 2)
                self.assertEqual(first.call(HARDLINK, PAYLOAD)[0], 183)
                err, held, _, _ = first.call(OPEN, SRC,
                    access=0x80000000, share=0, disposition=3)
                self.assertEqual(err, 0)
                self.assertEqual(second.call(OPEN, DST,
                    access=0x80000000, share=7, disposition=3)[0], 32)
                self.assertEqual(first.call(CLOSE, handle=held)[0], 0)
                err, cap, _, _ = second.call(OPEN, DST,
                    access=0x80000000, share=7, disposition=3)
                self.assertEqual(err, 0)
                self.assertEqual(second.call(READ, handle=cap, size=15),
                                 (0, 0, 15, b"SHARED-IDENTITY"))
                self.assertEqual(second.call(CLOSE, handle=cap)[0], 0)
                self.assertEqual(second.call(DELETE, SRC)[0], 0)
                self.assertFalse(a.exists())
                self.assertEqual(b.read_bytes(), b"SHARED-IDENTITY")
                self.assertEqual(b.stat().st_nlink, 1)
                self.assertEqual(second.call(RENAME, DST + b"\0" + OTHER)[0], 0)
                self.assertFalse(b.exists())
                self.assertEqual((root / "anotherHardlinkReference.dat").read_bytes(),
                                 b"SHARED-IDENTITY")
                self.assertEqual(second.call(DELETE, OTHER)[0], 0)
            finally:
                try:
                    first.close()
                finally:
                    second.close()

    def test_fail_closed_traversal_root_symlink_and_pending(self):
        with tempfile.TemporaryDirectory(prefix="fs4-link-deny-") as d:
            root = new_root(d)
            (root / "bad-symlink.dat").symlink_to("originalHardlinkReference.dat")
            broker = Broker(root, binary=NORMAL_BINARY)
            try:
                forbidden = (
                    SRC + b"\0C:/../outside.dat",
                    SRC + b"\0D:/out.dat",
                    b"D:/other.dat\0" + DST,
                    b"C:/bad-symlink.dat\0" + DST,
                    SRC + b"\0C:/bad-symlink.dat",
                    SRC + b"\0C:/originalHardlinkReference.dat",
                    SRC + b"\0//server/share/link.dat",
                )
                for attempt in forbidden:
                    with self.subTest(path=attempt):
                        self.assertNotEqual(broker.call(HARDLINK, attempt)[0], 0)
                        self.assertFalse((root / "secondHardlinkReference.dat").exists())
                err, held, _, _ = broker.call(OPEN, SRC,
                    access=0x80000000, share=7, disposition=3)
                self.assertEqual(err, 0)
                self.assertEqual(broker.call(DELETE, SRC)[0], 0)
                self.assertNotEqual(broker.call(HARDLINK, PAYLOAD)[0], 0)
                self.assertFalse((root / "secondHardlinkReference.dat").exists())
                self.assertEqual(broker.call(CLOSE, handle=held)[0], 0)
                self.assertFalse((root / "originalHardlinkReference.dat").exists())
            finally:
                broker.close()

    def test_two_workers_create_one_target_only(self):
        with tempfile.TemporaryDirectory(prefix="fs4-link-race-") as d:
            root = new_root(d)
            first = Broker(root, binary=NORMAL_BINARY)
            try:
                second = Broker(root, binary=NORMAL_BINARY)
            except BaseException:
                first.close()
                raise
            results = []
            barrier = threading.Barrier(3)
            def runner(client):
                barrier.wait(timeout=6)
                results.append(client.call(HARDLINK, PAYLOAD)[0])
            try:
                threads = [threading.Thread(target=runner, args=(b,))
                           for b in (first, second)]
                for thread in threads:
                    thread.start()
                barrier.wait(timeout=6)
                for thread in threads:
                    thread.join(timeout=10)
                self.assertFalse(any(t.is_alive() for t in threads),
                                 "concurrent hardlink WAL deadlock")
                self.assertEqual(len(results), 2)
                self.assertEqual(results.count(0), 1)
                self.assertEqual(results.count(183), 1)
                self.assertEqual((root / "secondHardlinkReference.dat").stat().st_nlink, 2)
            finally:
                try:
                    first.close()
                finally:
                    second.close()

    def test_foreign_inode_at_replay_target_blocks_and_preserves_data(self):
        with tempfile.TemporaryDirectory(prefix="fs4-link-foreign-") as d:
            root = new_root(d)
            crashed = Broker(root, binary=CRASH_BINARY,
                             wal_crash="hardlink-after-link")
            try:
                with self.assertRaises(AssertionError):
                    crashed.call(HARDLINK, PAYLOAD)
                self.assertEqual(crashed.process.wait(timeout=5),
                                 -signal.SIGKILL)
            finally:
                crashed.close(expected_returncode=-signal.SIGKILL)
            destination = root / "secondHardlinkReference.dat"
            destination.unlink()
            destination.write_bytes(b"FOREIGN INODE")
            wal_path = root / ":ntnative-file-leases" / "dos83-rename-v1.wal"
            pending = wal_path.read_bytes()
            self.assertTrue(pending.startswith(b"N83LNK03"))
            with self.assertRaises(AssertionError):
                Broker(root, binary=NORMAL_BINARY)
            self.assertEqual(destination.read_bytes(), b"FOREIGN INODE")
            self.assertEqual(wal_path.read_bytes(), pending,
                             "ambiguous replay must preserve the WAL")
            self.assertEqual((root / "originalHardlinkReference.dat").read_bytes(),
                             b"SHARED-IDENTITY")

    def test_corrupt_hardlink_wal_refuses_without_truncation(self):
        with tempfile.TemporaryDirectory(prefix="fs4-link-corrupt-") as d:
            root = new_root(d)
            crashed = Broker(root, binary=CRASH_BINARY,
                             wal_crash="hardlink-after-wal")
            try:
                with self.assertRaises(AssertionError):
                    crashed.call(HARDLINK, PAYLOAD)
                self.assertEqual(crashed.process.wait(timeout=5),
                                 -signal.SIGKILL)
            finally:
                crashed.close(expected_returncode=-signal.SIGKILL)
            wal_path = root / ":ntnative-file-leases" / "dos83-rename-v1.wal"
            original = wal_path.read_bytes()
            self.assertTrue(original.startswith(b"N83LNK03"))
            poisoned = bytearray(original)
            poisoned[32] ^= 0x40  # checksum-covered inode identity
            wal_path.write_bytes(poisoned)
            with self.assertRaises(AssertionError):
                Broker(root, binary=NORMAL_BINARY)
            self.assertEqual(wal_path.read_bytes(), poisoned)
            self.assertFalse((root / "secondHardlinkReference.dat").exists())

    def test_sigkill_at_all_wal_barriers_and_cold_recovery(self):
        phases = (
            "hardlink-after-wal", "hardlink-after-link",
            "hardlink-after-inode-fsync", "hardlink-after-parent-fsync",
            "hardlink-after-alias", "hardlink-after-wal-clear")
        for phase in phases:
            with self.subTest(phase=phase), tempfile.TemporaryDirectory(
                    prefix="fs4-link-sigkill-") as d:
                root = new_root(d)
                crashed = Broker(root, binary=CRASH_BINARY, wal_crash=phase)
                try:
                    with self.assertRaises(AssertionError):
                        crashed.call(HARDLINK, PAYLOAD)
                    self.assertEqual(crashed.process.wait(timeout=5),
                                     -signal.SIGKILL)
                finally:
                    crashed.close(expected_returncode=-signal.SIGKILL)
                # Dedicated fresh production worker executes real WAL replay,
                # not a synthetic repair of the backing from the test code.
                recovered = Broker(root, binary=NORMAL_BINARY)
                try:
                    target = root / "secondHardlinkReference.dat"
                    if phase == "hardlink-after-wal":
                        self.assertFalse(target.exists())
                    else:
                        self.assertEqual(target.read_bytes(), b"SHARED-IDENTITY")
                        self.assertEqual(
                            (root / "originalHardlinkReference.dat").stat().st_ino,
                            target.stat().st_ino)
                    self.assertEqual(recovered.call(
                        HARDLINK, SRC + b"\0" + OTHER)[0], 0,
                        "cold recovery left WAL intent locked")
                    self.assertEqual(recovered.call(DELETE, OTHER)[0], 0)
                    if target.exists():
                        self.assertEqual(recovered.call(DELETE, DST)[0], 0)
                finally:
                    recovered.close()


if __name__ == "__main__":
    if not NORMAL_BINARY or not CRASH_BINARY:
        raise SystemExit("Usage: fs4_hardlink_wal_test.py BROKER CRASH_BROKER")
    unittest.main()
