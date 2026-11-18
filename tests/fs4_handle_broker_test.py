#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Win32 runtime HANDLEs over per-client binary IPC, no shared backing FDs."""
import os
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
