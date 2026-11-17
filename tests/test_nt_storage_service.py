#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""FileSystem4 dedicated-UID service: executable IPC/permission regressions."""
import importlib.util
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("nt_storage_service",
                                             ROOT / "scripts/nt-storage-service.py")
service = importlib.util.module_from_spec(spec)
spec.loader.exec_module(service)


class Security(unittest.TestCase):
    def test_root_and_identical_uid_denied(self):
        with mock.patch.object(service.os, "geteuid", return_value=0):
            with self.assertRaises(service.Refused):
                service.service(Path("/tmp"), Path("/tmp"), client_uid=1001,
                                client_gid=1001, fuse="x", mediator="x", unmount="x")
        with self.assertRaises(service.Refused):
            service.service(Path("/tmp"), Path("/tmp"),
                            client_uid=os.geteuid(), client_gid=1001,
                            fuse="x", mediator="x", unmount="x")

    def test_untrusted_names(self):
        for path in ("D:/a", "C:", "C:/", "C:/../other", "C:/a/./b",
                     "C:/a//b", "C:/a\\b", "C:/a:b", "C:/a\x00",
                     "C:/bad\n", "C:/a/", "C:/" + "a" * 1500, 5, None):
            with self.subTest(path=repr(path)), self.assertRaises(service.Refused):
                service.guest_path(path)
        self.assertEqual(service.guest_path("C:/Some Name/file.txt"),
                         "C:/Some Name/file.txt")

    def test_protocol_rename_uses_only_broker(self):
        with mock.patch.object(service.subprocess, "run") as mocked:
            mocked.return_value.returncode = 0
            request = {"version": 1, "op": "rename", "from": "C:/one", "to": "C:/two"}
            result = service.dispatch(request, mediator="/trusted/broker",
                                      backing=Path("/secret"), live_mount=True)
            self.assertEqual(result["ok"], True)
            self.assertEqual(mocked.call_args.args[0],
                             ["/trusted/broker", "--root", "/secret", "--move",
                              "C:/one", "C:/two"])
            for invalid in (
                {"version": 2, "op": "status"},
                {"version": 1, "op": "write"},
                dict(request, uid=1000),
                dict(request, **{"from": "C:/../../escape"}),
            ):
                with self.assertRaises(service.Refused):
                    service.dispatch(invalid, mediator="x", backing=Path("/"),
                                     live_mount=True)
            with self.assertRaises(service.Refused):
                service.dispatch(request, mediator="x", backing=Path("/"),
                                 live_mount=False)
            mocked.return_value.returncode = 3
            with self.assertRaises(service.Refused):
                service.dispatch(request, mediator="x", backing=Path("/"),
                                 live_mount=True)

    def test_backing_ownership_and_symlinks(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "root"
            root.mkdir(mode=0o700)
            self.assertEqual(service.private_directory(root, os.geteuid()), root)
            root.chmod(0o755)
            with self.assertRaises(service.Refused):
                service.private_directory(root, os.geteuid())
            root.chmod(0o700)
            alias = Path(temp) / "symlink"
            alias.symlink_to(root)
            with self.assertRaises(service.Refused):
                service.private_directory(alias, os.geteuid())

    def test_real_unix_kernel_peer_credentials(self):
        with socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET) as (server, client):
            client.sendall(b'{"version":1,"op":"status"}')
            self.assertEqual(service.peer_identity(server)[1], os.geteuid())
            service.serve_one(server, client_uid=os.geteuid(), mediator="unused",
                              backing=Path("/"), ready=True)
            self.assertTrue(json.loads(client.recv(4096))["mediated"])
        with socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET) as (server, client):
            # A different UID gets no data or response at all.
            service.serve_one(server, client_uid=os.geteuid() + 1,
                              mediator="unused", backing=Path("/"), ready=True)
            client.settimeout(1)
            with self.assertRaises(TimeoutError):
                client.recv(4096)

    def test_bad_packet_rejected(self):
        for packet in (b"", b"this is not JSON", b"{" + b"a" * 5000):
            with socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET) as (server, client):
                if packet:
                    client.sendall(packet)
                else:
                    client.shutdown(socket.SHUT_WR)
                service.serve_one(server, client_uid=os.geteuid(),
                                  mediator="unused", backing=Path("/"), ready=True)
                self.assertIs(json.loads(client.recv(4096))["ok"], False)

    def test_real_other_uid_cannot_access_0700_backing(self):
        if os.geteuid() != 0:
            self.skipTest("Needs root CI to switch to a real distinct UID")
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "backing"
            root.mkdir(mode=0o700)
            (root / "file").write_bytes(b"private")
            def lower_identity():
                os.setgroups([])
                os.setgid(65534)
                os.setuid(65534)
            result = subprocess.run(
                [sys.executable, "-c",
                 "from pathlib import Path; import sys; Path(sys.argv[1]).read_bytes()",
                 str(root / "file")], preexec_fn=lower_identity,
                capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"PermissionError", result.stderr)

    def test_every_service_fuse_callback_checks_kernel_uid(self):
        code = (ROOT / "src/tools/dos83_fuse.c").read_text(encoding="utf-8")
        callbacks = ("getattr", "opendir", "readdir", "releasedir", "open",
                     "create", "read", "write", "release", "flush", "fsync",
                     "truncate", "mkdir", "mknod", "unlink", "rmdir", "rename",
                     "link", "statfs", "chmod", "utimens", "getxattr",
                     "setxattr", "removexattr", "listxattr")
        for name in callbacks:
            pos = code.index("static int fs_" + name + "(")
            brace = code.index("{", pos)
            self.assertIn("!service_authorized()", code[brace:brace + 120], name)
        self.assertIn("if (service_mode)\n        return -EPERM;", code)
        self.assertIn("default_permissions,allow_other", code)


if __name__ == "__main__":
    unittest.main()
