#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate an untrusted Windows NTFS *reference* report's schema, not origin."""
import json
from pathlib import Path
import sys


def check(path: str) -> dict:
    source = Path(path)
    if not source.is_file() or source.stat().st_size > 65536:
        raise ValueError("Invalid reference file size or type")
    record = json.loads(source.read_text(encoding="utf-8-sig"))
    if not isinstance(record, dict):
        raise ValueError("Reference must be a JSON object")
    if record.get("schema") not in ("filesystem4-windows-reference-v1",
                                    "filesystem4-windows-reference-v2"):
        raise ValueError("Wrong Windows reference schema")
    if record.get("volume_filesystem") != "NTFS":
        raise ValueError("Reference must report actual NTFS")
    for name in ("case_insensitive_exact_name",
                 "rename_preserves_content", "hardlink_created",
                 "hardlink_readable", "readonly_attribute_visible",
                 "short_path_generated"):
        if type(record.get(name)) is not bool:
            raise ValueError(f"Missing or invalid boolean: {name}")
    if record["schema"] == "filesystem4-windows-reference-v2":
        for name in ("acl_descriptor_readable",
                     "acl_explicit_deny_enforced",
                     "junction_reparse_visible",
                     "symlink_reparse_visible"):
            if name not in record or (record[name] is not None and
                                     type(record[name]) is not bool):
                raise ValueError(f"Invalid v2 advanced NTFS field: {name}")
    if record.get("short_path") is not None and (
        not isinstance(record["short_path"], str) or
        len(record["short_path"]) > 32768
    ):
        raise ValueError("Invalid short path")
    return record


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: fs4_windows_reference_validate.py reference.json", file=sys.stderr)
        return 2
    try:
        result = check(sys.argv[1])
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"REFUSED: {error}", file=sys.stderr)
        return 3
    print("PASS Windows reference schema validated; physical provenance and "
          "NT144 parity still require operator review")
    return 0


if __name__ == "__main__":
    sys.exit(main())
