#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Dedicated-UID storage owner for the opted-in POSIX FUSE data plane.

The authenticated AF_UNIX control plane NEVER transfers backing FDs.
No PE runtime is allowed to run under the storage identity; a PE IPC file
backend is a separate, still unimplemented milestone.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import signal
import socket
import stat
import struct
import subprocess
import sys
import time

PROTOCOL = 1
MAX_PACKET = 4096
MAX_NAME = 1024


class Refused(Exception):
    pass


def private_directory(path: Path, uid: int) -> Path:
    if path.is_symlink():
        raise Refused("Symlink storage root refused")
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        info = os.fstat(fd)
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != uid or info.st_mode & 0o077:
            raise Refused("Backing must be owned exclusively by service UID (0700)")
        return path.resolve(strict=True)
    finally:
        os.close(fd)


def trusted_binary(path: str) -> str:
    target = Path(path)
    if not target.is_absolute() or target.is_symlink():
        raise Refused("Trusted executable must be an absolute, non-symlink path")
    info = target.stat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
        raise Refused("Trusted executable must be root-owned and not client-writable")
    if not os.access(target, os.X_OK):
        raise Refused("Trusted executable is not executable")
    return str(target)


def guest_path(path: object) -> str:
    if not isinstance(path, str) or not path.startswith("C:/") or len(path) > MAX_NAME:
        raise Refused("Only bounded, absolute C:/ paths are supported")
    parts = path[3:].split("/")
    if not parts or any(p in ("", ".", "..") for p in parts):
        raise Refused("Empty, dot or traversal component denied")
    if any("\\" in p or ":" in p or any(ord(c) < 32 for c in p) for p in parts):
        raise Refused("Invalid Windows path component")
    return path


def peer_identity(conn: socket.socket) -> tuple[int, int, int]:
    raw = conn.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i"))
    return struct.unpack("3i", raw)  # pid, uid, gid at connection


def mounted_fuse(view: Path) -> bool:
    # A random mountpoint is not sufficient evidence of *our* FUSE backend.
    wanted = str(view)
    try:
        with open("/proc/self/mountinfo", encoding="utf-8") as stream:
            for line in stream:
                before, sep, after = line.partition(" - ")
                if sep:
                    fields, fs = before.split(), after.split()
                    if len(fields) >= 5 and fs and fields[4] == wanted:
                        return fs[0] == "fuse.nt-dos83"
    except OSError:
        return False
    return False


def dispatch(request: object, *, mediator: str, backing: Path,
             live_mount: bool) -> dict[str, object]:
    if not isinstance(request, dict) or request.get("version") != PROTOCOL:
        raise Refused("Protocol version mismatch")
    if request.get("op") == "status":
        if set(request) != {"version", "op"}:
            raise Refused("Unexpected status parameters")
        return {"version": PROTOCOL, "ok": True, "mediated": live_mount}
    if request.get("op") != "rename":
        raise Refused("Unsupported operation")
    if set(request) != {"version", "op", "from", "to"} or not live_mount:
        raise Refused("Invalid rename request or FUSE mount unavailable")
    old, new = guest_path(request["from"]), guest_path(request["to"])
    if old == new:
        raise Refused("No-op rename refused")
    # Rename authority stays inside the existing Win32 broker, which owns
    # the DOS8.3 WAL and its own share-lease validation. No shell or fd pass.
    try:
        completed = subprocess.run(
            [mediator, "--root", str(backing), "--move", old, new],
            capture_output=True, timeout=15, check=False)
    except subprocess.TimeoutExpired:
        raise Refused("Mediated rename timed out; recovery required") from None
    if completed.returncode != 0:
        raise Refused("Mediated rename rejected; inspect service logs")
    return {"version": PROTOCOL, "ok": True}


def serve_one(conn: socket.socket, *, client_uid: int, mediator: str,
              backing: Path, ready: bool) -> None:
    conn.settimeout(2)
    _pid, uid, _gid = peer_identity(conn)
    if uid != client_uid:
        # Unauthorized clients learn nothing about the backing or its files.
        return
    try:
        data = conn.recv(MAX_PACKET + 1)
        if not data or len(data) > MAX_PACKET:
            raise Refused("Empty or oversized request")
        try:
            request = json.loads(data.decode("utf-8"))
        except (UnicodeError, ValueError):
            raise Refused("Malformed request") from None
        reply = dispatch(request, mediator=mediator, backing=backing,
                         live_mount=ready)
    except (Refused, OSError) as error:
        reply = {"version": PROTOCOL, "ok": False, "error": str(error)}
    try:
        conn.sendall(json.dumps(reply, separators=(",", ":")).encode("utf-8"))
    except (OSError, TimeoutError):
        pass


def inherited_socket() -> socket.socket:
    if (os.environ.get("LISTEN_PID") != str(os.getpid()) or
            os.environ.get("LISTEN_FDS") != "1"):
        raise Refused("Systemd Unix socket activation is mandatory")
    sock = socket.socket(fileno=3)
    if sock.family != socket.AF_UNIX or sock.getsockopt(
            socket.SOL_SOCKET, socket.SO_TYPE) != socket.SOCK_SEQPACKET:
        raise Refused("Expected a Unix SOCK_SEQPACKET listener")
    sock.settimeout(0.25)
    return sock


def service(backing: Path, view: Path, *, client_uid: int, client_gid: int,
            fuse: str, mediator: str, unmount: str) -> None:
    uid = os.geteuid()
    if uid == 0 or uid == client_uid or client_uid <= 0 or client_gid <= 0:
        raise Refused("Service requires its own non-root UID, distinct from clients")
    backing = private_directory(backing, uid)
    if view.is_symlink() or not view.is_dir() or view.stat().st_uid != uid:
        raise Refused("FUSE mountpoint must be an existing service-owned directory")
    exposed = view.resolve(strict=True)
    if backing == exposed or backing in exposed.parents or exposed in backing.parents:
        raise Refused("Backing and public view must be disjoint")
    fuse, mediator, unmount = map(trusted_binary, (fuse, mediator, unmount))
    if mounted_fuse(exposed):
        raise Refused("Refusing to take over a mounted view")
    # Startup recovery must resolve both DOS8.3 WAL and orphaned
    # FILE_DELETE_PENDING markers BEFORE the FUSE view becomes visible.
    for operation in ("--recover", "--scan"):
        preflight = subprocess.run([mediator, "--root", str(backing), operation],
                                   capture_output=True, timeout=30, check=False)
        if preflight.returncode:
            raise Refused("FileSystem4 storage preflight refused " + operation)

    stopped = False
    def terminate(_signal: int, _frame: object) -> None:
        nonlocal stopped
        stopped = True
    for signum in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(signum, terminate)

    # Child FUSE daemon is the same dedicated UID as this service, NOT the
    # client's UID, and holds the only physical writable backing grant.
    proc = subprocess.Popen([
        fuse, str(backing), str(exposed), "-f",
        "--service-client-uid", str(client_uid),
        "--service-client-gid", str(client_gid)
    ], close_fds=True, start_new_session=True,
       stdin=subprocess.DEVNULL)
    try:
        for _ in range(100):
            if proc.poll() is not None:
                raise Refused("FUSE backend failed to start")
            if mounted_fuse(exposed):
                break
            time.sleep(0.1)
        else:
            raise Refused("FUSE mount was not established")
        listener = inherited_socket()
        try:
            while not stopped:
                if proc.poll() is not None or not mounted_fuse(exposed):
                    raise Refused("FUSE data plane lost; refusing future operations")
                try:
                    conn, _address = listener.accept()
                except socket.timeout:
                    continue
                with conn:
                    serve_one(conn, client_uid=client_uid,
                              mediator=mediator, backing=backing, ready=True)
        finally:
            listener.close()
    finally:
        if mounted_fuse(exposed):
            subprocess.run([unmount, "-u", str(exposed)],
                           capture_output=True, timeout=8, check=False)
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=3)
        if mounted_fuse(exposed):
            raise Refused("FUSE unmount failed; operator intervention required")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backing", type=Path, required=True)
    parser.add_argument("--view", type=Path, required=True)
    parser.add_argument("--client-uid", type=int, required=True)
    parser.add_argument("--client-gid", type=int, required=True)
    parser.add_argument("--fuse", required=True)
    parser.add_argument("--mediator", required=True)
    parser.add_argument("--unmount", default="/usr/bin/fusermount3")
    args = parser.parse_args()
    try:
        service(args.backing, args.view, client_uid=args.client_uid,
                client_gid=args.client_gid, fuse=args.fuse,
                mediator=args.mediator, unmount=args.unmount)
    except (OSError, Refused, subprocess.TimeoutExpired) as error:
        print("STORAGE REFUSED: " + str(error), file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
