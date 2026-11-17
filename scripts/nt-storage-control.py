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
            # Socket activation may leave SO_PEERCRED identifying PID1
            # (the listener CREATOR). Kernel SCM_CREDENTIALS on the reply
            # authenticates the process which ACTUALLY sent the response.
            conn.setsockopt(socket.SOL_SOCKET, socket.SO_PASSCRED, 1)
            conn.sendall(json.dumps(request, separators=(",", ":")).encode("utf-8"))
            payload, control, flags, _ = conn.recvmsg(
                4096, socket.CMSG_SPACE(struct.calcsize("3i")))
            identities = [struct.unpack("3i", data[:12])
                          for level, kind, data in control
                          if level == socket.SOL_SOCKET and
                          kind == socket.SCM_CREDENTIALS and len(data) >= 12]
            if (flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC) or
                    len(identities) != 1 or
                    identities[0][1] != expected_uid or
                    identities[0][1] == os.geteuid()):
                raise RuntimeError("Untrusted storage reply sender identity")
            response = json.loads(payload.decode("utf-8"))
        if response.get("version") != VERSION or response.get("ok") is not True:
            raise RuntimeError(response.get("error", "Invalid storage response"))
        print(json.dumps(response, sort_keys=True))
        return 0
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        print(f"REFUSED: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
