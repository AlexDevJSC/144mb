#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Experimental read-only FileSystem4 capabilities, NEVER a writable PE backend."""
import base64
import json
import os
from pathlib import Path
import secrets
import socket
import stat
import time

VERSION=2
MAX_PACKET=4096
MAX_READ=2048
MAX_HANDLES=32
MAX_REQUESTS=128
SESSION_SECONDS=15

class Refused(Exception):
    pass

def components(path):
    if not isinstance(path,str) or not path.startswith("C:/") or len(path)>1024:
        raise Refused("invalid guest path")
    names=path[3:].split("/")
    if (not names or any(not x or x in (".","..") or "\\" in x or ":" in x
                         or any(ord(c)<32 for c in x) for x in names)
            or names[0].casefold()==":ntnative-file-leases"):
        raise Refused("invalid guest path")
    return names

def root_open(root):
    if root.is_symlink():
        raise Refused("symlink backing")
    fd=os.open(root,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC)
    st=os.fstat(fd)
    if not stat.S_ISDIR(st.st_mode) or st.st_uid!=os.geteuid() or st.st_mode&0o077:
        os.close(fd)
        raise Refused("backing owner or mode")
    return fd

def pinned_readonly(root,path):
    names=components(path)
    parent=root
    temporary=[]
    try:
        for name in names[:-1]:
            parent=os.open(name,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|
                           os.O_CLOEXEC,dir_fd=parent)
            temporary.append(parent)
        file=os.open(names[-1],os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC|
                     os.O_NONBLOCK,dir_fd=parent)
        st=os.fstat(file)
        if not stat.S_ISREG(st.st_mode) or st.st_dev!=os.fstat(root).st_dev:
            os.close(file)
            raise Refused("not a regular file on granted device")
        return file
    except OSError as exc:
        raise Refused("path not readable") from exc
    finally:
        for fd in reversed(temporary): os.close(fd)

def send(conn,reply):
    payload=json.dumps(reply,separators=(",",":")).encode()
    if len(payload)>MAX_PACKET: raise Refused("oversized response")
    conn.sendall(payload)

def run_readonly_session(conn,backing:Path):
    """Called only by the server after SO_PEERCRED and live-mount admission."""
    descriptors={}
    root=root_open(backing)
    token=secrets.token_hex(16)
    deadline=time.monotonic()+SESSION_SECONDS
    conn.settimeout(1)
    try:
        send(conn,{"version":VERSION,"ok":True,"session":token,
                   "readonly":True,"max_read":MAX_READ})
        for _ in range(MAX_REQUESTS):
            if time.monotonic()>=deadline: break
            try:
                packet=conn.recv(MAX_PACKET+1)
            except (OSError,TimeoutError):
                break
            if not packet or len(packet)>MAX_PACKET: break
            try:
                q=json.loads(packet.decode("utf-8"))
            except (ValueError,UnicodeError):
                break
            if not isinstance(q,dict) or q.get("version")!=VERSION or q.get("session")!=token:
                break
            op=q.get("op")
            if op=="end" and set(q)=={"version","session","op"}:
                send(conn,{"version":VERSION,"ok":True})
                break
            try:
                if op=="open" and set(q)=={"version","session","op","path"}:
                    if len(descriptors)>=MAX_HANDLES: raise Refused("handle limit")
                    fd=pinned_readonly(root,q["path"])
                    cap=secrets.token_hex(16)
                    descriptors[cap]=fd
                    reply={"version":VERSION,"ok":True,"handle":cap,
                           "size":os.fstat(fd).st_size}
                elif op=="read" and set(q)=={"version","session","op","handle","offset","size"}:
                    cap=q["handle"]
                    offset,size=q["offset"],q["size"]
                    if (not isinstance(cap,str) or cap not in descriptors or
                        type(offset)!=int or type(size)!=int or
                        offset<0 or offset>(1<<63)-1 or size<0 or size>MAX_READ):
                        raise Refused("bad read")
                    b=os.pread(descriptors[cap],size,offset)
                    reply={"version":VERSION,"ok":True,
                           "data_b64":base64.b64encode(b).decode("ascii"),"read":len(b)}
                elif op=="close" and set(q)=={"version","session","op","handle"}:
                    cap=q["handle"]
                    if not isinstance(cap,str) or cap not in descriptors:
                        raise Refused("unknown handle")
                    os.close(descriptors.pop(cap))
                    reply={"version":VERSION,"ok":True}
                else:
                    raise Refused("unknown or forbidden operation")
            except (OSError,Refused):
                reply={"version":VERSION,"ok":False,"error":"refused"}
            send(conn,reply)
    finally:
        for fd in descriptors.values(): os.close(fd)
        os.close(root)
