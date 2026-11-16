#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare same-source Windows/Linux scheduler observations, never NT quantum.

Exit 0 means valid comparable observation metadata, NOT scheduler equivalence.
Exit 3 for explicitly requested NT equivalence, which is not demonstrated.
Do not compare Linux CPU numbers with Windows group/slot identifiers.
"""
import argparse
import json
import math
from pathlib import Path

SCHEMA = "ntnative.stage4.quantum-observation.v1"


def parse(path: Path, expected_platform: str):
    values = {}
    for lineno, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise ValueError(f"{path}:{lineno}: malformed KEY=VALUE")
        k, v = line.split("=", 1)
        if not k or k in values:
            raise ValueError(f"{path}:{lineno}: duplicate/empty key")
        values[k] = v
    required = ("SCHEMA", "PLATFORM", "PINNED_ONE_CPU", "CONTENDERS",
                "WALL_NS", "THREAD0_CPU_NS", "THREAD1_CPU_NS",
                "WAKE0_US", "WAKE1_US", "FAIRNESS_RATIO",
                "NT_QUANTUM_VERIFIED", "SCHEDULER_EQUIVALENCE", "RESULT")
    missing = [name for name in required if name not in values]
    if missing:
        raise ValueError(f"{path}: missing {missing}")
    if values["SCHEMA"] != SCHEMA or values["PLATFORM"] != expected_platform:
        raise ValueError(f"{path}: wrong schema or platform")
    if (values["PINNED_ONE_CPU"] != "YES" or values["CONTENDERS"] != "2"
            or values["RESULT"] != "PASS"
            or values["NT_QUANTUM_VERIFIED"] != "NO"
            or values["SCHEDULER_EQUIVALENCE"] != "NOT_DEMONSTRATED"):
        raise ValueError(f"{path}: invalid benchmark evidence")
    numeric = {}
    for key in ("WALL_NS", "THREAD0_CPU_NS", "THREAD1_CPU_NS", "WAKE0_US", "WAKE1_US"):
        value = int(values[key])
        if value < 0 or (key not in ("WAKE0_US", "WAKE1_US") and value == 0):
            raise ValueError(f"{path}: invalid {key}={value}")
        numeric[key.lower()] = value
    ratio = float(values["FAIRNESS_RATIO"])
    if not math.isfinite(ratio) or not (0 < ratio <= 1):
        raise ValueError(f"{path}: invalid fairness ratio")
    numeric["fairness_ratio"] = ratio
    return numeric


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--windows", type=Path, required=True)
    cli.add_argument("--linux", type=Path, required=True)
    cli.add_argument("--output", type=Path)
    cli.add_argument("--require-nt-equivalence", action="store_true")
    args = cli.parse_args()
    win = parse(args.windows, "WINDOWS_NATIVE")
    linux = parse(args.linux, "LINUX_HOST")
    payload = {
        "schema": "ntnative.stage4.quantum-differential.v1",
        "windows": win,
        "linux": linux,
        "observations": {
            "fairness_absolute_delta": round(abs(win["fairness_ratio"] -
                                                 linux["fairness_ratio"]), 5),
            "windows_wakeup_max_us": max(win["wake0_us"], win["wake1_us"]),
            "linux_wakeup_max_us": max(linux["wake0_us"], linux["wake1_us"]),
        },
        "contracts": {
            "both_used_one_pinned_cpu": "OBSERVED",
            "cpu_time_and_first_run_latency": "OBSERVED",
            "nt_quantum_equivalence": "NOT_DEMONSTRATED",
            "windows_dynamic_boost_parity": "NOT_DEMONSTRATED",
        },
        "disclaimer": ("The same C workload is measured on different host schedulers "
                       "and CPU hardware; absolute wall/CPU/wake values or a fairness "
                       "ratio cannot identify NT quantum length or establish parity."),
    }
    output = json.dumps(payload, indent=2, ensure_ascii=False) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output, encoding="utf-8")
    print(output, end="")
    return 3 if args.require_nt_equivalence else 0


if __name__ == "__main__":
    raise SystemExit(main())
