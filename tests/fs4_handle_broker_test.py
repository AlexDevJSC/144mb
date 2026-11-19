#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Win32 runtime HANDLEs over per-client binary IPC, no shared backing FDs."""
import os
import signal
import threading
from pathlib import Path
import secrets
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

BINARY = str(Path(sys.argv[1]).resolve()) if len(sys.argv) > 1 else ""
sys.argv = sys.argv[:1]
REQ = struct.Struct("<IHHQqIIIIIIQ")
REP = struct.Struct("<IHHIIQQ")
MAGIC, VERSION = 0x34465348, 3
OPEN, READ, WRITE, CLOSE, DELETE, RENAME, TRUNCATE, LOCK, UNLOCK = range(1, 10)
SEEK, SIZE = 10, 11
ATTR_GET, ATTR_SET, FIND_FIRST, FIND_NEXT, FIND_CLOSE, DUPLICATE, READ_AT, WRITE_AT, HANDLE_INFO = range(12, 21)
NATIVE_QUERY, NATIVE_SET, NATIVE_FLUSH = 21, 22, 23

class Broker:
    def __init__(self, root, *, crash=None, binary=None, expire=False):
        server, self.client = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.client.settimeout(5)
        env = dict(os.environ)
        if crash:
            env["NT_FS4_TEST_CRASH"] = crash
        else:
            env.pop("NT_FS4_TEST_CRASH", None)
        # Explicit offline-only injection: never inherited by other tests.
        env.pop("NT_FS4_TEST_EXPIRE_1S", None)
        if expire:
            env["NT_FS4_TEST_EXPIRE_1S"] = "1"
        self.process = None
        try:
            self.process = subprocess.Popen([
                BINARY if binary is None else binary,
                "--root", str(root), "--fd", str(server.fileno()),
                "--client-uid", str(os.geteuid()), "--test"
            ], pass_fds=(server.fileno(),), stdin=subprocess.DEVNULL,
               stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
            # Parent must drop its copy before waiting for HELLO: otherwise
            # a worker that exits early cannot produce EOF on the client.
            server.close()
            hello = self.client.recv(4096)
            if len(hello) != REP.size:
                raise AssertionError(("bad hello", hello, self.process.poll()))
            mag, ver, op, status, n, cap, value = REP.unpack(hello)
            assert (mag, ver, op, status, n, cap) == (MAGIC, VERSION, 0, 0, 0, 0)
            self.nonce = value
        except BaseException:
            # Startup failure must not retain the inherited IPC endpoint or
            # parent-side pipes. Never block on stderr.read() from a live worker.
            self.client.close()
            if self.process is not None:
                if self.process.poll() is None:
                    self.process.kill()
                self.process.communicate(timeout=5)
            raise
        finally:
            server.close()

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

    def close(self, expected_returncode=0, *, timeout=5):
        # Closing the connection makes the worker release all session handles.
        # communicate() drains BOTH pipes and closes their BufferedReaders.
        # wait() alone leaves them open and triggers ResourceWarning on GC.
        self.client.close()
        try:
            _, stderr = self.process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            # An unresponsive test worker cannot survive teardown or retain
            # physical leases. Still report failure, not a successful test.
            self.process.kill()
            self.process.communicate()
            raise AssertionError("FS4 broker did not exit after IPC close") from error
        assert self.process.stdout.closed and self.process.stderr.closed
        assert self.process.returncode == expected_returncode, (
            self.process.returncode, expected_returncode,
            stderr.decode("utf-8", errors="replace"))

class FileSystem4Handles(unittest.TestCase):
    def test_absolute_session_expiry_releases_open_lease(self):
        # A test-only 1s absolute deadline confirms that expiration actually
        # revokes all physical leases, even when no CLOSE request is sent.
        with tempfile.TemporaryDirectory(prefix="fs4-broker-expiry-") as temp:
            root = Path(temp) / "C"
            root.mkdir(mode=0o700)
            (root / "expiry.dat").write_bytes(b"EXPIRING")
            first = Broker(root, expire=True)
            try:
                second = Broker(root)
            except BaseException:
                first.close()
                raise
            try:
                err, h, _, _ = first.call(
                    OPEN, b"C:/expiry.dat", access=0x80000000,
                    share=0, disposition=3)
                self.assertEqual(err, 0)
                self.assertNotEqual(h, 0)
                self.assertEqual(second.call(
                    OPEN, b"C:/expiry.dat", access=0x80000000,
                    share=7, disposition=3)[0], 32)
                self.assertEqual(first.process.wait(timeout=4), 0)
                self.assertEqual(first.client.recv(4096), b"")
                err, h2, _, _ = second.call(
                    OPEN, b"C:/expiry.dat", access=0x80000000,
                    share=7, disposition=3)
                self.assertEqual(err, 0, "expired worker kept OFD lease")
                self.assertEqual(second.call(CLOSE, handle=h2)[0], 0)
            finally:
                try:
                    first.close()
                finally:
                    second.close()

    def test_request_quota_counts_packets_not_idle_wakeups(self):
        with tempfile.TemporaryDirectory(prefix="fs4-broker-quota-") as temp:
            root = Path(temp) / "C"
            root.mkdir(mode=0o700)
            (root / "quota.dat").write_bytes(b"Q")
            first = Broker(root)
            try:
                second = Broker(root)
            except BaseException:
                first.close()
                raise
            try:
                err, h, _, _ = first.call(
                    OPEN, b"C:/quota.dat", access=0x80000000,
                    share=0, disposition=3)
                self.assertEqual(err, 0)
                for _ in range(255):
                    self.assertEqual(first.call(SIZE, handle=h)[0], 0)
                self.assertEqual(first.client.recv(4096), b"")
                self.assertEqual(first.process.wait(timeout=4), 0)
                err, h2, _, _ = second.call(
                    OPEN, b"C:/quota.dat", access=0x80000000,
                    share=7, disposition=3)
                self.assertEqual(err, 0, "quota worker kept OFD lease")
                self.assertEqual(second.call(CLOSE, handle=h2)[0], 0)
            finally:
                try:
                    first.close()
                finally:
                    second.close()

    def test_open_handle_survives_idle_poll_and_preserves_lease(self):
        # A 2-second poll timeout is NOT the 120-second session expiry.
        # A second broker must still see the exclusive physical OFD lease
        # while the first has sent no IPC traffic for more than 2 seconds.
        with tempfile.TemporaryDirectory(prefix="fs4-broker-idle-") as temp:
            root = Path(temp) / "C"
            root.mkdir(mode=0o700)
            (root / "idle.dat").write_bytes(b"PERSIST")
            first = Broker(root)
            try:
                second = Broker(root)
            except BaseException:
                first.close()
                raise
            try:
                error, handle, _, _ = first.call(
                    OPEN, b"C:/idle.dat", access=0x80000000,
                    share=0, disposition=3)
                self.assertEqual(error, 0)
                # Deliberately do not ping either worker.
                time.sleep(2.35)
                self.assertIsNone(first.process.poll(),
                                  "idle poll prematurely exited broker worker")
                self.assertEqual(second.call(
                    OPEN, b"C:/idle.dat", access=0x80000000,
                    share=7, disposition=3)[0], 32)
                self.assertEqual(first.call(READ, handle=handle, size=7),
                                 (0, 0, 7, b"PERSIST"))
                self.assertEqual(first.call(CLOSE, handle=handle)[0], 0)
                error, reopened, _, _ = second.call(
                    OPEN, b"C:/idle.dat", access=0x80000000,
                    share=7, disposition=3)
                self.assertEqual(error, 0)
                self.assertEqual(second.call(CLOSE, handle=reopened)[0], 0)
            finally:
                try:
                    first.close()
                finally:
                    second.close()

    def test_broker_clean_shutdown_closes_subprocess_streams(self):
        with tempfile.TemporaryDirectory(prefix="fs4-broker-lifetime-") as temp:
            root = Path(temp) / "C"
            root.mkdir(mode=0o700)
            broker = Broker(root)
            process = broker.process
            broker.close()
            self.assertEqual(process.returncode, 0)
            self.assertTrue(process.stdout.closed)
            self.assertTrue(process.stderr.closed)

    def test_failed_handshake_does_not_leak_parent_fds(self):
        # A deterministic negative handshake: /usr/bin/false exits without
        # HELLO. Closing the parent's server endpoint must expose EOF, rather
        # than hanging until the socket timeout.
        self.assertTrue(Path("/proc/self/fd").is_dir(),
                        "Linux /proc/self/fd required for FD accounting; no SKIP")
        with tempfile.TemporaryDirectory(prefix="fs4-broker-badhello-") as temp:
            root = Path(temp) / "C"
            root.mkdir(mode=0o700)
            before = len(os.listdir("/proc/self/fd"))
            with self.assertRaises(AssertionError):
                Broker(root, binary="/usr/bin/false")
            self.assertEqual(len(os.listdir("/proc/self/fd")), before)

    def test_broker_timeout_kills_worker_and_closes_pipes(self):
        with tempfile.TemporaryDirectory(prefix="fs4-broker-timeout-") as temp:
            root = Path(temp) / "C"
            root.mkdir(mode=0o700)
            broker = Broker(root)
            process = broker.process
            try:
                os.kill(process.pid, signal.SIGSTOP)
                with self.assertRaisesRegex(AssertionError, "did not exit"):
                    broker.close(timeout=0.2)
                self.assertEqual(process.returncode, -signal.SIGKILL)
                self.assertTrue(process.stdout.closed)
                self.assertTrue(process.stderr.closed)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate()

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
                self.assertEqual(b.call(SEEK, handle=h, offset=0, flags=0)[0], 0)
                self.assertEqual(b.call(READ, handle=h, size=5),
                                 (0, 0, 5, b"HELLO"))
                self.assertEqual(b.call(TRUNCATE, handle=h, offset=3)[0], 0)
                self.assertEqual(b.call(SIZE, handle=h)[2], 3)
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
            a = Broker(root)
            try:
                b = Broker(root)
            except BaseException:
                a.close()
                raise
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
                try:
                    a.close()
                finally:
                    b.close()

    def test_native_query_set_and_flush_through_service(self):
        with tempfile.TemporaryDirectory(prefix="fs4-native-") as temp:
            root = Path(temp) / "C"
            root.mkdir(mode=0o700)
            broker = Broker(root)
            try:
                err, handle, _, _ = broker.call(OPEN, b"C:/native.dat",
                                               access=0xC0000000,
                                               share=7, disposition=1)
                self.assertEqual(err, 0)
                self.assertEqual(broker.call(WRITE, b"ABCDEFG",
                                             handle=handle)[0], 0)
                err, _, status, data = broker.call(
                    NATIVE_QUERY, handle=handle, access=5, size=24)
                self.assertEqual((err, status, len(data)), (0, 0, 24))
                self.assertEqual(struct.unpack_from("<q", data, 8)[0], 7)
                self.assertEqual(broker.call(NATIVE_SET,
                    struct.pack("<q", 2), handle=handle, access=14)[2], 0)
                err, _, status, data = broker.call(
                    NATIVE_QUERY, handle=handle, access=14, size=8)
                self.assertEqual((err, status, struct.unpack("<q", data)[0]),
                                 (0, 0, 2))
                self.assertEqual(broker.call(NATIVE_SET,
                    struct.pack("<q", 4), handle=handle, access=20)[2], 0)
                self.assertEqual(broker.call(NATIVE_FLUSH,
                                             handle=handle)[2], 0)
                self.assertEqual(broker.call(CLOSE, handle=handle)[0], 0)
                self.assertEqual((root/"native.dat").read_bytes(), b"ABCD")
            finally:
                broker.close()

    def test_sigkill_releases_remote_share_lease(self):
        with tempfile.TemporaryDirectory(prefix="fs4-kill-lease-") as temp:
            root = Path(temp) / "C"
            root.mkdir(mode=0o700)
            (root / "lease.dat").write_bytes(b"SAFE")
            first = Broker(root)
            try:
                second = Broker(root)
            except BaseException:
                first.close()
                raise
            killed = False
            try:
                err, handle, _, _ = first.call(OPEN, b"C:/lease.dat",
                                                access=0x80000000,
                                                share=0, disposition=3)
                self.assertEqual(err, 0)
                self.assertEqual(second.call(OPEN, b"C:/lease.dat",
                    access=0x80000000, share=7, disposition=3)[0], 32)
                os.kill(first.process.pid, signal.SIGKILL)
                killed = True
                self.assertEqual(first.process.wait(timeout=5),
                                 -signal.SIGKILL)
                self.assertNotEqual(first.process.returncode, 0)
                err, available, _, _ = second.call(
                    OPEN, b"C:/lease.dat", access=0x80000000,
                    share=7, disposition=3)
                self.assertEqual(err, 0, "SIGKILL leaked physical OFD lease")
                self.assertEqual(second.call(CLOSE, handle=available)[0], 0)
                self.assertEqual((root / "lease.dat").read_bytes(), b"SAFE")
            finally:
                try:
                    first.close(expected_returncode=-signal.SIGKILL if killed else 0)
                finally:
                    second.close()

    def test_concurrent_create_new_is_single_winner(self):
        with tempfile.TemporaryDirectory(prefix="fs4-create-race-") as temp:
            root = Path(temp) / "C"
            root.mkdir(mode=0o700)
            first = Broker(root)
            try:
                second = Broker(root)
            except BaseException:
                first.close()
                raise
            start = threading.Barrier(3)
            results = []
            def create(client):
                start.wait(timeout=5)
                results.append((client, client.call(
                    OPEN, b"C:/contended.bin", access=0xC0000000,
                    share=7, disposition=1)))
            try:
                a = threading.Thread(target=create, args=(first,))
                b = threading.Thread(target=create, args=(second,))
                a.start(); b.start(); start.wait(timeout=5)
                a.join(timeout=8); b.join(timeout=8)
                self.assertFalse(a.is_alive() or b.is_alive(),
                                 "stalled physical WAL/lease lock ordering")
                self.assertEqual(len(results), 2)
                self.assertEqual(sum(reply[0] == 0 for _, reply in results), 1)
                for broker, reply in results:
                    if reply[0] == 0:
                        self.assertEqual(broker.call(CLOSE,
                                                     handle=reply[1])[0], 0)
                self.assertTrue((root / "contended.bin").is_file())
            finally:
                try:
                    first.close()
                finally:
                    second.close()

    def test_sigkill_during_broker_mutations_and_recovery(self):
        """Process death after the syscall but before reply must not leak
        WAL or share leases. This is NOT physical power-loss durability."""
        for phase in ("after-open", "after-write", "after-rename",
                      "after-delete", "after-attribute", "after-native-set"):
            with self.subTest(phase=phase), tempfile.TemporaryDirectory(
                    prefix="fs4-broker-crash-") as temp:
                root = Path(temp) / "C"
                root.mkdir(mode=0o700)
                if phase in ("after-write", "after-native-set"):
                    (root / "victim.bin").write_bytes(b"ORIGINAL")
                elif phase == "after-rename":
                    (root / "victim.bin").write_bytes(b"RENAMED")
                elif phase in ("after-delete", "after-attribute"):
                    (root / "victim.bin").write_bytes(b"KEEP")
                b = Broker(root, crash=phase)
                try:
                    # Send one successful destructive request to the worker.
                    if phase == "after-open":
                        req = (OPEN, b"C:/victim.bin", dict(access=0xC0000000,
                                                          share=7, disposition=1))
                    elif phase in ("after-write", "after-native-set"):
                        err, h, _, _ = b.call(OPEN, b"C:/victim.bin",
                                              access=0xC0000000, share=7,
                                              disposition=3)
                        self.assertEqual(err, 0)
                        req = ((WRITE, b"RESULT", dict(handle=h))
                               if phase == "after-write" else
                               (NATIVE_SET, struct.pack("<q", 3),
                                dict(handle=h, access=20)))
                    elif phase == "after-rename":
                        req = (RENAME, b"C:/victim.bin\0C:/renamed.bin", {})
                    elif phase == "after-delete":
                        req = (DELETE, b"C:/victim.bin", {})
                    else:
                        req = (ATTR_SET, b"C:/victim.bin", dict(flags=1))
                    with self.assertRaises(AssertionError):
                        b.call(req[0], req[1], **req[2])
                    self.assertEqual(b.process.wait(timeout=5),
                                     -signal.SIGKILL)
                finally:
                    # The death is intentional; reap and close parent pipes
                    # without ever weakening the cold-restart WAL test.
                    b.close(expected_returncode=-signal.SIGKILL)
                # A cold worker re-runs startup recovery under the same root.
                recovered = Broker(root)
                try:
                    if phase == "after-open":
                        self.assertTrue((root / "victim.bin").exists())
                    elif phase == "after-write":
                        self.assertEqual((root / "victim.bin").read_bytes(),
                                         b"RESULTAL")
                    elif phase == "after-native-set":
                        self.assertEqual((root / "victim.bin").read_bytes(), b"ORI")
                    elif phase == "after-rename":
                        self.assertFalse((root / "victim.bin").exists())
                        self.assertEqual((root / "renamed.bin").read_bytes(),
                                         b"RENAMED")
                    elif phase == "after-delete":
                        self.assertFalse((root / "victim.bin").exists())
                    elif phase == "after-attribute":
                        err, _, _, attrs = recovered.call(
                            ATTR_GET, b"C:/victim.bin")
                        self.assertEqual(err, 0)
                        self.assertEqual(struct.unpack_from("<I", attrs)[0] & 1, 1)
                    path = b"C:/renamed.bin" if phase == "after-rename" else b"C:/victim.bin"
                    if phase != "after-delete":
                        err, cap, _, _ = recovered.call(
                            OPEN, path, access=0x80000000, share=7,
                            disposition=3)
                        self.assertEqual(err, 0, "orphaned OFD lease")
                        self.assertEqual(recovered.call(CLOSE, handle=cap)[0], 0)
                finally:
                    recovered.close()

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
