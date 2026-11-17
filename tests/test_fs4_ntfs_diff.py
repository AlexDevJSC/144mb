# SPDX-License-Identifier: MIT
import importlib.util
import pathlib
import sys
import unittest

SCRIPTS = pathlib.Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
from fs4_ntfs_diff import compare


class NTFSReferenceDiffTests(unittest.TestCase):
    def setUp(self):
        self.win = dict(schema="filesystem4-windows-reference-v1",
                        volume_filesystem="NTFS",
                        case_insensitive_exact_name=True,
                        rename_preserves_content=True,
                        readonly_attribute_visible=True,
                        short_path_generated=True,
                        hardlink_created=True,
                        hardlink_readable=True)
        self.nt = dict(self.win,
                       schema="filesystem4-nt144-reference-v1",
                       hardlink_created=None,
                       hardlink_readable=None,
                       probe_cleaned_up=True)

    def test_unimplemented_hardlink_cannot_count_as_parity(self):
        result = compare(self.win, self.nt)
        self.assertFalse(result["complete"])
        self.assertEqual(result["differences"], [])
        self.assertEqual(set(result["unverified"]),
                         {"hardlink_created", "hardlink_readable"})

    def test_failing_case_insensitivity_is_detected(self):
        self.nt["case_insensitive_exact_name"] = False
        result = compare(self.win, self.nt)
        self.assertEqual(result["differences"][0]["field"],
                         "case_insensitive_exact_name")

    def test_probe_must_have_cleaned_up(self):
        self.nt["probe_cleaned_up"] = False
        with self.assertRaises(ValueError):
            compare(self.win, self.nt)


if __name__ == "__main__":
    unittest.main()
