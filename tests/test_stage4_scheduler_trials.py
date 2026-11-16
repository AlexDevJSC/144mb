# SPDX-License-Identifier: MIT
"""Synthetic parser/decision regression. Does not replace host measurements."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load_script(path, name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


trials = load_script("stage4_scheduler_trials.py", "stage4_scheduler_trials")
assess = load_script("assess-stage4-scheduler-trials.py", "stage4_scheduler_assess")


class RepeatedSchedulerTests(unittest.TestCase):
    def benchmark_report(self, platform="LINUX_HOST", duration=200):
        return "\n".join([
            "SCHEMA=ntnative.stage4.quantum-observation.v1",
            f"PLATFORM={platform}",
            "PINNED_ONE_CPU=YES", "CONTENDERS=2",
            f"REQUESTED_DURATION_MS={duration}",
            "WALL_NS=200000000", "THREAD0_CPU_NS=99000000",
            "THREAD1_CPU_NS=97000000", "WAKE0_US=100",
            "WAKE1_US=32000", "FAIRNESS_RATIO=0.97980",
            "NT_QUANTUM_VERIFIED=NO",
            "SCHEDULER_EQUIVALENCE=NOT_DEMONSTRATED",
            "RESULT=PASS",
        ]) + "\n"

    def test_single_32ms_sample_cannot_be_a_p95_conclusion(self):
        report = trials.parse_observation(self.benchmark_report(),
                                          "LINUX_HOST", 200)
        self.assertEqual(report["worst_first_run_us"], 32000)
        self.assertEqual(trials.quantile([100] * 99 + [32000], 0.95), 100)
        self.assertEqual(trials.quantile([100] * 99 + [32000], 0.99), 100)

    def test_bootstrap_is_seeded_and_bounded(self):
        data = [100] * 96 + [34000] * 4
        a = trials.bootstrap_p95_ci(data)
        self.assertEqual(a, trials.bootstrap_p95_ci(data))
        self.assertLessEqual(a[0], a[1])

    def test_fail_closed_on_fake_quantum_or_malformed_duration(self):
        with self.assertRaises(ValueError):
            trials.parse_observation(self.benchmark_report().replace(
                "NT_QUANTUM_VERIFIED=NO", "NT_QUANTUM_VERIFIED=YES"),
                "LINUX_HOST", 200)
        with self.assertRaises(ValueError):
            trials.parse_observation(self.benchmark_report(), "LINUX_HOST", 500)

    def test_decision_is_not_a_claim_of_nt_parity(self):
        stable = {
            "interactive_200ms": {"first_run_us": {"p95": 33000}},
            "sustained_500ms": {"fairness": {"median": 0.92}},
        }
        result = assess.decide(stable, stable)
        self.assertEqual(result["decision"],
                         "PROCEED_EXTERNAL_THREAD_BOUNDARY_NO_REWRITE_INDICATED")
        self.assertEqual(result["guest_runtime_scheduler_impact"], "NOT_MEASURED")
        unstable = {
            "interactive_200ms": {"first_run_us": {"p95": 34000}},
            "sustained_500ms": {"fairness": {"median": 0.70}},
        }
        self.assertIn("INVESTIGATE", assess.decide(stable, unstable)["decision"])

    def test_bad_trial_count_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory) / "fake.json"
            p.write_text(json.dumps({
                "schema": "ntnative.stage4.scheduler-trials.v1",
                "platform": "LINUX_HOST",
                "nt_quantum_parity": "NOT_DEMONSTRATED",
                "profiles": {
                    "interactive_200ms": {"summary": {"trials": 100},
                                          "observations": []},
                    "sustained_500ms": {"summary": {"trials": 50},
                                        "observations": []},
                },
            }), encoding="utf-8")
            with self.assertRaises(ValueError):
                assess.load(p, "LINUX_HOST")


if __name__ == "__main__":
    unittest.main()
