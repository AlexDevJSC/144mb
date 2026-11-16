# SPDX-License-Identifier: MIT
"""Parser invariants for synthetic fixtures; no invented Windows measurement."""
import importlib.util
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "compare-stage4-quantum.py"
spec = importlib.util.spec_from_file_location("stage4_quantum_compare", SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class CompareQuantumTests(unittest.TestCase):
    def make_report(self, path, platform="WINDOWS_NATIVE", **changes):
        facts = {
            "SCHEMA": module.SCHEMA, "PLATFORM": platform,
            "PINNED_ONE_CPU": "YES", "CONTENDERS": "2",
            "WALL_NS": "500000000",
            "THREAD0_CPU_NS": "210000000",
            "THREAD1_CPU_NS": "220000000",
            "WAKE0_US": "40", "WAKE1_US": "100",
            "FAIRNESS_RATIO": "0.95455",
            "NT_QUANTUM_VERIFIED": "NO",
            "SCHEDULER_EQUIVALENCE": "NOT_DEMONSTRATED",
            "RESULT": "PASS",
        }
        facts.update(changes)
        path.write_text("".join(f"{k}={v}\n" for k, v in facts.items()),
                        encoding="utf-8")
        return path

    def test_complete_observation_is_not_a_claim_of_parity(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.make_report(Path(directory) / "one.txt")
            report = module.parse(path, "WINDOWS_NATIVE")
            self.assertEqual(report["wall_ns"], 500000000)
            self.assertEqual(report["fairness_ratio"], 0.95455)

    def test_wrong_platform_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.make_report(Path(directory) / "one.txt",
                                    platform="LINUX_HOST")
            with self.assertRaises(ValueError):
                module.parse(path, "WINDOWS_NATIVE")

    def test_boost_or_quantum_claim_cannot_be_smuggled_into_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.make_report(Path(directory) / "one.txt",
                                    NT_QUANTUM_VERIFIED="YES")
            with self.assertRaises(ValueError):
                module.parse(path, "WINDOWS_NATIVE")

    def test_zero_cpu_time_is_invalid(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.make_report(Path(directory) / "one.txt",
                                    THREAD1_CPU_NS="0")
            with self.assertRaises(ValueError):
                module.parse(path, "WINDOWS_NATIVE")

    def test_bad_fairness_and_duplicate_keys_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.make_report(Path(directory) / "one.txt",
                                    FAIRNESS_RATIO="nan")
            with self.assertRaises(ValueError):
                module.parse(path, "WINDOWS_NATIVE")
            path.write_text(path.read_text() + "PLATFORM=WINDOWS_NATIVE\n")
            with self.assertRaises(ValueError):
                module.parse(path, "WINDOWS_NATIVE")


if __name__ == "__main__":
    unittest.main()
