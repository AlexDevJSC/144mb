#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare *limited* native Windows and Linux-hosted PE FXSAVE evidence.

Different fixtures and environments are not a general NT ABI parity proof.
Any missing evidence yields NOT_DEMONSTRATED, never a fabricated PASS.
"""
import argparse
import json
import pathlib


REQUIRED = (
    "FP_CAPTURE", "FP_WRITE", "MXCSR_ROUNDTRIP", "FP_RESTORE", "THREAD_EXIT"
)


def parse_native(contents: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in contents.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key in result:
            raise ValueError(f"duplicate Windows reference field: {key}")
        result[key] = value
    if result.get("SCHEMA") != "ntnative.stage4.fp-context.v1":
        raise ValueError("invalid Windows reference schema")
    if result.get("PLATFORM") != "WINDOWS_NATIVE":
        raise ValueError("native Windows context evidence not present")
    if result.get("NT_CONTEXT_PARITY_VERIFIED") != "NO":
        raise ValueError("reference improperly claims NT parity")
    for field in REQUIRED:
        if result.get(field) not in ("PASS", "FAIL"):
            raise ValueError(f"missing result in Windows reference: {field}")
    return result


def compare(windows_contents: str, linux_log: str) -> dict:
    native = parse_native(windows_contents)
    linux_test_ran = "PASS real runtime Win32 entrypoints:" in linux_log
    linux_legacy_write = (
        linux_test_ran and
        "Get/SetThreadContext AMD64 integer and legacy FP kernel readback" in linux_log
    )
    windows_roundtrip = all(native[field] == "PASS" for field in REQUIRED)
    evidence = (
        "CONSISTENT_NARROW_PROFILE"
        if windows_roundtrip and linux_legacy_write
        else "NOT_DEMONSTRATED"
    )
    return {
        "schema": "ntnative.stage4.fp-context.compare.v1",
        "native_windows": {field: native[field] for field in REQUIRED},
        "linux_runtime_test_observed": linux_test_ran,
        "linux_legacy_fp_roundtrip_observed": linux_legacy_write,
        "result": evidence,
        "nt_context_parity_verified": False,
        "physical_fedora_verified": False,
        "limitations": (
            "Windows fixture uses a CREATE_SUSPENDED native thread; Linux "
            "runtime uses a ptrace-supervised PE thread stopped while "
            "executing guest code. CPU, kernel and XSTATE scope differ; "
            "no control/segment writes, arbitrary threads or SEH parity."
        ),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--windows", required=True, type=pathlib.Path)
    parser.add_argument("--linux-log", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()
    result = compare(
        args.windows.read_text(encoding="utf-8"),
        args.linux_log.read_text(encoding="utf-8"),
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"STAGE4_FP_COMPARISON={result['result']}")
    print(f"EVIDENCE={args.output}")


if __name__ == "__main__":
    main()
