#!/usr/bin/env python3
"""LAN failures: bad IP, incompatible procedural world, absent host and close."""
import os
import socket
import subprocess
import sys
import time

def port():
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as p:
        p.bind(("127.0.0.1",0))
        return str(p.getsockname()[1])

def run(args, duration=7, extra=None):
    env=dict(os.environ,MOUTH_DEMO_SECONDS=str(duration),MOUTH_DEMO_AUTOMOVE="1")
    env.update(extra or {})
    return subprocess.run(args,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                          text=True,timeout=duration+8,env=env)

def main():
    binpath=sys.argv[1] if len(sys.argv)>1 else "./build/mouth_lan_demo"
    bad=run([binpath,"join","this-is-not-an-ip","14444"],1)
    assert bad.returncode!=0 and "result=2" in bad.stderr,(bad.returncode,bad.stderr)
    p=port()
    absent=run([binpath,"join","127.0.0.1",p],7)
    assert absent.returncode!=0 and "phase=7 cause=8" in absent.stderr,absent.stderr
    p=port()
    env=dict(os.environ,MOUTH_DEMO_SECONDS="3",MOUTH_DEMO_AUTOMOVE="1")
    host=subprocess.Popen([binpath,"host",p],stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE,text=True,env=env)
    try:
        time.sleep(0.30)
        mismatch=run([binpath,"join","127.0.0.1",p],2,
                     {"MOUTH_DEMO_BAD_GENERATOR":"1"})
        assert mismatch.returncode!=0 and "phase=6 cause=6" in mismatch.stderr,\
               mismatch.stderr
        hout,herr=host.communicate(timeout=9)
        assert host.returncode==0,(herr,hout)
        assert "status=2" not in hout, hout
    finally:
        if host.poll() is None:
            host.terminate()
            host.communicate(timeout=5)
    p=port()
    env=dict(os.environ,MOUTH_DEMO_SECONDS="2",MOUTH_DEMO_AUTOMOVE="1")
    host=subprocess.Popen([binpath,"host",p],stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE,text=True,env=env)
    try:
        time.sleep(0.3)
        ended=run([binpath,"join","127.0.0.1",p],8)
        hout,herr=host.communicate(timeout=8)
        assert host.returncode==0,(hout,herr)
        assert ended.returncode!=0 and "phase=8 cause=9" in ended.stderr,\
               (ended.returncode,ended.stdout,ended.stderr)
        assert "CLIENT tick=" in ended.stdout,ended.stdout
    finally:
        if host.poll() is None:
            host.terminate()
            host.communicate(timeout=5)
    print("PASS Mouth: invalid IP, connection timeout, incompatible world, graceful disconnect.")
if __name__=="__main__":
    main()
