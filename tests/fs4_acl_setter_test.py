#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual authenticated broker DACL path/HANDLE writer and inode checks.

These offline tests exercise IPC + WAL + xattr durability, not a domain
logon token or a physical Fedora/SELinux test.
"""
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fs4_handle_broker_test import Broker, OPEN, CLOSE, DUPLICATE
from fs4_acl_enforcement_test import descriptor, sid

SET_PATH, SET_HANDLE, GET_PATH = 30, 31, 25
DACL_INFORMATION = 4
READ = 0x80000000
WRITE = 0x40000000
WRITE_DAC = 0x00040000
ERR_ACCESS = 5
ERR_UNSUPPORTED = 50
ERR_INVALID = 87
EVERYONE = sid(1, 0)
OWNER = sid(22, 1, os.geteuid())
GROUP = sid(22, 2, os.getegid())
READ_MASK = 0x00120089
WRITE_MASK = 0x00120116


class BrokerDACLWriter(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="fs4-ntacl-write-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "C"
        self.root.mkdir(mode=0o700)
        self.file = self.root / "secured.dat"
        self.file.write_bytes(b"UNMODIFIED")
        os.link(self.file, self.root / "alias.dat")
        self.worker = Broker(self.root)
        self.addCleanup(self.worker.close)

    def path_write(self, sd, path=b"C:/secured.dat", info=4, **overrides):
        payload = path + sd
        return self.worker.call(SET_PATH, payload, access=info,
                                share=len(path), disposition=len(sd),
                                **overrides)

    def read_dacl(self, path=b"C:/secured.dat"):
        error, _, needed, sd = self.worker.call(
            GET_PATH, path, access=7, disposition=2048)
        self.assertEqual(error, 0)
        self.assertEqual(needed, len(sd))
        return sd

    def test_bootstrap_unmanaged_sd_does_not_trust_guest_owner(self):
        attacker_owner = sid(22, 1, 31337)
        new = descriptor([
            (0, 0, READ_MASK, EVERYONE),
            (1, 0, WRITE_MASK, EVERYONE),
        ], owner=attacker_owner)
        self.assertEqual(self.path_write(new)[0], 0)
        saved = self.read_dacl()
        off = struct.unpack_from("<I", saved, 4)[0]
        self.assertEqual(saved[off:off+len(OWNER)], OWNER,
                         "guest owner SID must not be installed")
        self.assertEqual(self.worker.call(OPEN, b"C:/secured.dat",
                             access=READ, share=7, disposition=3)[0], 0)
        for pathname in (b"C:/secured.dat", b"C:/alias.dat"):
            self.assertEqual(self.worker.call(OPEN, pathname,
                                 access=WRITE, share=7, disposition=3)[0],
                             ERR_ACCESS)
        self.assertEqual(self.file.read_bytes(), b"UNMODIFIED")
        self.assertEqual(self.file.stat().st_ino,
                         (self.root / "alias.dat").stat().st_ino)

    def test_handle_security_access_is_not_inherited_from_read_handle(self):
        initial = descriptor([(0, 0, READ_MASK | WRITE_MASK | WRITE_DAC, OWNER)])
        self.assertEqual(self.path_write(initial)[0], 0)
        err, readonly, _, _ = self.worker.call(
            OPEN, b"C:/secured.dat", access=READ,
            share=7, disposition=3)
        self.assertEqual(err, 0)
        replacement = descriptor([(0, 0, READ_MASK, EVERYONE)])
        self.assertEqual(self.worker.call(
            SET_HANDLE, replacement, handle=readonly,
            access=DACL_INFORMATION)[0], ERR_ACCESS)
        self.assertEqual(self.worker.call(CLOSE, handle=readonly)[0], 0)
        # Native OPEN with WRITE_DAC only has no implicit DATA_READ/WRITE.
        err, rights, _, _ = self.worker.call(
            OPEN, b"C:/secured.dat", access=WRITE_DAC,
            share=7, disposition=3)
        self.assertEqual(err, 0)
        err, cloned, _, _ = self.worker.call(
            DUPLICATE, handle=rights, flags=2)
        self.assertEqual(err, 0)
        self.assertEqual(self.worker.call(
            SET_HANDLE, replacement, handle=cloned,
            access=DACL_INFORMATION)[0], 0)
        self.assertEqual(self.worker.call(CLOSE, handle=cloned)[0], 0)
        self.assertEqual(self.worker.call(CLOSE, handle=rights)[0], 0)
        saved = self.read_dacl()
        offset = struct.unpack_from("<I", saved, 16)[0]
        self.assertEqual(saved[offset+8], 0)

    def test_malformed_and_unauthorized_write_cannot_mutate_xattr(self):
        valid = descriptor([(0, 0, READ_MASK | WRITE_MASK, OWNER)])
        self.assertEqual(self.path_write(valid)[0], 0)
        old = os.getxattr(self.file, "user.ntnative.security.sd")
        # Malformed NULL-DACL: never grant all access.
        wrong = bytearray(valid)
        struct.pack_into("<I", wrong, 16, 0)
        self.assertNotEqual(self.path_write(wrong)[0], 0)
        self.assertEqual(self.path_write(valid, info=8)[0], ERR_INVALID)
        self.assertEqual(os.getxattr(self.file, "user.ntnative.security.sd"), old)
        # Size overflowing FS4_MAX must be rejected by the wire contract.
        self.assertEqual(self.worker.call(SET_PATH, b"C:/secured.dat" + b"0"*40,
                             access=4, share=14, disposition=0xffffffff,
                             size=2)[0], ERR_INVALID)
        self.assertEqual(os.getxattr(self.file, "user.ntnative.security.sd"), old)


if __name__ == "__main__":
    unittest.main()
