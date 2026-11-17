#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Supervise one explicitly launched NT-compatible FUSE session.

This is a per-session security boundary, not a kernel-wide interposition.
Never run clients if the controlled FUSE mount and Landlock confinement
cannot both be demonstrated. Do not attach to or suspend foreign threads.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time


class Refused(RuntimeError):
    pass


def canonical_grants(lower: str, view: str) -> tuple[Path, Path]:
    for raw in (lower, view):
        if not Path(raw).is_dir():
            raise Refused("Both lower and view must be existing directories")
    a, b = Path(lower).resolve(strict=True), Path(view).resolve(strict=True)
    if a == b or a in b.parents or b in a.parents:
        raise Refused("Lower and FUSE view must be disjoint directories")
    if a.stat().st_dev == 0 or b.stat().st_dev == 0:
        raise Refused("Invalid root identity")
    return a, b


def require_program(name: str) -> str:
    path = shutil.which(name)
    if not path:
        raise Refused(f"Missing required program: {name}")
    return path


def mounted(view: Path, mountpoint: str) -> bool:
    return subprocess.run([mountpoint, "-q", "--", str(view)],
                          check=False, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


def stop_group(proc: subprocess.Popen[bytes] | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    # start_new_session=True makes this process the leader of its own group.
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        proc.wait(timeout=2.0)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait()


def run_session(lower: Path, view: Path, program: list[str]) -> int:
    if os.geteuid() == 0:
        raise Refused("Do not run the FileSystem session supervisor as root")
    root = Path(__file__).resolve().parent.parent
    fuse = root / "build" / "nt-dos83-fuse"
    landlock = root / "build" / "nt-dos83-landlock"
    # Accept both an in-tree build and an installed RPM/user-local setup.
    sibling = Path(__file__).resolve().parent
    if not fuse.is_file():
        fuse = sibling / "nt-dos83-fuse"
    if not landlock.is_file():
        landlock = sibling / "nt-dos83-landlock"
    mediator = root / "build" / "nt-dos83-mediator"
    if not mediator.is_file():
        mediator = sibling / "nt-dos83-mediator"
    if not mediator.is_file():
        mediator = Path(shutil.which("nt-dos83-mediator") or str(mediator))
    if not fuse.is_file():
        fuse = Path(shutil.which("nt-dos83-fuse") or str(fuse))
    if not landlock.is_file():
        landlock = Path(shutil.which("nt-dos83-landlock") or str(landlock))
    if not fuse.is_file() or not os.access(fuse, os.X_OK):
        raise Refused(f"Missing built FUSE backend: {fuse}")
    if not landlock.is_file() or not os.access(landlock, os.X_OK):
        raise Refused(f"Missing built Landlock runner: {landlock}")
    if not mediator.is_file() or not os.access(mediator, os.X_OK):
        raise Refused(f"Missing trusted DOS 8.3 mediator: {mediator}")
    mountpoint = require_program("mountpoint")
    fusermount = require_program("fusermount3")
    if not Path("/dev/fuse").exists():
        raise Refused("/dev/fuse missing")
    if mounted(view, mountpoint):
        raise Refused("Refusing a view that is already mounted")
    if subprocess.run([str(landlock), "--probe"], check=False,
                      stdout=subprocess.DEVNULL,
                      stderr=subprocess.DEVNULL).returncode != 0:
        raise Refused("Landlock ABI >=3 is required, not optional")
    # Recover any missing DOS aliases after a previous abrupt stop before
    # launching an untrusted command. This scan is not a durable rename WAL.
    scan = subprocess.run([str(mediator), "--root", str(lower), "--scan"],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          check=False)
    if scan.returncode != 0:
        raise Refused("Preflight namespace reconciliation failed: " +
                      scan.stderr.decode("utf-8", "replace")[-1000:])
    fuse_proc: subprocess.Popen[bytes] | None = None
    guest: subprocess.Popen[bytes] | None = None
    with tempfile.TemporaryDirectory(prefix="nt-fs-supervisor-") as logs:
        log_path = Path(logs) / "fuse.log"
        try:
            with log_path.open("wb") as output:
                fuse_proc = subprocess.Popen(
                    [str(fuse), str(lower), str(view), "-f"],
                    stdout=output, stderr=subprocess.STDOUT,
                    start_new_session=True)
            for _ in range(200):
                if fuse_proc.poll() is not None:
                    break
                if mounted(view, mountpoint):
                    break
                time.sleep(0.05)
            if fuse_proc.poll() is not None or not mounted(view, mountpoint):
                detail = log_path.read_text(encoding="utf-8", errors="replace")[-2000:]
                raise Refused(f"FUSE mount did not become ready: {detail}")
            # Exactly the same verified mount will be checked again before
            # the child executes by nt-dos83-landlock itself.
            check = subprocess.run([str(landlock), "--view", str(view), "--",
                                    "/bin/true"], check=False)
            if check.returncode != 0:
                raise Refused("Landlock rejected the live mediated mount")
            env = dict(os.environ)
            env["NT_FS_SUPERVISED"] = "1"
            env["NT_FS_VIEW"] = str(view)
            guest = subprocess.Popen([str(landlock), "--view", str(view), "--", *program],
                                     env=env, start_new_session=True)
            while True:
                status = guest.poll()
                if status is not None:
                    return 128 + -status if status < 0 else status
                if fuse_proc.poll() is not None or not mounted(view, mountpoint):
                    raise Refused("Mediation daemon died or its mount disappeared")
                time.sleep(0.05)
        finally:
            stop_group(guest)
            if mounted(view, mountpoint):
                subprocess.run([fusermount, "-u", str(view)], check=False,
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
            stop_group(fuse_proc)
            if mounted(view, mountpoint):
                print("ERROR: mediated mount could not be unmounted safely",
                      file=sys.stderr)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Supervise one FUSE/Landlock-protected NT file session")
    parser.add_argument("--lower", required=True, help="trusted physical backing")
    parser.add_argument("--view", required=True, help="dedicated empty mountpoint")
    parser.add_argument("program", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    program = args.program[1:] if args.program[:1] == ["--"] else args.program
    if not program:
        parser.error("A program after -- is mandatory")
    try:
        lower, view = canonical_grants(args.lower, args.view)
        return run_session(lower, view, program)
    except (Refused, OSError) as error:
        print(f"REFUSED: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
