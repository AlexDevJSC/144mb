#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual authenticated broker access checks on pinned inode DACL xattrs.

The xattr is planted by the trusted *host fixture*, never by a guest RPC.
It tests allow/deny at the boundary, not Windows SID-domain impersonation.
"""
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fs4_handle_broker_test import (
    Broker, OPEN, READ, WRITE, DELETE, RENAME, ATTR_SET, HARDLINK,
    CLOSE,
)

SECURITY = "user.ntnative.security.sd"
READ_GENERIC = 0x80000000
WRITE_GENERIC = 0x40000000
GENERIC_READ_MASK = 0x00120089
GENERIC_WRITE_MASK = 0x00120116
DENIED = 5
NOT_SUPPORTED = 50

def sid(auth, *subs):
    return bytes((1, len(subs))) + auth.to_bytes(6, "big") + b"".join(
        struct.pack("<I", s) for s in subs
    )

EVERYONE = sid(1, 0)

def descriptor(aces, owner=None, group=None):
    owner = owner or sid(22, 1, os.getuid())
    group = group or sid(22, 2, os.getgid())
    acl = bytearray(b"\x02\x00\x00\x00\x00\x00\x00\x00")
    for kind, flags, mask, principal in aces:
        ace = struct.pack("<BBHI", kind, flags, 8 + len(principal), mask) + principal
        acl.extend(ace)
    struct.pack_into("<HH", acl, 2, len(acl), len(aces))
    result = bytearray(20)
    result[0] = 1
    struct.pack_into("<H", result, 2, 0x8004)
    struct.pack_into("<I", result, 4, len(result))
    result.extend(owner)
    struct.pack_into("<I", result, 8, len(result))
    result.extend(group)
    struct.pack_into("<I", result, 16, len(result))
    result.extend(acl)
    return bytes(result)

def record(file: Path, sd: bytes):
    stat = file.stat()
    first = b"NTSD0001" + struct.pack("<IIQQ", len(sd), 0, stat.st_dev, stat.st_ino)
    checksum = 14695981039346656037
    for value in first[8:32] + sd:
        checksum = ((checksum ^ value) * 1099511628211) & ((1 << 64) - 1)
    return first + struct.pack("<Q", checksum) + sd

def grant(file, aces):
    sd = descriptor(aces)
    os.setxattr(file, SECURITY, record(file, sd))

class BrokerACL(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="fs4-broker-dacl-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "C"
        self.root.mkdir(mode=0o700)

    def test_managed_dacl_is_enforced_for_open_write_attributes_and_delete(self):
        data = self.root / "policy.dat"
        data.write_bytes(b"ACL-PROTECTED")
        grant(data, [
            (1, 0, GENERIC_WRITE_MASK | 0x00010000, EVERYONE),
            (0, 0, GENERIC_READ_MASK, EVERYONE),
        ])
        os.link(data, self.root / "alias.dat")
        broker = Broker(self.root)
        try:
            err, handle, _, _ = broker.call(
                OPEN, b"C:/policy.dat", access=READ_GENERIC,
                disposition=3, share=7)
            self.assertEqual(err, 0)
            self.assertEqual(broker.call(READ, handle=handle, size=13),
                             (0, 0, 13, b"ACL-PROTECTED"))
            self.assertEqual(broker.call(CLOSE, handle=handle)[0], 0)
            for path in (b"C:/policy.dat", b"C:/alias.dat"):
                self.assertEqual(broker.call(
                    OPEN, path, access=WRITE_GENERIC,
                    disposition=3, share=7)[0], DENIED)
                self.assertEqual(broker.call(DELETE, path)[0], DENIED)
            self.assertEqual(broker.call(
                ATTR_SET, b"C:/policy.dat", flags=1)[0], DENIED)
            self.assertEqual(broker.call(
                RENAME, b"C:/policy.dat\x00C:/renamed.dat", flags=1)[0], DENIED)
            self.assertEqual(data.read_bytes(), b"ACL-PROTECTED")
            self.assertFalse((self.root / "renamed.dat").exists())
            # Failing an ACL must not invalidate the same-inode source.
            self.assertEqual(os.stat(data).st_ino,
                             os.stat(self.root / "alias.dat").st_ino)
        finally:
            broker.close()

    def test_unmanaged_dacl_parent_cannot_publish_uninherited_child(self):
        parent = self.root / "managed"
        parent.mkdir()
        grant(parent, [
            (0, 3, GENERIC_READ_MASK | GENERIC_WRITE_MASK, EVERYONE),
        ])
        broker = Broker(self.root)
        try:
            self.assertEqual(broker.call(
                OPEN, b"C:/managed/unsafe.txt", access=WRITE_GENERIC,
                share=7, disposition=1)[0], NOT_SUPPORTED)
            self.assertFalse((parent / "unsafe.txt").exists())
        finally:
            broker.close()

    def test_corrupt_acl_fails_closed_before_mutation(self):
        file = self.root / "corrupt.dat"
        file.write_bytes(b"SAFE")
        grant(file, [(0, 0, GENERIC_READ_MASK | GENERIC_WRITE_MASK, EVERYONE)])
        payload = bytearray(os.getxattr(file, SECURITY))
        payload[-1] ^= 0x80
        os.setxattr(file, SECURITY, payload)
        broker = Broker(self.root)
        try:
            self.assertEqual(broker.call(
                OPEN, b"C:/corrupt.dat", access=READ_GENERIC,
                share=7, disposition=3)[0], DENIED)
            self.assertEqual(broker.call(
                ATTR_SET, b"C:/corrupt.dat", flags=1)[0], DENIED)
            self.assertEqual(broker.call(DELETE, b"C:/corrupt.dat")[0], DENIED)
            self.assertEqual(file.read_bytes(), b"SAFE")
        finally:
            broker.close()

if __name__ == "__main__":
    unittest.main()
