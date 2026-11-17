# SPDX-License-Identifier: MIT
"""Synthetic FileSystem4 lower-tree audit: no privileged service assumed."""
import importlib.util
import os
import pathlib
import tempfile
import unittest
from unittest import mock

MODULE_PATH = pathlib.Path(__file__).resolve().parents[1] / "scripts/fs4-backing-audit.py"
SPEC = importlib.util.spec_from_file_location("fs4_backing_audit", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class BackingAuditTests(unittest.TestCase):
    def test_unprotected_same_uid_never_reports_isolation(self):
        with tempfile.TemporaryDirectory() as base:
            root = pathlib.Path(base)
            lower, view = root / "lower", root / "view"
            lower.mkdir(mode=0o700)
            view.mkdir(mode=0o700)
            with mock.patch.object(MODULE, "selinux_mode",
                                   return_value="Enforcing"):
                result = MODULE.inspect(str(lower), str(view))
            self.assertTrue(result["same_uid_controls_backing"])
            self.assertFalse(result["protected"])
            self.assertIsNone(result["view_mount_type"])
            self.assertTrue(any("client UID owns" in x for x in result["findings"]))
            # Fedora RPM build tests sometimes run as root. The public
            # CLI must refuse root, while read-only inspection remains
            # testable with disposable directories.
            self.assertEqual(MODULE.main(["--lower", str(lower)]),
                             2 if os.geteuid() == 0 else 3)

    def test_symlink_parent_alias_and_nested_view_rejected(self):
        with tempfile.TemporaryDirectory() as base:
            root = pathlib.Path(base)
            lower = root / "lower"
            lower.mkdir()
            nested = lower / "nested"
            nested.mkdir()
            alias = root / "alias"
            alias.symlink_to(lower, target_is_directory=True)
            for path in (str(alias), "relative-folder", str(root/"missing")):
                with self.subTest(path=path):
                    with self.assertRaises(MODULE.AuditRefused):
                        MODULE.inspected_dir(path)
            with self.assertRaises(MODULE.AuditRefused):
                MODULE.inspect(str(lower), str(nested))

    def test_world_write_is_flagged_independent_of_uid(self):
        with tempfile.TemporaryDirectory() as base:
            lower = pathlib.Path(base) / "lower"
            lower.mkdir()
            os.chmod(lower, 0o777)
            with mock.patch.object(MODULE, "selinux_mode",
                                   return_value="Enforcing"):
                report = MODULE.inspect(str(lower))
            self.assertTrue(report["group_or_other_dac_write"])
            self.assertTrue(any("group or world" in x for x in report["findings"]))


if __name__ == "__main__":
    unittest.main()
