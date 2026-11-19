#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Operator-driven FS4 reboot checkpoint. NEVER powers off or remounts Fedora.

Verifies only a committed FUSE checkpoint across a different boot. The operator
must separately document whether the shutdown really was an abrupt power cut.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import stat
import subprocess
import sys

VIEW = Path("/run/ntnative-storage/view")
BOOT_ID = Path("/proc/sys/kernel/random/boot_id")
RUN_RE = re.compile(r"^[a-f0-9]{24}$")
DATA_SIZE = 65536


class Incomplete(Exception):
    pass


def required(condition, reason):
    if not condition:
        raise Incomplete(reason)


def state_root():
    home = Path(os.environ.get("XDG_STATE_HOME") or
                str(Path.home() / ".local" / "state"))
    root = home / "nt-native-linux" / "fs4-reboot-checkpoints"
    required(not root.is_symlink(), "state directory may not be symlink")
    return root


def commit_sha():
    repo = Path(__file__).resolve().parents[1]
    result = subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"],
                            capture_output=True, text=True, timeout=5,
                            check=False)
    required(result.returncode == 0 and
             re.fullmatch(r"[0-9a-f]{40}", result.stdout.strip()) is not None,
             "cannot identify tested Git commit")
    return result.stdout.strip()


def valid_mount(view):
    required(view.is_dir() and not view.is_symlink() and
             view.resolve(strict=True) == view,
             "expected a real, non-symlink service mountpoint")
    for record in Path("/proc/self/mountinfo").read_text().splitlines():
        left, sep, right = record.partition(" - ")
        if not sep:
            continue
        fields = left.split()
        right_fields = right.split()
        if (len(fields) >= 5 and fields[4] == str(view) and
                right_fields and right_fields[0] == "fuse.nt-dos83"):
            return
    raise Incomplete("no mounted fuse.nt-dos83 service view; never use raw backing")


def verify_host():
    required(os.geteuid() != 0, "run as a normal service client, never root")
    required(os.environ.get("CI", "").lower() not in ("1", "true", "yes") and
             os.environ.get("GITHUB_ACTIONS", "").lower() != "true",
             "CI cannot attest to physical Fedora")
    system = Path("/etc/os-release").read_text().splitlines()
    required(any(s.strip() in ('ID=fedora', 'ID="fedora"') for s in system),
             "Fedora host required")
    required(os.uname().machine == "x86_64", "x86_64 required")
    enforce = subprocess.run(["getenforce"], capture_output=True, text=True,
                             timeout=5, check=False)
    required(enforce.returncode == 0 and
             enforce.stdout.strip() == "Enforcing",
             "SELinux Enforcing required; do not change policy")
    required(stat.S_ISCHR(Path("/dev/fuse").stat().st_mode),
             "physical FUSE device required")
    valid_mount(VIEW)
    boot = BOOT_ID.read_text().strip()
    required(re.fullmatch(r"[0-9a-f-]{36}", boot) is not None,
             "invalid kernel boot_id")
    return boot


def prepare(boot):
    base = state_root()
    base.mkdir(parents=True, exist_ok=True, mode=0o700)
    required(not base.is_symlink(), "refusing symlink state root")
    token = secrets.token_hex(12)
    folder = VIEW / (".nt144-fs4-reboot-" + token)
    folder.mkdir(mode=0o700)
    payload = secrets.token_bytes(DATA_SIZE)
    target = folder / "payload.bin"
    flags = os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_CLOEXEC | os.O_NOFOLLOW
    fd = os.open(target, flags, 0o600)
    try:
        with os.fdopen(fd, "wb", closefd=True) as out:
            out.write(payload)
            out.flush()
            os.fsync(out.fileno())
    except BaseException:
        # Leave the disposable test artifact intact for forensic inspection.
        raise
    info = target.stat(follow_symlinks=False)
    required(stat.S_ISREG(info.st_mode) and info.st_nlink == 1,
             "unexpected checkpoint inode type or hardlink")
    manifest = {
        "schema": "fs4-postreboot-checkpoint-v1",
        "token": token,
        "commit": commit_sha(),
        "uid": os.geteuid(),
        "boot_id": boot,
        "kernel": os.uname().release,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "bytes": DATA_SIZE,
        "sha256": hashlib.sha256(payload).hexdigest(),
    }
    path = base / ("fs4-" + token + ".json")
    with path.open("x", encoding="utf-8") as out:
        json.dump(manifest, out, indent=2, sort_keys=True)
        out.write("\n")
        out.flush()
        os.fsync(out.fileno())
    # The evidence manifest is on the Linux client side, not in the FUSE
    # backing; fail closed if its directory cannot be durably synchronized.
    parent = os.open(base, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(parent)
    finally:
        os.close(parent)
    print("PREPARED: committed disposable FUSE file, manifest: " + str(path))
    print("No shutdown was performed. Operator must arrange a safe, supervised")
    print("power-loss/reboot on disposable storage before running verify.")


def verify(boot, manifest_path):
    base = state_root().resolve(strict=True)
    path = Path(manifest_path)
    required(path.is_file() and not path.is_symlink() and
             path.parent.resolve(strict=True) == base,
             "manifest must be a regular file inside local checkpoint state")
    required(path.stat().st_size <= 4096, "oversized manifest")
    data = json.loads(path.read_text(encoding="utf-8"))
    required(isinstance(data, dict) and
             data.get("schema") == "fs4-postreboot-checkpoint-v1" and
             isinstance(data.get("token"), str) and
             RUN_RE.fullmatch(data["token"]) is not None,
             "invalid checkpoint schema or token")
    required(path.name == "fs4-" + data["token"] + ".json",
             "manifest name and token mismatch")
    required(data.get("uid") == os.geteuid() and
             data.get("commit") == commit_sha(),
             "different UID or source revision; comparison refused")
    required(data.get("boot_id") != boot and
             isinstance(data.get("boot_id"), str),
             "no different kernel boot detected")
    required(data.get("bytes") == DATA_SIZE and
             isinstance(data.get("sha256"), str) and
             re.fullmatch(r"[a-f0-9]{64}", data["sha256"]) is not None,
             "invalid checkpoint data contract")
    folder = VIEW / (".nt144-fs4-reboot-" + data["token"])
    required(folder.is_dir() and not folder.is_symlink(),
             "checkpoint folder is missing or not a directory")
    target = folder / "payload.bin"
    fd = os.open(target, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        info = os.fstat(fd)
        required(stat.S_ISREG(info.st_mode) and
                 info.st_size == DATA_SIZE and info.st_nlink == 1,
                 "checkpoint inode invalid or size changed")
        digest = hashlib.sha256()
        with os.fdopen(fd, "rb", closefd=False) as stream:
            while True:
                chunk = stream.read(8192)
                if not chunk:
                    break
                digest.update(chunk)
        required(digest.hexdigest() == data["sha256"],
                 "committed FUSE file bytes differ after reboot")
    finally:
        os.close(fd)
    print("PASS: committed FileSystem4 FUSE checkpoint survived a different boot")
    print("INCOMPLETE: actual abrupt power-loss provenance and in-flight")
    print("WAL/delete-pending/rename recovery still require operator evidence.")
    print("Test artifacts retained for manual forensic review; not auto-deleted.")


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("prepare", "verify"))
    parser.add_argument("--physical-confirmed", action="store_true")
    parser.add_argument("--disposable-confirmed", action="store_true")
    parser.add_argument("--manifest")
    args = parser.parse_args(argv)
    if not args.physical_confirmed:
        parser.error("--physical-confirmed is required")
    if args.phase == "prepare" and not args.disposable_confirmed:
        parser.error("prepare requires --disposable-confirmed")
    if args.phase == "verify" and not args.manifest:
        parser.error("verify requires --manifest")
    try:
        boot = verify_host()
        if args.phase == "prepare":
            prepare(boot)
        else:
            verify(boot, args.manifest)
    except (Incomplete, OSError, ValueError, subprocess.TimeoutExpired) as err:
        print("INCOMPLETE: " + str(err), file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
