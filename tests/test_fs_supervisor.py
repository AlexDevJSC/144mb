# SPDX-License-Identifier: MIT
"""Synthetic fail-closed supervisor and grant validation, no FUSE mount needed."""
import importlib.util
import os
import pathlib
import tempfile
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "scripts" / "nt-fs-supervisor.py"
SPEC = importlib.util.spec_from_file_location("nt_fs_supervisor", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class FileSystemSupervisorTests(unittest.TestCase):
    def test_disjoint_grants(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            (root / "backing").mkdir()
            (root / "view").mkdir()
            lower, view = MODULE.canonical_grants(str(root / "backing"),
                                                   str(root / "view"))
            self.assertNotEqual(lower, view)

    def test_reject_same_nested_and_symlink_alias(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            (root / "backing").mkdir()
            (root / "backing" / "nested").mkdir()
            (root / "alias").symlink_to(root / "backing", target_is_directory=True)
            cases = [
                (root / "backing", root / "backing"),
                (root / "backing", root / "backing" / "nested"),
                (root / "backing", root / "alias"),
            ]
            for lower, view in cases:
                with self.subTest(case=(lower, view)):
                    with self.assertRaises(MODULE.Refused):
                        MODULE.canonical_grants(str(lower), str(view))

    def test_reject_absent_directories_and_missing_command(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            with self.assertRaises(MODULE.Refused):
                MODULE.canonical_grants(str(root), str(root / "none"))
        with self.assertRaises(SystemExit) as error:
            MODULE.main(["--lower", "/tmp", "--view", "/tmp", "--"])
        self.assertEqual(error.exception.code, 2)

    def test_two_sessions_cannot_own_same_view(self):
        with tempfile.TemporaryDirectory() as temp:
            view = pathlib.Path(temp) / "view"
            view.mkdir()
            with mock.patch.dict(os.environ, {"XDG_RUNTIME_DIR": temp}):
                first = MODULE.lock_session(view)
                try:
                    with self.assertRaises(MODULE.Refused):
                        MODULE.lock_session(view)
                finally:
                    os.close(first)
                second = MODULE.lock_session(view)
                os.close(second)

    def test_unverified_mount_refusal_without_executing_guest(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            (root / "backing").mkdir()
            (root / "view").mkdir()
            # This test is a no-op on an ordinary CI host. It does not need
            # /dev/fuse and may not start any untrusted guest application.
            self.assertFalse(MODULE.mounted(root / "view", "mountpoint"))


if __name__ == "__main__":
    unittest.main()
