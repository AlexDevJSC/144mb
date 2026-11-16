#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run repeated independent Windows/Linux same-source scheduler observations.

Per-run measurements come from the native scheduler of the host that executes
the benchmark (NOT from the nt-native guest and NOT an NT quantum measurement).
Two bounded profiles avoid conclusions from a single 32-ms observation.
Bootstrap is a seeded statistical simulation of sampling uncertainty ONLY.
"""
import argparse
import json
import math
import os
from pathlib import Path
import random
import subprocess

SCHEMA = "ntnative.stage4.quantum-observation.v1"
SEED = 0x144
BOOTSTRAP = 1000


def quantile(samples, fraction):
    if not samples or not 0 < fraction <= 1:
        raise ValueError("quantile requires nonempty samples and 0 < fraction <= 1")
    ordered = sorted(samples)
    return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)]


def bootstrap_p95_ci(samples, *, repetitions=BOOTSTRAP, seed=SEED):
    if len(samples) < 20:
        raise ValueError("need >=20 independent trials for bootstrap")
    generator = random.Random(seed)
    bootstrap = [
        quantile([samples[generator.randrange(len(samples))]
                  for _ in range(len(samples))], 0.95)
        for _ in range(repetitions)
    ]
    return [quantile(bootstrap, 0.025), quantile(bootstrap, 0.975)]


def parse_observation(stdout, expected_platform, duration):
    fields = {}
    for line in stdout.splitlines():
        if "=" not in line:
            raise ValueError("malformed output line")
        name, value = line.split("=", 1)
        if not name or name in fields:
            raise ValueError("duplicate/empty observation field")
        fields[name] = value
    required = ("SCHEMA", "PLATFORM", "PINNED_ONE_CPU", "CONTENDERS",
                "REQUESTED_DURATION_MS", "WALL_NS",
                "THREAD0_CPU_NS", "THREAD1_CPU_NS",
                "WAKE0_US", "WAKE1_US", "FAIRNESS_RATIO",
                "NT_QUANTUM_VERIFIED", "SCHEDULER_EQUIVALENCE", "RESULT")
    if any(name not in fields for name in required):
        raise ValueError("observation missing required fields")
    if (fields["SCHEMA"] != SCHEMA or
        fields["PLATFORM"] != expected_platform or
        fields["PINNED_ONE_CPU"] != "YES" or
        fields["CONTENDERS"] != "2" or
        fields["REQUESTED_DURATION_MS"] != str(duration) or
        fields["NT_QUANTUM_VERIFIED"] != "NO" or
        fields["SCHEDULER_EQUIVALENCE"] != "NOT_DEMONSTRATED" or
        fields["RESULT"] != "PASS"):
        raise ValueError("invalid or misleading native scheduler observation")
    numeric = {}
    for name in ("WALL_NS", "THREAD0_CPU_NS", "THREAD1_CPU_NS",
                 "WAKE0_US", "WAKE1_US"):
        value = int(fields[name])
        if value < 0 or (name not in ("WAKE0_US", "WAKE1_US") and value == 0):
            raise ValueError(f"invalid measurement: {name}={value}")
        numeric[name.lower()] = value
    fairness = float(fields["FAIRNESS_RATIO"])
    if not math.isfinite(fairness) or not 0 < fairness <= 1:
        raise ValueError("invalid CPU fairness ratio")
    numeric["fairness_ratio"] = fairness
    numeric["worst_first_run_us"] = max(
        numeric["wake0_us"], numeric["wake1_us"])
    return numeric


def summarize(trials):
    wakes = [trial["worst_first_run_us"] for trial in trials]
    fairness = [trial["fairness_ratio"] for trial in trials]
    # Hypothesis tests are intentionally absent. Windows and Linux runner
    # CPUs are not controlled/matched, and first-run ≠ input latency.
    return {
        "trials": len(trials),
        "first_run_us": {
            "median": quantile(wakes, 0.50),
            "p95": quantile(wakes, 0.95),
            "p99": quantile(wakes, 0.99),
            "max": max(wakes),
            "bootstrap_p95_95pct_ci": bootstrap_p95_ci(wakes),
        },
        "fairness": {
            "median": quantile(fairness, 0.50),
            "p05": quantile(fairness, 0.05),
            "min": min(fairness),
            "bootstrap_p05_95pct_ci": bootstrap_p95_ci(
                [-value for value in fairness], seed=SEED + 1)[::-1],
        },
    }


def run_profile(executable, platform, *, count, duration_ms):
    trials = []
    for index in range(count):
        environment = os.environ.copy()
        environment["NT_STAGE4_DURATION_MS"] = str(duration_ms)
        result = subprocess.run([str(executable)], capture_output=True,
                                text=True, timeout=20, env=environment,
                                check=False)
        if result.returncode != 0:
            raise RuntimeError(f"{platform} run {index + 1}: exit "
                               f"{result.returncode}: {result.stderr} / "
                               f"{result.stdout}")
        try:
            trials.append(parse_observation(result.stdout, platform, duration_ms))
        except (KeyError, ValueError) as error:
            raise RuntimeError(
                f"{platform} run {index + 1}: {error}") from error
    return trials


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--platform", choices=("WINDOWS_NATIVE", "LINUX_HOST"),
                        required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--short-count", type=int, default=100)
    parser.add_argument("--long-count", type=int, default=50)
    args = parser.parse_args()
    if not (20 <= args.short_count <= 500 and 20 <= args.long_count <= 500):
        parser.error("trial counts must be 20..500")
    profiles = {"interactive_200ms": (args.short_count, 200),
                "sustained_500ms": (args.long_count, 500)}
    output = {
        "schema": "ntnative.stage4.scheduler-trials.v1",
        "platform": args.platform,
        "bootstrap_seed": SEED,
        "bootstrap_resamples": BOOTSTRAP,
        "profiles": {},
        "nt_quantum_parity": "NOT_DEMONSTRATED",
        "kernel_rewrite_justified": "NOT_DEMONSTRATED",
        "caveat": ("Host-native CPU contention is not PE-runtime performance. "
                   "Windows and Linux CI machines differ. Bootstraps quantify "
                   "sampling variability, not causal attribution or NT quantum."),
    }
    for profile, (count, duration) in profiles.items():
        observations = run_profile(args.executable, args.platform,
                                   count=count, duration_ms=duration)
        output["profiles"][profile] = {
            "duration_ms": duration,
            "summary": summarize(observations),
            "observations": observations,
        }
        stats = output["profiles"][profile]["summary"]
        print(f"{args.platform} {profile}: n={count}, "
              f"p95 first-run={stats['first_run_us']['p95']}us, "
              f"p99={stats['first_run_us']['p99']}us, "
              f"median fairness={stats['fairness']['median']:.4f}",
              flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
