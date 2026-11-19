#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Filesystem4 offline service-UID FUSE metadata test: not a mounted-FUSE PASS."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv[1]).resolve()) if len(sys.argv) > 1 else ""
sys.argv = sys.argv[:1]


class MetadataService(unittest.TestCase):
    def test_wal_leases_and_reserved_namespace(self):
        if not BINARY:
            self.fail("Missing nt-dos83-fuse binary")
        if os.geteuid() == 0:
            self.skipTest("selftest deliberately refuses root")
        with tempfile.TemporaryDirectory(prefix="fs4-fuse-metadata-") as root:
            result = subprocess.run(
                [BINARY, "--selftest-metadata", root],
                capture_output=True, text=True, timeout=20,
                check=False)
            self.assertEqual(result.returncode, 0,
                             f"stdout={result.stdout}\nstderr={result.stderr}")
            self.assertIn("PASS offline FUSE UID metadata", result.stdout)
            self.assertIn("(mount not tested)", result.stdout)


if __name__ == "__main__":
    unittest.main()
