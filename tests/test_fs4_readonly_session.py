# SPDX-License-Identifier: MIT
import base64
import json
import os
from pathlib import Path
import socket
import sys
import tempfile
import threading
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"scripts"))
from fs4_readonly_session import components, run_readonly_session, Refused

class ReadonlySessionTest(unittest.TestCase):
    def test_path_rejections(self):
        for path in ("C:/../bad","D:/bad","C:/","C:/a//b","C:/a/./b",
                     "C:/a\\b","C:/a:b","C:/:ntnative-file-leases/hidden"):
            with self.subTest(path=path),self.assertRaises(Refused): components(path)

    def test_kernel_socket_capabilities_and_cleanup(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)/"C"
            root.mkdir(mode=0o700)
            (root/"file.txt").write_bytes(b"HELLO")
            (root/"dir").mkdir()
            (root/"dir"/"two.txt").write_bytes(b"DATA")
            (root/"escape").symlink_to("/etc/passwd")
            (root/"marked.txt").write_bytes(b"NT-marked")
            marker_supported = True
            try:
                os.setxattr(root/"marked.txt","user.ntnative.fs_compat",b"1")
            except OSError:
                marker_supported = False
            server,client=socket.socketpair(socket.AF_UNIX,socket.SOCK_SEQPACKET)
            faults=[]
            def worker():
                try:
                    with server: run_readonly_session(server,root)
                except BaseException as e: faults.append(e)
            t=threading.Thread(target=worker,daemon=True)
            t.start()
            with client:
                hello=json.loads(client.recv(4096))
                self.assertTrue(hello["readonly"])
                s=hello["session"]
                def send(op,**kw):
                    client.sendall(json.dumps(dict(version=2,session=s,op=op,**kw)).encode())
                    return json.loads(client.recv(4096))
                opened=send("open",path="C:/file.txt")
                self.assertTrue(opened["ok"])
                h=opened["handle"]
                self.assertEqual(base64.b64decode(send("read",handle=h,offset=1,size=4)["data_b64"]),b"ELLO")
                if marker_supported:
                    self.assertFalse(send("open",path="C:/marked.txt")["ok"])
                for req in (("open",dict(path="C:/escape")),
                            ("open",dict(path="C:/dir")),
                            ("write",dict(handle=h,data_b64="WA==")),
                            ("truncate",dict(handle=h,size=0)),
                            ("read",dict(handle=h,offset=-1,size=5)),
                            ("read",dict(handle=h,offset=0,size=999999))):
                    self.assertFalse(send(req[0],**req[1])["ok"],req)
                self.assertTrue(send("close",handle=h)["ok"])
                self.assertFalse(send("read",handle=h,offset=0,size=1)["ok"])
                self.assertTrue(send("open",path="C:/dir/two.txt")["ok"])
                self.assertTrue(send("end")["ok"])
            t.join(3)
            self.assertFalse(t.is_alive())
            self.assertEqual(faults,[])
            self.assertEqual((root/"file.txt").read_bytes(),b"HELLO")
            self.assertEqual((root/"dir"/"two.txt").read_bytes(),b"DATA")
