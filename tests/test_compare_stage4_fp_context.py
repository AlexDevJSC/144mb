# SPDX-License-Identifier: MIT
"""Synthetic input tests, not a claim of running Windows locally."""
import importlib.util
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "compare-stage4-fp-context.py"
spec = importlib.util.spec_from_file_location("stage4_fp_compare", SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def reference(**overrides):
    facts = {
        "SCHEMA": "ntnative.stage4.fp-context.v1",
        "PLATFORM": "WINDOWS_NATIVE",
        "FP_CAPTURE": "PASS", "FP_WRITE": "PASS",
        "MXCSR_ROUNDTRIP": "PASS", "FP_RESTORE": "PASS",
        "THREAD_EXIT": "PASS", "GET_ERROR": "0", "SET_ERROR": "0",
        "NT_CONTEXT_PARITY_VERIFIED": "NO",
    }
    facts.update(overrides)
    return "".join(f"{k}={v}\n" for k, v in facts.items())


LINUX_FP = (
    "PASS real runtime Win32 entrypoints: SuspendThread nested counts, "
    "Get/SetThreadContext AMD64 integer and legacy FP kernel readback, "
    "ResumeThread, HANDLE rights\n"
)
LINUX_OLD = (
    "PASS real runtime Win32 entrypoints: SuspendThread nested counts, "
    "Get/SetThreadContext AMD64 integer readback, ResumeThread\n"
)


class Stage4FPReferenceTests(unittest.TestCase):
    def test_two_positive_observations_are_not_global_parity(self):
        result = module.compare(reference(), LINUX_FP)
        self.assertEqual(result["result"], "CONSISTENT_NARROW_PROFILE")
        self.assertFalse(result["nt_context_parity_verified"])
        self.assertFalse(result["physical_fedora_verified"])

    def test_original_read_only_linux_does_not_pretend_set_success(self):
        result = module.compare(reference(), LINUX_OLD)
        self.assertEqual(result["result"], "NOT_DEMONSTRATED")
        self.assertFalse(result["linux_legacy_fp_roundtrip_observed"])

    def test_windows_failure_is_preserved(self):
        result = module.compare(reference(FP_WRITE="FAIL"), LINUX_FP)
        self.assertEqual(result["native_windows"]["FP_WRITE"], "FAIL")
        self.assertEqual(result["result"], "NOT_DEMONSTRATED")

    def test_bad_or_duplicate_evidence_fails_closed(self):
        with self.assertRaises(ValueError):
            module.compare(reference(PLATFORM="LINUX_HOST"), LINUX_FP)
        with self.assertRaises(ValueError):
            module.compare(reference(NT_CONTEXT_PARITY_VERIFIED="YES"), LINUX_FP)
        with self.assertRaises(ValueError):
            module.compare(reference() + "FP_WRITE=PASS\n", LINUX_FP)
        with self.assertRaises(ValueError):
            module.compare(reference(FP_RESTORE="SKIP"), LINUX_FP)


if __name__ == "__main__":
    unittest.main()
