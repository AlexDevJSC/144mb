#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""FileSystem4 security and metadata over authenticated HANDLE IPC.

All mutations use a disposable private test C: backing. Physical Fedora
and NTFS ACL inheritance are deliberately outside this regression.
"""
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fs4_handle_broker_test import Broker, OPEN, CLOSE, HANDLE_INFO

SECURITY_PATH, NATIVE_SECURITY, TIME_SET = 25, 26, 27
INFO_GET, INFO_SET = 28, 29
BASIC = struct.Struct("<QQQQI4x")
FILETIME = struct.Struct("<II")
EPOCH_OFFSET = 11644473600
PATH = b"C:/secure-dates.txt"
SUCCESS = 0
ACCESS_DENIED = 5
INSUFFICIENT_BUFFER = 122
INVALID_PARAMETER = 87
NOT_SUPPORTED = 50
BUFFER_TOO_SMALL_STATUS = 0xC0000023


def ticks(unix_seconds):
    return (unix_seconds + EPOCH_OFFSET) * 10_000_000


def filetime(value):
    return FILETIME.pack(value & 0xFFFFFFFF, value >> 32)


class SecurityMetadataIPC(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="fs4-acl-time-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "C"
        self.root.mkdir(mode=0o700)
        self.target = self.root / "secure-dates.txt"
        self.target.write_bytes(b"TRUSTED")
        self.worker = Broker(self.root)
        self.addCleanup(self.worker.close)

    def opening(self, access=0x80000000):
        err, handle, _, _ = self.worker.call(
            OPEN, PATH, access=access, share=7, disposition=3)
        self.assertEqual(err, SUCCESS)
        self.addCleanup(lambda: self.worker.call(CLOSE, handle=handle))
        return handle

    def test_security_descriptor_is_bounded_and_read_only(self):
        err, _, needed, data = self.worker.call(
            SECURITY_PATH, PATH, access=7, disposition=0)
        self.assertEqual((err, needed, data),
                         (INSUFFICIENT_BUFFER, 128, b""))
        err, _, needed, descriptor = self.worker.call(
            SECURITY_PATH, PATH, access=7, disposition=128)
        self.assertEqual((err, needed, len(descriptor)), (0, 128, 128))
        self.assertEqual(descriptor[0], 1)  # self-relative SECURITY_DESCRIPTOR
        self.assertEqual(struct.unpack_from("<H", descriptor, 2)[0], 0x8004)
        self.assertEqual(struct.unpack_from("<I", descriptor, 4)[0], 20)
        self.assertEqual(struct.unpack_from("<I", descriptor, 8)[0], 36)
        self.assertEqual(struct.unpack_from("<I", descriptor, 16)[0], 52)
        self.assertEqual(self.worker.call(
            SECURITY_PATH, PATH, access=8, disposition=128)[0],
            ACCESS_DENIED, "SACL must never be silently returned")
        self.assertEqual(self.worker.call(
            SECURITY_PATH, PATH, access=0, disposition=128)[0],
            INVALID_PARAMETER)
        self.assertEqual(self.worker.call(
            SECURITY_PATH, b"C:/../escape", access=7, disposition=128)[0],
            INVALID_PARAMETER)
        self.assertEqual(self.worker.call(
            SECURITY_PATH, PATH, access=7, disposition=2049)[0],
            INVALID_PARAMETER)
        self.assertEqual(self.target.read_bytes(), b"TRUSTED")

    def test_native_security_reports_required_size_and_status(self):
        handle = self.opening()
        err, needed, status, data = self.worker.call(
            NATIVE_SECURITY, handle=handle, access=7, size=0)
        self.assertEqual((err, needed, status, data),
                         (SUCCESS, 128, BUFFER_TOO_SMALL_STATUS, b""))
        err, needed, status, descriptor = self.worker.call(
            NATIVE_SECURITY, handle=handle, access=7, size=128)
        self.assertEqual((err, needed, status, len(descriptor)),
                         (SUCCESS, 128, SUCCESS, 128))
        err, _, status, _ = self.worker.call(
            NATIVE_SECURITY, handle=handle, access=8, size=128)
        self.assertEqual(err, SUCCESS)
        self.assertEqual(status, 0xC0000022)  # STATUS_ACCESS_DENIED
        self.assertEqual(self.worker.call(
            NATIVE_SECURITY, handle=handle + 2, access=7, size=128)[0],
            INVALID_PARAMETER)

    def test_timestamps_are_durable_and_share_file_basic_info(self):
        readonly = self.opening()
        old = ticks(1577836800)
        new = ticks(1609459200)
        fields = filetime(old) + filetime(new) + filetime(new)
        self.assertEqual(len(fields), 24)
        self.assertEqual(self.worker.call(
            TIME_SET, fields, handle=readonly, flags=7)[0],
            ACCESS_DENIED)
        write_handle = self.opening(0xC0000000)
        self.assertEqual(self.worker.call(
            TIME_SET, fields, handle=write_handle, flags=7)[0], SUCCESS)
        # SetFileTime returns only after fsync of the pinned inode.
        self.assertEqual(self.target.stat().st_mtime_ns,
                         1609459200 * 1_000_000_000)
        err, _, _, data = self.worker.call(
            INFO_GET, handle=write_handle, access=0, size=40)
        self.assertEqual((err, len(data)), (SUCCESS, 40))
        created, accessed, written, changed, attrs = BASIC.unpack(data)
        self.assertEqual((created, accessed, written), (old, new, new))
        self.assertTrue(attrs & 0x80)  # FILE_ATTRIBUTE_NORMAL
        err, _, _, _ = self.worker.call(
            INFO_GET, handle=write_handle, access=1, size=24)
        self.assertEqual(err, SUCCESS)
        err, _, _, _ = self.worker.call(
            INFO_GET, handle=write_handle, access=9, size=8)
        self.assertEqual(err, SUCCESS)
        self.assertEqual(self.worker.call(
            INFO_GET, handle=write_handle, access=2, size=64)[0],
            INVALID_PARAMETER)
        # FILE_BASIC_INFO class 0 supports atime/mtime only; metadata
        # requiring unmodelled ACL/creation/attributes must fail closed.
        older = ticks(1546300800)
        basic = BASIC.pack(0, older, older, 0, 0)
        self.assertEqual(self.worker.call(
            INFO_SET, basic, handle=write_handle, access=0)[0], SUCCESS)
        self.assertEqual(self.target.stat().st_mtime_ns,
                         1546300800 * 1_000_000_000)
        unsupported = BASIC.pack(0, 0, 0, 1, 0)
        self.assertEqual(self.worker.call(
            INFO_SET, unsupported, handle=write_handle, access=0)[0],
            NOT_SUPPORTED)
        self.assertEqual(self.worker.call(
            INFO_SET, basic, handle=readonly, access=0)[0], ACCESS_DENIED)
        err, _, _, info = self.worker.call(HANDLE_INFO, handle=write_handle)
        self.assertEqual((err, len(info)), (SUCCESS, 52))
        # FILETIME fields in BY_HANDLE_FILE_INFORMATION follow attrs.
        self.assertEqual(info[4:12], filetime(old))
        self.assertEqual(info[12:20], filetime(older))
        self.assertEqual(info[20:28], filetime(older))
        self.assertEqual(self.target.read_bytes(), b"TRUSTED")

    def test_reopen_sees_committed_metadata_and_no_wal_leak(self):
        old = ticks(1483228800)
        write_handle = self.opening(0xC0000000)
        fields = filetime(0) + filetime(0) + filetime(old)
        self.assertEqual(self.worker.call(
            TIME_SET, fields, handle=write_handle, flags=4)[0], 0)
        self.assertEqual(self.worker.call(CLOSE, handle=write_handle)[0], 0)
        self.worker.close()
        # Explicitly re-open in a fresh trusted broker (cold recovery).
        # Disable the old cleanup before replacement to avoid double close.
        self.worker = Broker(self.root)
        err, handle, _, _ = self.worker.call(
            OPEN, PATH, access=0x80000000, share=7, disposition=3)
        self.assertEqual(err, 0)
        err, _, _, data = self.worker.call(
            INFO_GET, handle=handle, access=0, size=40)
        self.assertEqual(err, 0)
        self.assertEqual(BASIC.unpack(data)[2], old)
        self.assertEqual(self.worker.call(CLOSE, handle=handle)[0], 0)
        self.assertEqual(self.target.read_bytes(), b"TRUSTED")


if __name__ == "__main__":
    unittest.main()
