#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare actual Windows NTFS evidence with NT144 Win32 runtime observations.

A schema-valid JSON is NOT proof of genuine Windows hardware or equivalence;
the operator must attest provenance. Return 3 until every field is supported.
"""
import json
from pathlib import Path
import sys
from fs4_windows_reference_validate import check as validate_windows

FIELDS = (
    "case_insensitive_exact_name",
    "rename_preserves_content",
    "readonly_attribute_visible",
    "short_path_generated",
    "hardlink_created",
    "hardlink_readable",
)


def compare(windows: dict, nt144: dict) -> dict:
    if nt144.get("schema") != "filesystem4-nt144-reference-v1":
        raise ValueError("Invalid NT144 probe schema")
    if nt144.get("probe_cleaned_up") is not True:
        raise ValueError("NT144 probe left disposable test files behind")
    diffs = []
    missing = []
    matches = []
    for key in FIELDS:
        left = windows.get(key)
        right = nt144.get(key)
        if type(left) is not bool:
            raise ValueError(f"Windows reference missing {key}")
        if right is None:
            missing.append(key)
        elif type(right) is not bool:
            raise ValueError(f"Invalid NT144 field {key}")
        elif left == right:
            matches.append(key)
        else:
            diffs.append({"field": key, "windows_ntfs": left,
                          "nt144_runtime": right})
    return {
        "schema": "filesystem4-ntfs-comparison-v1",
        "matches": matches,
        "differences": diffs,
        "unverified": missing,
        "complete": not diffs and not missing,
        "warning": "Operator must verify real Windows provenance, SELinux, power-loss and PE sandbox independently",
    }


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("Usage: fs4_ntfs_diff.py WINDOWS_REFERENCE.json NT144_REFERENCE.json",
              file=sys.stderr)
        return 2
    try:
        windows = validate_windows(argv[0])
        raw = Path(argv[1])
        if not raw.is_file() or raw.stat().st_size > 65536:
            raise ValueError("Invalid NT144 report")
        nt144 = json.loads(raw.read_text(encoding="utf-8-sig"))
        if not isinstance(nt144, dict):
            raise ValueError("NT144 report must be an object")
        diff = compare(windows, nt144)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"REFUSED: {error}", file=sys.stderr)
        return 3
    print(json.dumps(diff, ensure_ascii=False, indent=2, sort_keys=True))
    return 0 if diff["complete"] else 3


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
