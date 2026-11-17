#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Client for the dedicated-UID FileSystem4 control socket. No FD passing."""
import argparse
import json
import os
import pwd
import socket
import struct
import sys

VERSION = 1
DEFAULT = "/run/ntnative-storage/control.sock"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", default=DEFAULT)
    parser.add_argument("--service-user", default="ntnative-storage")
    sub = parser.add_subparsers(dest="op", required=True)
    sub.add_parser("status")
    rename = sub.add_parser("rename")
    rename.add_argument("source")
    rename.add_argument("destination")
    args = parser.parse_args()
    try:
        expected_uid = pwd.getpwnam(args.service_user).pw_uid
        request = {"version": VERSION, "op": args.op}
        if args.op == "rename":
            request.update({"from": args.source, "to": args.destination})
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as conn:
            conn.settimeout(20)
            conn.connect(args.socket)
            raw = conn.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED,
                                  struct.calcsize("3i"))
            _pid, peer_uid, _gid = struct.unpack("3i", raw)
            if peer_uid != expected_uid or peer_uid == os.geteuid():
                raise RuntimeError("Untrusted or same-UID storage service")
            conn.sendall(json.dumps(request, separators=(",", ":")).encode("utf-8"))
            response = json.loads(conn.recv(4096).decode("utf-8"))
        if response.get("version") != VERSION or response.get("ok") is not True:
            raise RuntimeError(response.get("error", "Invalid storage response"))
        print(json.dumps(response, sort_keys=True))
        return 0
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        print(f"REFUSED: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
