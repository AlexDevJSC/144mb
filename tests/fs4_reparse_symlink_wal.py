#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reparse symlink creation under a corrupt WAL must fail before symlinkat."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

def run(binary,root,*args):
    return subprocess.run([binary,"--root",str(root),*args],
                          text=True,capture_output=True,timeout=15)

def main():
    normal=str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="fs4-reparse-wal-") as tmp:
        root=Path(tmp)
        target=root/"regular.txt"
        target.write_bytes(b"UNMODIFIED")
        link=root/"safeLink.txt"
        private=root/":ntnative-file-leases"
        private.mkdir(mode=0o700)
        wal=private/"dos83-rename-v1.wal"
        wal.write_bytes(b"CORRUPT-WAL")
        wal.chmod(0o600)
        result=run(normal,root,"--symlink","C:/safeLink.txt","regular.txt")
        assert result.returncode!=0,result.stderr
        assert not link.is_symlink()
        assert target.read_bytes()==b"UNMODIFIED"
        assert wal.read_bytes()==b"CORRUPT-WAL"
        wal.write_bytes(b"")
        result=run(normal,root,"--symlink","C:/safeLink.txt","regular.txt")
        assert result.returncode==0,result.stderr
        assert link.is_symlink()
        assert os.readlink(link)=="regular.txt"
        assert target.read_bytes()==b"UNMODIFIED"
        invalid=run(normal,root,"--symlink","C:/badLink.txt","../outside")
        assert invalid.returncode!=0, invalid.stderr
        assert not (root/"badLink.txt").exists()
        print("PASS symlink reparse creation respects WAL and relative target policy")

if __name__=="__main__":
    if len(sys.argv)!=2:
        raise SystemExit("Usage: fs4_reparse_symlink_wal.py MEDIATOR")
    main()
