#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare contract observations, never raw CPU IDs or kernel time slices.
Outputs a machine-readable gap report; exit 3 means incomplete or divergent.
"""
import argparse
import json
from pathlib import Path


def read_facts(path: Path):
    result = {}
    for index, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise ValueError(f"{path}:{index}: expected KEY=VALUE")
        key, value = line.split("=", 1)
        if not key or key in result:
            raise ValueError(f"{path}:{index}: empty/duplicate key {key!r}")
        result[key] = value
    if result.get("SCHEMA") != "ntnative.stage4.scheduler.v1":
        raise ValueError(f"{path}: unexpected schema")
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--windows", required=True, type=Path)
    parser.add_argument("--fedora", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--require-multigroup", action="store_true")
    args = parser.parse_args()
    windows, fedora = read_facts(args.windows), read_facts(args.fedora)
    if windows.get("PLATFORM") != "WINDOWS_NATIVE":
        raise ValueError("Windows data must come from a native Windows executable")
    if fedora.get("PLATFORM") != "FEDORA_KDE_PHYSICAL":
        raise ValueError("Fedora data must come from strict physical acceptance")

    observations = {}
    for name, data in (("windows", windows), ("fedora", fedora)):
        observations[name] = {
            "active_cpus": int(data["ACTIVE_CPUS"]),
            "groups": int(data["GROUPS"]),
            "thread_group": data.get("THREAD_GROUP", "MISSING"),
            "group_transition": data.get("CROSS_GROUP",
                                         data.get("MULTIGROUP", "MISSING")),
            "legacy_group_relative": data.get("LEGACY_MASK_PRIMARY_GROUP", "MISSING"),
            "boost": data.get("DYNAMIC_BOOST", "MISSING"),
            "quantum": data.get("QUANTUM", "MISSING"),
            "external_threads": data.get("EXTERNAL_THREADS", "MISSING"),
        }

    multi = all(observations[os]["groups"] > 1 and
                observations[os]["active_cpus"] >= 65 and
                observations[os]["group_transition"] == "PASS"
                for os in ("windows", "fedora"))
    relative = all(observations[os]["legacy_group_relative"] == "PASS"
                   for os in ("windows", "fedora"))
    thread = all(observations[os]["thread_group"] == "PASS"
                 for os in ("windows", "fedora"))
    results = {
        "multigroup_cross_host": "OBSERVED" if multi else "INCOMPLETE",
        "thread_group_contract": "OBSERVED" if thread else "INCOMPLETE",
        "legacy_mask_contract": "OBSERVED" if relative else "INCOMPLETE",
        "windows_11_primary_group_default": "UNVERIFIED_NOT_EMULATED",
        "priority_boost_equivalence": "GAP_RUNTIME_NO_DYNAMIC_BOOST",
        "nt_quantum_equivalence": "UNVERIFIED_NO_USERMODE_QUANTUM_CONTRACT",
        "external_thread_broker": "MISSING_REQUIRES_AUTHENTICATED_IPC",
        "cross_os_scheduler_parity": "NOT_DEMONSTRATED",
    }
    if args.require_multigroup and not multi:
        results["required_multigroup_acceptance"] = "FAILED_NO_HARDWARE_EVIDENCE"
    payload = {
        "schema": "ntnative.stage4.scheduler.differential.v1",
        "windows": observations["windows"],
        "fedora": observations["fedora"],
        "contracts": results,
        "note": ("Passing these probes demonstrates bounded observable contracts, "
                 "not scheduling fairness, latency or full Windows NT parity."),
    }
    formatted = json.dumps(payload, indent=2, ensure_ascii=False) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(formatted, encoding="utf-8")
    print(formatted, end="")
    return 3 if any(value in {"INCOMPLETE", "FAILED_NO_HARDWARE_EVIDENCE"}
                    for value in results.values()) or args.require_multigroup and not multi else 0


if __name__ == "__main__":
    raise SystemExit(main())
