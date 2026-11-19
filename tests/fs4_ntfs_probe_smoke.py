#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise NT144 W-API differential probe on a disposable C: and parse JSON."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Usage: fs4_ntfs_probe_smoke.py BIN")
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="fs4-ntfs-differential-") as root:
        result = subprocess.run([binary, "--root", root], capture_output=True,
                                text=True, timeout=20)
        assert result.returncode == 0, (
            "NT144 probe exit", result.returncode, result.stderr, result.stdout)
        rows = json.loads(result.stdout)
        assert rows["schema"] == "filesystem4-nt144-reference-v1"
        assert rows["probe_cleaned_up"] is True
        assert rows["case_insensitive_exact_name"] is True
        assert rows["rename_preserves_content"] is True
        assert rows["readonly_attribute_visible"] is True
        assert type(rows["short_path_generated"]) is bool
        assert rows["hardlink_created"] is True, rows
        assert rows["hardlink_readable"] is True, rows
        print("PASS NT144 live Win32 W-API differential probe (hardlink shared inode)")


if __name__ == "__main__":
    main()
