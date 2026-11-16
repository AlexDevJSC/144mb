#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Interpret independent Windows/Linux native scheduler trial distributions.

Do not use this tool to certify NT quantum or PE runtime performance.
The heuristics identify whether native Linux measurements give a concrete
reason to investigate host scheduling, not whether Windows ≈ Linux.
"""
import argparse
import json
import math
from pathlib import Path

PROFILES = ("interactive_200ms", "sustained_500ms")


def load(path, platform):
    payload = json.loads(path.read_text(encoding="utf-8"))
    if payload.get("schema") != "ntnative.stage4.scheduler-trials.v1":
        raise ValueError(f"{path}: invalid schema")
    if payload.get("platform") != platform:
        raise ValueError(f"{path}: wrong platform")
    if payload.get("nt_quantum_parity") != "NOT_DEMONSTRATED":
        raise ValueError(f"{path}: quantum claim without evidence")
    summary = {}
    for profile in PROFILES:
        data = payload.get("profiles", {}).get(profile, {})
        n = 100 if profile == PROFILES[0] else 50
        if data.get("summary", {}).get("trials", -1) != n or len(
                data.get("observations", [])) != n:
            raise ValueError(f"{path}: wrong number of {profile} trials")
        stats = data["summary"]
        wakes = stats["first_run_us"]
        fairness = stats["fairness"]
        if (not 0 <= wakes["median"] <= wakes["p95"] <=
                wakes["p99"] <= wakes["max"] or
                not 0 < fairness["min"] <= fairness["p05"] <=
                fairness["median"] <= 1):
            raise ValueError(f"{path}: invalid percentile order")
        if len(wakes["bootstrap_p95_95pct_ci"]) != 2:
            raise ValueError(f"{path}: missing uncertainty interval")
        summary[profile] = stats
    return summary


def decide(windows, linux):
    # 33 ms / 90% are project-specific exploratory investigation thresholds.
    # One isolated 32.86 ms Windows result was not a legitimate causal proof.
    short_wake_p95_ms = linux[PROFILES[0]]["first_run_us"]["p95"] / 1000
    sustained_fairness = linux[PROFILES[1]]["fairness"]["median"]
    investigate = short_wake_p95_ms > 33 or sustained_fairness < 0.90
    action = ("INVESTIGATE_HOST_MEASUREMENTS_NO_REWRITE_AUTHORIZED" if investigate
              else "PROCEED_EXTERNAL_THREAD_BOUNDARY_NO_REWRITE_INDICATED")
    return {
        "schema": "ntnative.stage4.scheduler-trials-assessment.v1",
        "windows_native": windows,
        "linux_host": linux,
        "exploratory_thresholds": {
            "linux_interactive_first_run_p95_max_ms": 33,
            "linux_sustained_fairness_median_min": 0.90,
        },
        "linux_p95_first_run_ms": short_wake_p95_ms,
        "linux_sustained_fairness_median": sustained_fairness,
        "decision": action,
        "nt_quantum_parity": "NOT_DEMONSTRATED",
        "guest_runtime_scheduler_impact": "NOT_MEASURED",
        "hardware_multigroup_physical": "NOT_TESTED",
        "next_priority": "6.EXTERNAL_GUEST_THREAD_BROKER_BOUNDARY",
        "limitations": ("Thresholds are an engineering triage heuristic, not "
                        "Microsoft specifications. CPU, hypervisor, power policy, "
                        "NT quantum and workloads are not matched. Windows host "
                        "data are descriptive, never a target of fixed milliseconds. "
                        "No general rewrite can be authorized by these samples."),
    }


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--windows", type=Path, required=True)
    cli.add_argument("--linux", type=Path, required=True)
    cli.add_argument("--output", type=Path, required=True)
    args = cli.parse_args()
    windows = load(args.windows, "WINDOWS_NATIVE")
    linux = load(args.linux, "LINUX_HOST")
    report = decide(windows, linux)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("Decision: " + report["decision"])
    print("Linux short P95: %.3f ms; sustained fairness median: %.4f"
          % (report["linux_p95_first_run_ms"],
             report["linux_sustained_fairness_median"]))
    print("NT quantum equivalence: NOT_DEMONSTRATED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
