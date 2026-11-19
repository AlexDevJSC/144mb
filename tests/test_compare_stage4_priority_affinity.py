# SPDX-License-Identifier: MIT
"""Synthetic parser fixtures only: they are NOT native Windows evidence."""
import importlib.util
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "compare-stage4-priority-affinity.py"
spec = importlib.util.spec_from_file_location("stage4_functional", SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

SHA = "0a" * 20


def report(platform, **overrides):
    facts = {
        "SCHEMA": module.SCHEMA,
        "COMMIT": SHA,
        "PLATFORM": platform,
        "NT_SCHEDULER_PARITY_VERIFIED": "NO",
    }
    if platform == "WINDOWS_NATIVE":
        facts.update({f"{key}_EXIT": "0" for key in module.FIELDS})
    else:
        facts["PHYSICAL_FEDORA_VERIFIED"] = "NO"
        facts.update({f"{key}_RESULT": "PASS" for key in module.FIELDS})
    facts.update(overrides)
    return "".join(f"{key}={value}\n" for key, value in facts.items())


class FunctionalSchedulerTests(unittest.TestCase):
    def test_all_common_fixtures_can_pass_without_equivalence(self):
        result = module.compare(report("WINDOWS_NATIVE"), report("LINUX_GUEST_PE"))
        self.assertEqual(len(result["tests"]), 4)
        self.assertTrue(all(x["assessment"] == "SAME_SOURCE_CONTRACT_CONSISTENT"
                            for x in result["tests"].values()))
        self.assertFalse(result["nt_scheduler_parity_verified"])
        self.assertFalse(result["physical_fedora_verified"])

    def test_expected_windows_native_difference_is_not_hidden(self):
        result = module.compare(
            report("WINDOWS_NATIVE", PRIORITY_BOOST_BOUNDARY_EXIT="10"),
            report("LINUX_GUEST_PE"),
        )
        field = result["tests"]["priority_boost_boundary"]
        self.assertEqual(field["native_windows_exit"], 10)
        self.assertEqual(field["assessment"],
                         "OBSERVED_DIFFERENCE_CHECK_FIXTURE_ASSUMPTIONS")
        self.assertFalse(result["nt_dynamic_boost_parity_verified"])

    def test_linux_fail_never_gains_approval_from_windows(self):
        result = module.compare(
            report("WINDOWS_NATIVE"),
            report("LINUX_GUEST_PE", THREAD_GROUP_RESULT="FAIL"),
        )
        self.assertEqual(result["tests"]["thread_group"]["assessment"],
                         "NOT_DEMONSTRATED")

    def test_commit_mismatch_fails_closed(self):
        with self.assertRaisesRegex(ValueError, "different source commits"):
            module.compare(
                report("WINDOWS_NATIVE"),
                report("LINUX_GUEST_PE", COMMIT="ef" * 20),
            )

    def test_provenance_and_claims_fail_closed(self):
        with self.assertRaises(ValueError):
            module.compare(report("LINUX_GUEST_PE"), report("LINUX_GUEST_PE"))
        with self.assertRaises(ValueError):
            module.compare(report("WINDOWS_NATIVE"),
                           report("LINUX_GUEST_PE", PHYSICAL_FEDORA_VERIFIED="YES"))
        with self.assertRaises(ValueError):
            module.compare(report("WINDOWS_NATIVE", NT_SCHEDULER_PARITY_VERIFIED="YES"),
                           report("LINUX_GUEST_PE"))
        with self.assertRaises(ValueError):
            module.compare(report("WINDOWS_NATIVE", PROCESS_AFFINITY_EXIT="SKIP"),
                           report("LINUX_GUEST_PE"))
        with self.assertRaises(ValueError):
            module.compare(report("WINDOWS_NATIVE") + "PRIORITY_CLASS_EXIT=0\n",
                           report("LINUX_GUEST_PE"))


if __name__ == "__main__":
    unittest.main()
