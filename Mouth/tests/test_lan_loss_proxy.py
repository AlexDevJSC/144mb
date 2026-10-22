#!/usr/bin/env python3
"""Bounded LAN resilience smoke test over UDP loopback with deterministic loss.

Does NOT emulate a second PC, guarantee reliability or test security. The
standard Mouth demo supplies host-authoritative Bones movement. No game-code
changes or wire-protocol changes. Run from repo root with:
    python Mouth/tests/test_lan_loss_proxy.py Mouth/build/mouth_lan_demo.exe

The proxy simulates 15% loss per direction and one short link interruption
(1.2 seconds) below Mouth's 5-second timeout. Host outlives client, so a
graceful LEAVE from host cannot race the successful playtest.
"""
import os
import random
import re
import select
import socket
import subprocess
import sys
import threading
import time

def udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]

def main():
    binary=sys.argv[1] if len(sys.argv)>1 else "./build/mouth_lan_demo"
    host_port=udp_port()
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as proxy:
        proxy.bind(("127.0.0.1",0))
        proxy_port=proxy.getsockname()[1]
        # Rare ephemeral port reuse: must be distinct for host and relay.
        while proxy_port==host_port:
            host_port=udp_port()
        proxy.setblocking(False)
        host_address=("127.0.0.1",host_port)
        rng=random.Random(1440304)
        stats={"from_client":0,"from_host":0,"dropped":0,"forwarded":0,
               "blackout_drops":0}
        stop=threading.Event()
        started=time.monotonic()
        client_address=[None]

        def relay():
            while not stop.is_set():
                if not select.select([proxy],[],[],0.05)[0]:
                    continue
                try:
                    data,addr=proxy.recvfrom(2048)
                except BlockingIOError:
                    continue
                elapsed=time.monotonic()-started
                if addr==host_address:
                    stats["from_host"]+=1
                    target=client_address[0]
                else:
                    stats["from_client"]+=1
                    client_address[0]=addr
                    target=host_address
                if not target:
                    continue
                if 3.0 <= elapsed < 4.2:
                    stats["blackout_drops"]+=1
                    stats["dropped"]+=1
                    continue
                if rng.random()<0.15:
                    stats["dropped"]+=1
                    continue
                try:
                    proxy.sendto(data,target)
                    stats["forwarded"]+=1
                except OSError:
                    pass

        thread=threading.Thread(target=relay,daemon=True)
        thread.start()
        host_env=dict(os.environ,MOUTH_DEMO_AUTOMOVE="1")
        host_env.pop("MOUTH_DEMO_SECONDS",None)
        client_env=dict(os.environ,MOUTH_DEMO_SECONDS="12",
                        MOUTH_DEMO_AUTOMOVE="1")
        host=subprocess.Popen([binary,"host",str(host_port)],
                              env=host_env,stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE,text=True)
        try:
            time.sleep(0.3)
            client=subprocess.run([binary,"join","127.0.0.1",str(proxy_port)],
                                  env=client_env,stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE,text=True,timeout=18)
            host_was_alive=host.poll() is None
            if host_was_alive:
                host.terminate()
            try:
                hout,herr=host.communicate(timeout=6)
            except subprocess.TimeoutExpired:
                host.kill()
                hout,herr=host.communicate(timeout=6)
            assert host_was_alive, ("Host exited during the link test",hout,herr)
            assert client.returncode==0,(client.stdout,client.stderr,stats,hout)
            h=re.findall(r"HOST tick=(\d+) status=(\d+).*p1\(([-\d.]+) ([-\d.]+)\) p2\(([-\d.]+) ([-\d.]+)\)",hout)
            c=re.findall(r"CLIENT tick=(\d+) p1\(([-\d.]+) ([-\d.]+)\) p2\(([-\d.]+) ([-\d.]+)\)",client.stdout)
            assert h and c,(hout,client.stdout)
            assert max(int(x[0]) for x in c)>400,c
            assert max(int(x[0]) for x in h)>500,h
            assert any(x[1]=="2" for x in h),h
            assert max(float(x[5]) for x in h)>-2.0,h
            assert max(float(x[4]) for x in c)>-2.0,c
            # Host actor 1 cannot be driven by remote INPUT.
            assert all(abs(float(x[2]))<0.02 and abs(float(x[3])+7.0)<0.02 for x in h),h
            assert all(abs(float(x[1]))<0.02 and abs(float(x[2])+7.0)<0.02 for x in c),c
            assert stats["dropped"]>=10 and stats["blackout_drops"]>=10,stats
            assert stats["forwarded"]>70,stats
            print("PASS: 12-second Bones UDP loopback through 15% loss and 1.2s interruption.")
            print("packets:",stats,
                  "final_client_tick:",max(int(x[0]) for x in c),
                  "host_tick:",max(int(x[0]) for x in h))
        finally:
            stop.set()
            thread.join(timeout=2)
            if host.poll() is None:
                host.terminate()
                try:
                    host.communicate(timeout=3)
                except subprocess.TimeoutExpired:
                    host.kill()
                    host.communicate()

if __name__=="__main__":
    main()
