#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read-only FileSystem4 diagnostic for the proposed separate-service-UID gate.

Never claims that a service exists or that SELinux/DAC enforcement is proved.
No chmod, chown, mount, setenforce or filesystem write is performed here.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import stat
import sys


class AuditRefused(Exception):
    pass


def inspected_dir(raw: str) -> tuple[Path, os.stat_result]:
    path = Path(raw)
    if not path.is_absolute() or path.is_symlink() or not path.is_dir():
        raise AuditRefused("Expected an existing, absolute, non-symlink directory")
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        info = os.fstat(fd)
        if not stat.S_ISDIR(info.st_mode):
            raise AuditRefused("Backing is not a physical directory")
        canonical = path.resolve(strict=True)
        current = canonical.stat()
        if info.st_dev != current.st_dev or info.st_ino != current.st_ino:
            raise AuditRefused("Backing directory changed during inspection")
        return canonical, info
    finally:
        os.close(fd)


def mount_type(view: Path) -> str | None:
    """Inspect this process's mount namespace only (not the host globally)."""
    try:
        lines = Path("/proc/self/mountinfo").read_text().splitlines()
    except OSError:
        return None
    # Mountinfo path components may escape spaces as \040.
    target = str(view)
    for line in reversed(lines):
        halves = line.split(" - ", 1)
        fields = halves[0].split()
        if len(halves) != 2 or len(fields) < 5:
            continue
        name = fields[4].replace("\\040", " ").replace("\\011", "\t")
        if name == target:
            return halves[1].split(" ", 1)[0]
    return None


def selinux_mode() -> str:
    try:
        result = Path("/sys/fs/selinux/enforce").read_text().strip()
    except OSError:
        return "unverified"
    return "Enforcing" if result == "1" else "not-Enforcing"


def inspect(backing: str, view: str | None = None) -> dict:
    if os.geteuid() == 0:
        raise AuditRefused("Do not audit user isolation as root")
    lower, info = inspected_dir(backing)
    report = {
        "scope": "diagnostic_only_not_accepted",
        "backing": str(lower),
        "operator_uid": os.geteuid(),
        "backing_uid": info.st_uid,
        "backing_gid": info.st_gid,
        "backing_mode": format(stat.S_IMODE(info.st_mode), "04o"),
        "same_uid_controls_backing": info.st_uid == os.geteuid(),
        "group_or_other_dac_write": bool(info.st_mode & (stat.S_IWGRP | stat.S_IWOTH)),
        "selinux": selinux_mode(),
        "view_mount_type": None,
        "protected": False,
        "requires": [
            "separate service UID with exclusive backing ownership",
            "DAC/SELinux policy denying direct client access",
            "authenticated IPC with grants and per-handle capabilities",
            "same namespace and verified FUSE mount for Linux clients",
            "real Fedora Enforcing plus cross-UID denial evidence",
        ],
    }
    if view is not None:
        upper, _ = inspected_dir(view)
        if lower == upper or lower in upper.parents or upper in lower.parents:
            raise AuditRefused("Backing and view must be disjoint")
        report["view_mount_type"] = mount_type(upper)
    reasons = []
    if report["same_uid_controls_backing"]:
        reasons.append("client UID owns backing and can change DAC permissions")
    if report["group_or_other_dac_write"]:
        reasons.append("backing has group or world write bits")
    if report["selinux"] != "Enforcing":
        reasons.append("SELinux Enforcing was not verified")
    if view is not None and report["view_mount_type"] != "fuse.nt-dos83":
        reasons.append("view is not a verified fuse.nt-dos83 mount")
    if not reasons:
        reasons.append("separate-UID broker/DAC and cross-UID denials not independently verified")
    report["findings"] = reasons
    return report


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lower", required=True)
    parser.add_argument("--view")
    args = parser.parse_args(argv)
    try:
        report = inspect(args.lower, args.view)
    except (AuditRefused, OSError) as exc:
        print(f"REFUSED: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(report, indent=2, sort_keys=True))
    # This code never returns PASS: it can identify hazards, but cannot
    # prove broker IPC / SELinux rules / cross-user isolation from one UID.
    return 3


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
