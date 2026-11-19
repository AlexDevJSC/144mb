#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fail-closed evidence join of identical Windows-source fixtures on two OSes.

A Windows fixture may return nonzero because the fixture explicitly
expects an NT144 limitation. That is a measurable difference, not
necessarily a Windows API defect. Never infer scheduling policy parity.
"""
import argparse
import json
import re
from pathlib import Path

SCHEMA = "ntnative.stage4.priority-affinity.functional.v1"
FIELDS = (
    "THREAD_GROUP",
    "PROCESS_AFFINITY",
    "PRIORITY_CLASS",
    "PRIORITY_BOOST_BOUNDARY",
)
DETAILS = {
    "THREAD_GROUP": "group affinity, legacy group-relative masks, rights and topology",
    "PROCESS_AFFINITY": "process mask, handle rights, live worker propagation",
    "PRIORITY_CLASS": "process class, relative thread priority, NT base and denial semantics",
    "PRIORITY_BOOST_BOUNDARY": "availability and denial of process/thread boost controls",
}
# These identifiers come from numbered 'return' statements in the exact
# source fixtures. A Windows fixture's nonzero result can mean that the
# test assumes an NT144 restriction, or that the hosted runner starts with
# a different default class; it is NOT automatically a runtime defect.
KNOWN_NATIVE_EXITS = {
    "THREAD_GROUP": {
        10: "The native Windows CreateThread(CREATE_SUSPENDED) call failed",
    },
    "PROCESS_AFFINITY": {
        10: "GetProcessAffinityMask failed with an error not expected by the fixture",
    },
    "PRIORITY_CLASS": {
        10: "Initial Windows process priority class was not NORMAL_PRIORITY_CLASS",
        14: "Windows did not return the fixture's expected REALTIME/NOT_SUPPORTED result",
        20: "Windows did not return NT144's expected multi-thread priority transition denial",
    },
    "PRIORITY_BOOST_BOUNDARY": {
        10: "GetProcessPriorityBoost did not return NT144's unsupported-path contract",
        11: "SetProcessPriorityBoost did not return NT144's unsupported-path contract",
        13: "GetThreadPriorityBoost did not return NT144's unsupported-path contract",
        14: "SetThreadPriorityBoost did not return NT144's unsupported-path contract",
    },
}


def facts(text: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for number, line in enumerate(text.splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise ValueError(f"line {number}: not KEY=VALUE")
        name, value = line.split("=", 1)
        if not re.fullmatch(r"[A-Z][A-Z0-9_]*", name) or name in result:
            raise ValueError(f"line {number}: invalid or duplicate key {name!r}")
        result[name] = value
    if result.get("SCHEMA") != SCHEMA:
        raise ValueError("wrong functional scheduler schema")
    if not re.fullmatch(r"[0-9a-fA-F]{40}", result.get("COMMIT", "")):
        raise ValueError("missing source commit SHA")
    if result.get("NT_SCHEDULER_PARITY_VERIFIED") != "NO":
        raise ValueError("report must not assert global NT scheduler equivalence")
    return result


def compare(windows_text: str, linux_text: str) -> dict:
    native = facts(windows_text)
    linux = facts(linux_text)
    if native.get("PLATFORM") != "WINDOWS_NATIVE":
        raise ValueError("reference is not a native Windows kernel execution")
    if linux.get("PLATFORM") != "LINUX_GUEST_PE":
        raise ValueError("Linux guest PE evidence missing")
    if linux.get("PHYSICAL_FEDORA_VERIFIED") != "NO":
        raise ValueError("hosted Linux probe cannot attest physical Fedora")
    if native["COMMIT"] != linux["COMMIT"]:
        raise ValueError("Windows and Linux reports come from different source commits")

    checks: dict[str, dict] = {}
    for fixture in FIELDS:
        raw = native.get(f"{fixture}_EXIT")
        if raw is None or not re.fullmatch(r"-?[0-9]+", raw):
            raise ValueError(f"missing native Windows exit code for {fixture}")
        exit_code = int(raw)
        status = linux.get(f"{fixture}_RESULT")
        if status not in ("PASS", "FAIL"):
            raise ValueError(f"missing Linux PE result for {fixture}")
        if status == "FAIL":
            assessment = "NOT_DEMONSTRATED"
        elif exit_code == 0:
            assessment = "SAME_SOURCE_CONTRACT_CONSISTENT"
        else:
            assessment = "OBSERVED_DIFFERENCE_CHECK_FIXTURE_ASSUMPTIONS"
        checks[fixture.lower()] = {
            "scope": DETAILS[fixture],
            "native_windows_exit": exit_code,
            "native_exit_interpretation": (
                "All tested fixture assertions passed"
                if exit_code == 0 else
                KNOWN_NATIVE_EXITS.get(fixture, {}).get(
                    exit_code, "Consult numbered returns in this fixture source"
                )
            ),
            "linux_guest_pe": status,
            "assessment": assessment,
            "same_fixture": True,
            "limitation": (
                "A nonzero native exit can mean the fixture intentionally "
                "requires NT144 limitations not present in Windows; "
                "inspect that fixture's numbered return codes"
            ),
        }

    return {
        "schema": "ntnative.stage4.priority-affinity.compare.v1",
        "source_commit": native["COMMIT"],
        "native_windows_kernel_observed": True,
        "linux_hosted_pe_observed": True,
        "physical_fedora_verified": False,
        "windows_physical_hardware_verified": False,
        "tests": checks,
        "nt_priority_policy_parity_verified": False,
        "nt_dynamic_boost_parity_verified": False,
        "nt_quantum_parity_verified": False,
        "nt_processor_group_topology_parity_verified": False,
        "nt_scheduler_parity_verified": False,
        "overall": "FUNCTIONAL_OBSERVATIONS_ONLY_NO_SCHEDULER_PARITY",
        "limitations": (
            "Runtime uses Linux cpusets/nice and bounded optional boost. "
            "Windows version, host topology, privileges and VM scheduling "
            "differ. This test joins explicit API contracts from identical "
            "source fixtures, not fairness, quantum or dynamic NT priority."
        ),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--windows", type=Path, required=True)
    parser.add_argument("--linux", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = compare(
        args.windows.read_text(encoding="utf-8-sig"),
        args.linux.read_text(encoding="utf-8-sig"),
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print("STAGE4_FUNCTIONAL_SCHEDULER=FUNCTIONAL_OBSERVATIONS_ONLY_NO_SCHEDULER_PARITY")
    for name, item in report["tests"].items():
        print(f"{name}={item['assessment']} windows_exit={item['native_windows_exit']}")
    print(f"ARTIFACT={args.output}")


if __name__ == "__main__":
    main()
