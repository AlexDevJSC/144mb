#!/usr/bin/env python3
"""Two process localhost smoke test, not an Internet stress/latency certification.
Run with: make -C Mouth loopback (Linux). Uses Bones Foundation from repository.
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
    env = dict(os.environ, MOUTH_DEMO_SECONDS="4", MOUTH_DEMO_AUTOMOVE="1")
    host = subprocess.Popen([binary, "host", port],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, env=env)
    try:
        time.sleep(0.25)
        client = subprocess.run([binary, "join", "127.0.0.1", port],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, timeout=10, env=env)
        hout, herr = host.communicate(timeout=10)
        assert client.returncode == 0, client.stderr
        assert host.returncode == 0, herr
        assert "CLIENT tick=" in client.stdout, client.stdout
        assert "HOST tick=" in hout, hout
        hs = re.findall(r"HOST tick=(\d+) status=(\d+).*p2\(([-\d.]+) ([-\d.]+)\)", hout)
        cs = re.findall(r"CLIENT tick=(\d+).*p2\(([-\d.]+) ([-\d.]+)\)", client.stdout)
        assert hs and cs, (hout, client.stdout)
        assert max(int(m[0]) for m in hs) > 90, hs
        assert max(int(m[0]) for m in cs) > 90, cs
        assert any(m[1] == "2" for m in hs), hs  # connected
        assert max(float(m[3]) for m in hs) > -3.0, hs  # Bones moved actor2
        assert max(float(m[2]) for m in cs) > 1.0, cs
        print("PASS: two UDP processes; client commands drive Bones actor2.")
        print("CLIENT OUTPUT:\n" + client.stdout.strip())
        print("HOST OUTPUT:\n" + hout.strip())
    finally:
        if host.poll() is None:
            host.terminate()
            try:
                host.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                host.kill()
                host.communicate()

if __name__ == "__main__":
    main()
