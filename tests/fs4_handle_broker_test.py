#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Win32 runtime HANDLEs over per-client binary IPC, no shared backing FDs."""
import os
import fcntl
import threading
import time
from pathlib import Path
import secrets
import socket
import struct
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv[1]).resolve()) if len(sys.argv) > 1 else ""
sys.argv = sys.argv[:1]
REQ = struct.Struct("<IHHQqIIIIIIQ")
REP = struct.Struct("<IHHIIQQ")
MAGIC, VERSION = 0x34465348, 3
OPEN, READ, WRITE, CLOSE, DELETE, RENAME, TRUNCATE, LOCK, UNLOCK = range(1, 10)
ATTR_GET, ATTR_SET, FIND_FIRST, FIND_NEXT, FIND_CLOSE, DUPLICATE, READ_AT, WRITE_AT, HANDLE_INFO = range(12, 21)

class Broker:
    def __init__(self, root):
        server, self.client = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.client.settimeout(5)
        self.process = subprocess.Popen([
            BINARY, "--root", str(root), "--fd", str(server.fileno()),
            "--client-uid", str(os.geteuid()), "--test"
        ], pass_fds=(server.fileno(),), stdin=subprocess.DEVNULL,
           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        server.close()
        hello = self.client.recv(4096)
        if len(hello) != REP.size:
            raise AssertionError(("bad hello", hello, self.process.poll(),
                                 self.process.stderr.read().decode()))
        mag, ver, op, status, n, cap, value = REP.unpack(hello)
        assert (mag, ver, op, status, n, cap) == (MAGIC, VERSION, 0, 0, 0, 0)
        self.nonce = value

    def call(self, op, data=b"", handle=0, offset=-1, access=0, share=0,
             disposition=0, flags=0, size=None, nonce=None):
        if size is None:
            size = len(data)
        request = REQ.pack(MAGIC, VERSION, op, handle, offset,
                           access, share, disposition, flags, size, 0,
                           self.nonce if nonce is None else nonce)
        self.client.sendall(request + data)
        frame = self.client.recv(4096)
        assert len(frame) >= REP.size, frame
        mag, ver, reply_op, err, length, cap, value = REP.unpack(frame[:REP.size])
        assert (mag, ver, reply_op) == (MAGIC, VERSION, op)
        assert len(frame) == REP.size + length, (frame, length)
        return err, cap, value, frame[REP.size:]

    def close(self):
        self.client.close()
        self.process.wait(timeout=5)
        assert self.process.returncode == 0, self.process.stderr.read().decode()

class FileSystem4Handles(unittest.TestCase):
    def test_create_write_read_truncate_delete_and_capabilities(self):
        with tempfile.TemporaryDirectory(prefix="fs4-broker-") as temp:
            root = Path(temp) / "C"
            root.mkdir(mode=0o700)
            b = Broker(root)
            try:
                error, h, _, _ = b.call(OPEN, b"C:/file.txt", access=0xC0000000,
                                        share=7, disposition=1)
                self.assertEqual(error, 0)
                self.assertNotEqual(h, 0)
                self.assertEqual(b.call(WRITE, b"HELLO", handle=h)[0], 0)
                self.assertEqual(b.call(READ, handle=h, offset=0, size=5),
                                 (0, 0, 5, b"HELLO"))
                self.assertEqual(b.call(TRUNCATE, handle=h, offset=3)[0], 0)
                self.assertEqual(b.call(CLOSE, handle=h)[0], 0)
                self.assertEqual((root / "file.txt").read_bytes(), b"HEL")
                self.assertNotEqual(b.call(READ, handle=h, size=1)[0], 0)
                self.assertEqual(b.call(RENAME, b"C:/file.txt\0C:/moved.txt")[0], 0)
                self.assertEqual(b.call(DELETE, b"C:/moved.txt")[0], 0)
                self.assertFalse((root / "moved.txt").exists())
                self.assertNotEqual(b.call(OPEN, b"C:/../escape",
                                           access=0x80000000, disposition=3)[0], 0)
                self.assertFalse((root.parent / "escape").exists())
            finally:
                b.close()

    def test_enumeration_attributes_and_search_capability_lifetime(self):
        with tempfile.TemporaryDirectory(prefix="fs4-attrs-") as temp:
            root = Path(temp) / "C"
            root.mkdir(mode=0o700)
            (root / "first.txt").write_bytes(b"FIRST")
            (root / "second.txt").write_bytes(b"SECOND")
            (root / "other.bin").write_bytes(b"BIN")
            broker = Broker(root)
            try:
                err, _, _, attrs = broker.call(ATTR_GET, b"C:/first.txt")
                self.assertEqual(err, 0)
                self.assertEqual(len(attrs), 36)
                flags = struct.unpack_from("<I", attrs, 0)[0]
                self.assertNotEqual(flags & 0x80, 0)  # NORMAL
                self.assertEqual(broker.call(ATTR_SET, b"C:/first.txt", flags=1)[0], 0)
                err, _, _, attrs = broker.call(ATTR_GET, b"C:/first.txt")
                self.assertEqual(err, 0)
                self.assertEqual(struct.unpack_from("<I", attrs)[0] & 1, 1)
                self.assertEqual(broker.call(ATTR_SET, b"C:/first.txt", flags=0x80)[0], 0)

                err, search, _, result = broker.call(FIND_FIRST, b"C:/*.txt")
                self.assertEqual(err, 0)
                self.assertNotEqual(search, 0)
                seen = set()
                while True:
                    self.assertEqual(len(result), 592)
                    name = result[44:44 + 520].decode("utf-16le").split("\0")[0]
                    seen.add(name)
                    err, _, _, result = broker.call(FIND_NEXT, handle=search)
                    if err == 18:  # ERROR_NO_MORE_FILES
                        break
                    self.assertEqual(err, 0)
                self.assertEqual(seen, {"first.txt", "second.txt"})
                self.assertEqual(broker.call(CLOSE, handle=search)[0], 87)
                self.assertEqual(broker.call(FIND_CLOSE, handle=search)[0], 0)
                self.assertNotEqual(broker.call(FIND_NEXT, handle=search)[0], 0)
                self.assertEqual(broker.call(FIND_FIRST, b"C:/../*", flags=0)[0], 87)
            finally:
                broker.close()

    def test_positional_io_duplicate_and_same_file_offset(self):
        with tempfile.TemporaryDirectory(prefix="fs4-positional-") as temp:
            root = Path(temp) / "C"
            root.mkdir(mode=0o700)
            broker = Broker(root)
            try:
                err, handle, _, _ = broker.call(OPEN, b"C:/data.bin",
                                                access=0xC0000000,
                                                share=7, disposition=1)
                self.assertEqual(err, 0)
                self.assertEqual(broker.call(WRITE, b"012345", handle=handle)[0], 0)
                err, _, count, data = broker.call(READ_AT, handle=handle,
                                                  offset=1, size=3)
                self.assertEqual((err, count, data), (0, 3, b"123"))
                self.assertEqual(broker.call(WRITE_AT, b"XX", handle=handle,
                                              offset=2)[0], 0)
                err, _, count, data = broker.call(READ, handle=handle, size=2)
                self.assertEqual((err, count, data), (0, 0, b""))  # cursor still EOF
                err, duplicate, _, _ = broker.call(DUPLICATE, handle=handle,
                                                    flags=2)
                self.assertEqual(err, 0)
                self.assertNotEqual(duplicate, handle)
                self.assertNotEqual(broker.call(DUPLICATE, handle=handle,
                                                 flags=2, share=1)[0], 0)
                self.assertEqual(broker.call(CLOSE, handle=handle)[0], 0)
                err, _, count, data = broker.call(READ_AT, handle=duplicate,
                                                  offset=0, size=6)
                self.assertEqual((err, count, data), (0, 6, b"01XX45"))
                err, _, _, info = broker.call(HANDLE_INFO, handle=duplicate)
                self.assertEqual(err, 0)
                self.assertEqual(len(info), 52)
                self.assertEqual(broker.call(CLOSE, handle=duplicate)[0], 0)
                self.assertEqual((root / "data.bin").read_bytes(), b"01XX45")
            finally:
                broker.close()

    def test_shared_lease_conflict_and_process_exit_release(self):
        with tempfile.TemporaryDirectory(prefix="fs4-lease-") as temp:
            root = Path(temp) / "C"
            root.mkdir(mode=0o700)
            (root / "file.txt").write_bytes(b"LOCKED")
            a, b = Broker(root), Broker(root)
            try:
                err, handle, _, _ = a.call(OPEN, b"C:/file.txt", access=0x80000000,
                                            share=0, disposition=3)
                self.assertEqual(err, 0)
                conflict, _, _, _ = b.call(OPEN, b"C:/file.txt",
                                            access=0x80000000, share=7,
                                            disposition=3)
                self.assertEqual(conflict, 32)  # ERROR_SHARING_VIOLATION
                self.assertEqual(a.call(CLOSE, handle=handle)[0], 0)
                err, next_handle, _, _ = b.call(OPEN, b"C:/file.txt",
                                                 access=0x80000000, share=7,
                                                 disposition=3)
                self.assertEqual(err, 0)
                self.assertEqual(b.call(CLOSE, handle=next_handle)[0], 0)
            finally:
                a.close()
                b.close()

    def test_service_admission_serializes_file_open_and_leases(self):
        with tempfile.TemporaryDirectory(prefix="fs4-admission-") as temp:
            root = Path(temp) / "C"
            root.mkdir(mode=0o700)
            (root / "same.txt").write_bytes(b"ORIGINAL")
            private = root / ":ntnative-file-leases"
            private.mkdir(mode=0o700)
            lockfile = private / "fs4-service-admission-v1.lock"
            guard = os.open(lockfile, os.O_RDWR | os.O_CREAT, 0o600)
            a, b = Broker(root), Broker(root)
            try:
                fcntl.flock(guard, fcntl.LOCK_EX)
                observed = []
                errors = []
                def attempt(broker):
                    try:
                        observed.append(broker.call(OPEN, b"C:/same.txt",
                                                    access=0x80000000,
                                                    share=0, disposition=3))
                    except Exception as exc:
                        errors.append(exc)
                first = threading.Thread(target=attempt, args=(a,))
                second = threading.Thread(target=attempt, args=(b,))
                first.start()
                second.start()
                time.sleep(0.2)
                self.assertEqual(observed, [], "Broker bypassed service admission")
                fcntl.flock(guard, fcntl.LOCK_UN)
                first.join(timeout=4)
                second.join(timeout=4)
                self.assertFalse(first.is_alive())
                self.assertFalse(second.is_alive())
                self.assertEqual(errors, [])
                self.assertEqual(sorted(item[0] for item in observed),
                                 [0, 32])  # one OPEN, one FILE_SHARE conflict
                opened = next(item for item in observed if item[0] == 0)
                # Only the worker holding the winning capability can close.
                closed_by = a if a.call(CLOSE, handle=opened[1])[0] == 0 else b
                if closed_by is b:
                    self.assertEqual(b.call(CLOSE, handle=opened[1])[0], 6)
            finally:
                fcntl.flock(guard, fcntl.LOCK_UN)
                os.close(guard)
                a.close()
                b.close()

    def test_corrupt_wal_denies_create_before_posix_mutation(self):
        with tempfile.TemporaryDirectory(prefix="fs4-wal-gate-") as temp:
            root=Path(temp)/"C"
            root.mkdir(mode=0o700)
            broker=Broker(root)
            try:
                directory=root/":ntnative-file-leases"
                journal=directory/"dos83-rename-v1.wal"
                journal.write_bytes(b"INVALID-WAL")
                result=broker.call(OPEN,b"C:/must-not-exist.dat",
                                   access=0x40000000,share=7,disposition=1)
                self.assertNotEqual(result[0],0)
                self.assertFalse((root/"must-not-exist.dat").exists())
                self.assertEqual(journal.read_bytes(),b"INVALID-WAL")
            finally:
                broker.close()

    def test_wrong_session_nonce_revokes_handles_and_socket(self):
        with tempfile.TemporaryDirectory(prefix="fs4-nonce-") as temp:
            root = Path(temp) / "C"
            root.mkdir(mode=0o700)
            b = Broker(root)
            try:
                result = b.call(OPEN, b"C:/safe", access=0x40000000,
                                disposition=1, nonce=(b.nonce ^ 1))
                self.assertNotEqual(result[0], 0)
                self.assertEqual(b.client.recv(4096), b"")
            finally:
                b.close()

if __name__ == "__main__":
    unittest.main()
