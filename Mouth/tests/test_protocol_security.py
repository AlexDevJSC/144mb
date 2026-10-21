#!/usr/bin/env python3
"""Trusted LAN protocol negative tests: incompatible wire version and bad owner.
   A hostile socket must not invent Bones movement or join without HELLO. """
import os
import re
import socket
import struct
import subprocess
import sys
import time

def main():
    exe=sys.argv[1] if len(sys.argv)>1 else "./build/mouth_lan_demo"
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as p:
        p.bind(("127.0.0.1",0))
        port=int(p.getsockname()[1])
    env=dict(os.environ,MOUTH_DEMO_SECONDS="3")
    host=subprocess.Popen([exe,"host",str(port)],stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE,text=True,env=env)
    try:
        time.sleep(0.25)
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as attacker:
            attacker.bind(("127.0.0.1",0))
            attacker.settimeout(2.0)
            # Format: 12-byte header + HELLO world/seed/generator/nonce.
            mismatched=struct.pack("<4sBBHIQQII",b"M144",9,1,36,0,
                                  0x14401,0x1442026,1,0x1255)
            assert len(mismatched)==36
            attacker.sendto(mismatched,("127.0.0.1",port))
            data,from_addr=attacker.recvfrom(128)
            assert data[:4]==b"M144" and len(data)==13 and data[5]==5
            assert data[12]==2, data   # protocol incompatibility
            # A forged input is ignored even if the remote actor exists.
            for sequence in range(1,60):
                bogus=struct.pack("<4sBBHI IbbhBB",b"M144",1,3,22,
                                  0x11223344,sequence,1,1,0,1,1)
                assert len(bogus)==22
                attacker.sendto(bogus,("127.0.0.1",port))
        hout,herr=host.communicate(timeout=8)
        assert host.returncode==0,(hout,herr)
        readings=re.findall(r"HOST tick=\d+ status=(\d+).*p2\(([-\d.]+) ([-\d.]+)\)",hout)
        assert readings,hout
        assert all(state!="2" and abs(float(z)+4.0)<0.02
                   for state,x,z in readings),readings
        print("PASS Mouth: wire protocol version rejection and foreign input isolation.")
    finally:
        if host.poll() is None:
            host.terminate()
            host.communicate(timeout=5)
if __name__=="__main__":
    main()
