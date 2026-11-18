#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Physical, non-root Fedora gate: service UID, SELinux, FUSE and IPC."""
import json
import errno
import os
from pathlib import Path
import pwd
import socket
import struct
import subprocess
import sys
import tempfile

BACKING = Path("/var/lib/ntnative-storage/C")
VIEW = Path("/run/ntnative-storage/view")
ENDPOINT = "/run/ntnative-storage/control.sock"


def required(ok, label):
    if not ok:
        raise RuntimeError("FAIL " + label)
    print("PASS " + label)


def control(query, expected_uid):
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as conn:
        conn.settimeout(20)
        conn.connect(ENDPOINT)
        # systemd may create the socket as root before the daemon starts.
        # SCM_CREDENTIALS authenticates the replying daemon itself.
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_PASSCRED, 1)
        conn.sendall(json.dumps(query).encode("utf-8"))
        payload, control, flags, _ = conn.recvmsg(4096, socket.CMSG_SPACE(12))
        peers = [struct.unpack("3i", data[:12])
                 for level, kind, data in control
                 if level == socket.SOL_SOCKET and
                 kind == socket.SCM_CREDENTIALS and len(data) >= 12]
        required(not flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC) and
                 len(peers) == 1 and peers[0][1] == expected_uid and
                 peers[0][1] != os.geteuid(),
                 "SCM_CREDENTIALS proves independent reply sender")
        return json.loads(payload.decode("utf-8"))



def denied(operation, reason):
    """A permission refusal is mandatory. ENOSYS/ENOTSUP is acceptable
    for operations deliberately disabled until NT security is implemented."""
    try:
        operation()
    except OSError as error:
        if error.errno not in (errno.EACCES, errno.EPERM, errno.ENOTSUP,
                               errno.EOPNOTSUPP, errno.ENOSYS):
            raise RuntimeError(
                "FAIL " + reason + ": unexpected error " + str(error.errno))
    else:
        raise RuntimeError("FAIL " + reason + ": operation unexpectedly succeeded")
    print("PASS " + reason + " denied")


def handle_service_handshake(expected_uid):
    """Require v3 storage worker itself; v1 status alone is not enough.
    The reply cannot carry a physical FD (SCM_RIGHTS)."""
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as conn:
        conn.settimeout(10)
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_PASSCRED, 1)
        conn.connect(ENDPOINT)
        conn.sendall(b'{"version":3,"op":"session"}')
        frame, control_messages, flags, _ = conn.recvmsg(
            4096, socket.CMSG_SPACE(12))
        required(not flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC) and
                 len(frame) == 32, "v3 PE HANDLE IPC handshake replied")
        identities = []
        for level, kind, payload in control_messages:
            if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                raise RuntimeError("FAIL broker attempted to transfer a backing FD")
            if level == socket.SOL_SOCKET and kind == socket.SCM_CREDENTIALS:
                if len(payload) >= 12:
                    identities.append(struct.unpack("3i", payload[:12]))
        required(len(identities) == 1 and identities[0][1] == expected_uid,
                 "broker HELLO uses dedicated service UID")
        magic, version, op, error, size, handle, nonce = struct.unpack(
            "<IHHIIQQ", frame)
        required((magic, version, op, error, size, handle) ==
                 (0x34465348, 3, 0, 0, 0, 0) and nonce != 0,
                 "authenticated v3 HANDLE capability handshake")


def main():
    required(os.geteuid() != 0, "unprivileged client, never root")
    required(not (os.environ.get("CI") == "true" or
                  os.environ.get("GITHUB_ACTIONS") == "true"),
             "physical owner-run only, never CI")
    enforcement = subprocess.run(["getenforce"], text=True, capture_output=True)
    required(enforcement.returncode == 0 and
             enforcement.stdout.strip() == "Enforcing", "SELinux Enforcing")
    service_uid = pwd.getpwnam("ntnative-storage").pw_uid
    required(service_uid != os.geteuid(), "service UID differs from client")

    # Check only with read-only FD. Do not mutate the physical backing.
    try:
        fd = os.open(BACKING, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    except PermissionError:
        pass
    else:
        os.close(fd)
        raise RuntimeError("FAIL client opened physical backing")
    print("PASS direct backing open denied by DAC/SELinux")

    status = control({"version": 1, "op": "status"}, service_uid)
    required(status.get("ok") is True and status.get("mediated") is True,
             "FUSE mounted, authenticated control socket")
    handle_service_handshake(service_uid)
    required(VIEW.is_dir() and VIEW.stat().st_uid == os.geteuid(),
             "FUSE owner mapping for client")

    with tempfile.TemporaryDirectory(prefix="fs4-svc-", dir=VIEW) as temp:
        base = Path(temp)
        source = base / "veryLongOriginalFileName.txt"
        dest = base / "veryLongRenamedFileName.txt"
        sample = b"fs4 dedicated UID mediated payload\x00\xff"
        source.write_bytes(sample)
        required(source.read_bytes() == sample, "FUSE read/write intact")
        identity = (source.stat().st_dev, source.stat().st_ino)
        prefix = "C:/" + base.name + "/"
        reply = control({"version": 1, "op": "rename",
                         "from": prefix + source.name,
                         "to": prefix + dest.name}, service_uid)
        required(reply.get("ok") is True and not source.exists() and dest.exists(),
                 "IPC rename used the broker")
        required((dest.stat().st_dev, dest.stat().st_ino) == identity and
                 dest.read_bytes() == sample, "inode and data identity preserved")
        # Negative tests run on the mounted FUSE inode, not an offline mock.
        denied(lambda: os.chmod(dest, 0o777), "FUSE chmod")
        denied(lambda: os.utime(dest, None), "FUSE utimens")
        denied(lambda: os.setxattr(dest, "user.fs4_service_probe", b"X"),
               "FUSE arbitrary setxattr")
        denied(lambda: os.removexattr(dest, "user.fs4_service_probe"),
               "FUSE arbitrary removexattr")
        hardlink = base / "blockedHardlink"
        denied(lambda: os.link(dest, hardlink), "FUSE raw hardlink")
        required(not hardlink.exists(), "denied hardlink not published")
        symlink = base / "blockedSymlink"
        denied(lambda: os.symlink("relative-target", symlink),
               "FUSE unimplemented symlink")
        required(not symlink.is_symlink(), "denied symlink not published")
        required(dest.read_bytes() == sample,
                 "denied mutations preserved original bytes")
        try:
            os.getxattr(dest, "user.ntnative.fs_compat")
        except OSError as error:
            required(error.errno in (errno.ENODATA, errno.EOPNOTSUPP),
                     "internal NT xattr cannot be read")
        else:
            raise RuntimeError("FAIL FUSE exposed private NT marker")
    # These denials may be attributable to DAC alone: the operator must
    # separately inspect effective SELinux policy/domain and AVC records.
    labels = subprocess.run(["ls", "-Zd", str(BACKING), str(VIEW)],
                            capture_output=True, text=True)
    if labels.returncode == 0:
        print("SELinux labels (manual AVC/policy review):", labels.stdout.strip())
    else:
        print("SELinux labels unavailable to client; inspect as administrator")
    print("PASS physical UID+Enforcing+mounted FUSE+v3 IPC smoke")
    print("MANUAL PENDING: SELinux AVC/domain review, another UID denial,"
          " PE executable, physical power cut and Windows NTFS comparison")


if __name__ == "__main__":
    if sys.argv[1:] != ["--physical-confirmed"]:
        print("REFUSED: supply --physical-confirmed on actual Fedora host",
              file=sys.stderr)
        sys.exit(2)
    try:
        main()
    except (OSError, RuntimeError, ValueError) as exc:
        print(exc, file=sys.stderr)
        sys.exit(3)
