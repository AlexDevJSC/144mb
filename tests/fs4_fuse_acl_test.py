#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Offline FUSE security admission/inheritance, not mounted FUSE coverage."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv[1]).resolve()) if len(sys.argv) > 1 else ""
sys.argv = sys.argv[:1]


class ManagedFUSEACL(unittest.TestCase):
    def test_inherit_before_publication_and_deny_protected_operations(self):
        self.assertTrue(BINARY, "missing nt-dos83-fuse")
        if os.geteuid() == 0:
            self.skipTest("the trusted service ACL fixture refuses root")
        with tempfile.TemporaryDirectory(prefix="fs4-managed-fuse-acl-") as root:
            result = subprocess.run(
                [BINARY, "--selftest-acl", root],
                capture_output=True, text=True, timeout=30, check=False)
            self.assertEqual(result.returncode, 0,
                             f"stdout={result.stdout}\nstderr={result.stderr}")
            if "SKIP offline managed FUSE ACL" in result.stdout:
                self.skipTest(result.stdout.strip())
            self.assertIn("PASS offline FUSE managed DACL inherited", result.stdout)
            self.assertIn("(mount not tested)", result.stdout)


if __name__ == "__main__":
    unittest.main()
