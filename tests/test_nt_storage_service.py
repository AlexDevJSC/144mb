#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""FileSystem4 dedicated-UID service: executable IPC/permission regressions."""
import importlib.util
from contextlib import contextmanager
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
# importlib loads the service by file location: add the sibling directory
# explicitly, as Python does when the installed script executes normally.
sys.path.insert(0, str(ROOT / "scripts"))
spec = importlib.util.spec_from_file_location("nt_storage_service",
                                             ROOT / "scripts/nt-storage-service.py")
service = importlib.util.module_from_spec(spec)
spec.loader.exec_module(service)


@contextmanager
def socket_pair():
    server, client = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    try:
        with server, client:
            yield server, client
    finally:
        pass


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
        with socket_pair() as (server, client):
            client.sendall(b'{"version":1,"op":"status"}')
            self.assertEqual(service.peer_identity(server)[1], os.geteuid())
            service.serve_one(server, client_uid=os.geteuid(), mediator="unused",
                              backing=Path("/"), ready=True)
            self.assertTrue(json.loads(client.recv(4096))["mediated"])
        with socket_pair() as (server, client):
            # A different UID gets no data or response at all.
            service.serve_one(server, client_uid=os.geteuid() + 1,
                              mediator="unused", backing=Path("/"), ready=True)
            client.settimeout(1)
            with self.assertRaises(TimeoutError):
                client.recv(4096)

    def test_v2_real_socket_auth_and_handle_scope(self):
        """Test v2 through serve_one, not just the session helper."""
        import threading
        import base64
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "C"
            root.mkdir(mode=0o700)
            (root / "safe.bin").write_bytes(b"UID SERVICE")
            with socket_pair() as (server, client):
                client.sendall(b'{"version":2,"op":"session"}')
                faults = []
                def serve():
                    try:
                        service.serve_one(server, client_uid=os.geteuid(),
                                          mediator="unused", backing=root, ready=True)
                    except BaseException as exc:
                        faults.append(exc)
                thread = threading.Thread(target=serve, daemon=True)
                thread.start()
                welcome = json.loads(client.recv(4096))
                self.assertTrue(welcome["readonly"])
                sess = welcome["session"]
                def ask(operation, **kwargs):
                    payload = dict(version=2, session=sess, op=operation, **kwargs)
                    client.sendall(json.dumps(payload).encode("utf-8"))
                    return json.loads(client.recv(4096))
                opened = ask("open", path="C:/safe.bin")
                self.assertTrue(opened["ok"])
                value = ask("read", handle=opened["handle"], offset=0, size=11)
                self.assertEqual(base64.b64decode(value["data_b64"]), b"UID SERVICE")
                self.assertFalse(ask("write", handle=opened["handle"],
                                     data_b64="WA==")["ok"])
                self.assertTrue(ask("end")["ok"])
                thread.join(timeout=3)
                self.assertFalse(thread.is_alive())
                self.assertFalse(faults)

            with socket_pair() as (server, client):
                client.sendall(b'{"version":2,"op":"session"}')
                service.serve_one(server, client_uid=os.geteuid() + 1,
                                  mediator="unused", backing=root, ready=True)
                client.settimeout(1)
                with self.assertRaises(TimeoutError):
                    client.recv(4096)

    def test_bad_packet_rejected(self):
        for packet in (b"", b"this is not JSON", b"{" + b"a" * 5000):
            with socket_pair() as (server, client):
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

    def test_systemd_root_listener_and_nonroot_reply_sender(self):
        """Reproduce a real systemd socket, creator root, acceptor nobody.

        SO_PEERCRED on the client side reports the creator, NOT the daemon;
        SCM_CREDENTIALS on replies must identify the daemon's true UID.
        """
        if os.geteuid() != 0:
            self.skipTest("Needs root to create a root-owned listener then drop UID")
        with tempfile.TemporaryDirectory() as temp:
            os.chmod(temp, 0o777)
            endpoint = str(Path(temp) / "activated.sock")
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as listener:
                listener.bind(endpoint)
                os.chmod(endpoint, 0o777)
                listener.listen(2)
                child = os.fork()
                if child == 0:
                    try:
                        os.setgroups([])
                        os.setgid(65534)
                        os.setuid(65534)
                        for _ in range(3):
                            conn, _addr = listener.accept()
                            with conn:
                                # First connection only probes SO_PEERCRED,
                                # then closes. The other two request replies.
                                if conn.recv(4096):
                                    conn.sendall(b'{"version":1,"ok":true,"mediated":true}')
                        os._exit(0)
                    except BaseException:
                        os._exit(55)
                try:
                    # Real peer UID at connection is PID1/root, so authenticating
                    # using SO_PEERCRED would fail despite the correct service.
                    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as probe:
                        probe.connect(endpoint)
                        creator = service.peer_identity(probe)[1]
                        self.assertEqual(creator, 0)
                    # Two accepted requests from the nonroot service, not the
                    # listener creator. Test both client tools.
                    import importlib.util
                    spec = importlib.util.spec_from_file_location(
                        "fs4_control", ROOT / "scripts/nt-storage-control.py")
                    control = importlib.util.module_from_spec(spec)
                    spec.loader.exec_module(control)
                    with mock.patch.object(control.sys, "argv", [
                            "nt-storage-control", "--socket", endpoint,
                            "--service-user", "nobody", "status"]):
                        self.assertEqual(control.main(), 0)
                    spec2 = importlib.util.spec_from_file_location(
                        "fs4_supervisor", ROOT / "scripts/nt-fs-supervisor.py")
                    supervisor = importlib.util.module_from_spec(spec2)
                    spec2.loader.exec_module(supervisor)
                    supervisor.service_attestation(endpoint, "nobody")
                finally:
                    _pid, status = os.waitpid(child, 0)
                    self.assertTrue(os.WIFEXITED(status) and
                                    os.WEXITSTATUS(status) == 0, status)

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
