#!/usr/bin/env python3
"""Two-process localhost smoke test with Bones as the authoritative host.

The host must NOT have a wall-clock expiration earlier than the client:
CI scheduling delays used to let its demo timer send LEAVE while the
client was still working (phase=8, cause=9), producing a false failure.
The separate test_session_network.py intentionally tests host shutdown.
"""
import os
import re
import socket
import subprocess
import sys
import time


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./build/mouth_lan_demo"
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        port = str(probe.getsockname()[1])

    # No host demo timeout. The harness owns its lifetime and always closes it.
    host_env = dict(os.environ, MOUTH_DEMO_AUTOMOVE="1")
    host_env.pop("MOUTH_DEMO_SECONDS", None)
    client_env = dict(os.environ, MOUTH_DEMO_SECONDS="5", MOUTH_DEMO_AUTOMOVE="1")
    host = subprocess.Popen(
        [binary, "host", port], stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, env=host_env
    )
    client = None
    host_was_running = False
    try:
        time.sleep(0.25)
        client = subprocess.run(
            [binary, "join", "127.0.0.1", port],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, timeout=15, env=client_env
        )
        host_was_running = host.poll() is None
    finally:
        if host.poll() is None:
            host.terminate()
        try:
            hout, herr = host.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            host.kill()
            hout, herr = host.communicate(timeout=5)

    assert client is not None
    assert host_was_running, (
        "Host terminated before the client completed; output follows:\n"
        + hout + "\n" + herr
    )
    assert client.returncode == 0, (
        "Client exited unexpectedly while host was alive:\n"
        + client.stdout + "\n" + client.stderr + "\nHOST:\n" + hout
    )
    assert "CLIENT tick=" in client.stdout, client.stdout
    assert "HOST tick=" in hout, (hout, herr)
    hs = re.findall(
        r"HOST tick=(\d+) status=(\d+).*p2\(([-\d.]+) ([-\d.]+)\)", hout
    )
    cs = re.findall(
        r"CLIENT tick=(\d+).*p2\(([-\d.]+) ([-\d.]+)\)",
        client.stdout
    )
    assert hs and cs, (hout, client.stdout)
    assert max(int(m[0]) for m in hs) > 90, hs
    assert max(int(m[0]) for m in cs) > 90, cs
    assert any(m[1] == "2" for m in hs), hs
    assert max(float(m[3]) for m in hs) > -3.0, hs
    assert max(float(m[2]) for m in cs) > 1.0, cs

    # The remote controls must not drive the local Bones actor.
    hlocal = re.findall(r"HOST tick=\d+.*p1\(([-\d.]+) ([-\d.]+)\)", hout)
    clocal = re.findall(
        r"CLIENT tick=\d+ p1\(([-\d.]+) ([-\d.]+)\)",
        client.stdout
    )
    assert hlocal and clocal, (hout, client.stdout)
    assert all(
        abs(float(x)) < 0.01 and abs(float(z) + 7.0) < 0.01
        for x, z in hlocal + clocal
    ), (hlocal, clocal)

    print("PASS: two UDP processes; host remained online throughout client test.")
    print("PASS: client commands drive only Bones actor2; actor1 stays local.")
    print("CLIENT OUTPUT:\n" + client.stdout.strip())
    print("HOST OUTPUT:\n" + hout.strip())


if __name__ == "__main__":
    main()
