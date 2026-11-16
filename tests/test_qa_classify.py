# SPDX-License-Identifier: MIT
import importlib.util
import pathlib
import unittest

FILE = pathlib.Path(__file__).resolve().parents[1] / "scripts" / "qa_classify.py"
SPEC = importlib.util.spec_from_file_location("qa_classify", FILE)
assert SPEC and SPEC.loader
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


class QACheck(unittest.TestCase):
    def test_exit_zero_does_not_overrule_skip(self):
        self.assertEqual(module.classify(["PASS runtime", "SKIP positive boost"],
                                         ["PASS runtime"], [])["status"], "INCOMPLETE")

    def test_physical_hardware_skip_not_fabricated(self):
        self.assertEqual(module.classify(
            ["PASS sparse synthetic 80-CPU compact guest groups (NOT physical hardware)",
             "SKIP process group inheritance: host has one allowed Linux CPU group"],
            [], [])["status"], "INCOMPLETE")

    def test_fail_has_priority(self):
        self.assertEqual(module.classify(["SKIP optional", "FAIL PE child loader"],
                                         [], ["optional"])["status"], "FAIL")

    def test_missing_child_sentinel(self):
        self.assertEqual(module.classify(
            ["PASS PE preferences persist across two isolated processes"],
            ["PASS PE child launched inside the same Linux sandbox"], [])["status"],
            "INCOMPLETE")

    def test_complete_and_explicit_optional(self):
        self.assertEqual(module.classify(
            ["PASS PE child launched inside the same Linux sandbox",
             "SKIP native FluidSynth ABI: libfluidsynth.so.3 unavailable"],
            ["PASS PE child launched inside the same Linux sandbox"],
            ["native FluidSynth ABI"])["status"], "PASS")

    def test_real_make_failure(self):
        self.assertEqual(module.classify(["make: *** [Makefile:101: check] Error 2"],
                                         [], [])["status"], "FAIL")


if __name__ == "__main__":
    unittest.main()
