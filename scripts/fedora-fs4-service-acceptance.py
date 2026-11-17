#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Physical, non-root Fedora gate: service UID, SELinux, FUSE and IPC."""
import json
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
        peer = conn.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
        _, uid, _ = struct.unpack("3i", peer)
        required(uid == expected_uid and uid != os.geteuid(),
                 "SO_PEERCRED identifies independent storage owner")
        conn.sendall(json.dumps(query).encode("utf-8"))
        return json.loads(conn.recv(4096).decode("utf-8"))


def main():
    required(os.geteuid() != 0, "unprivileged client, never root")
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
        try:
            os.chmod(dest, 0o777)
        except PermissionError:
            pass
        else:
            raise RuntimeError("FAIL FUSE allowed private-DAC chmod")
        print("PASS FUSE chmod refused")
    print("PASS physical service-UID gate (not a Windows/PE or power cut gate)")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as exc:
        print(exc, file=sys.stderr)
        sys.exit(3)
