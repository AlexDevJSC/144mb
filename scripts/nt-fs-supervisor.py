#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Supervise one explicitly launched NT-compatible FUSE session.

This is a per-session security boundary, not a kernel-wide interposition.
Never run clients if the controlled FUSE mount and Landlock confinement
cannot both be demonstrated. Do not attach to or suspend foreign threads.
"""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import os
from pathlib import Path
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time


class Refused(RuntimeError):
    pass


class SessionTerminated(Exception):
    def __init__(self, signum: int):
        super().__init__(f"Session termination signal {signum}")
        self.signum = signum


def handle_termination(signum: int, _frame: object) -> None:
    # A second termination request must not interrupt the finally block
    # responsible for stopping guests and unmounting FUSE.
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    signal.signal(signal.SIGHUP, signal.SIG_IGN)
    raise SessionTerminated(signum)


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


def lock_session(view: Path) -> int:
    """No two session supervisors may mount the same FUSE view concurrently."""
    runtime = Path(os.environ.get("XDG_RUNTIME_DIR") or
                   str(Path.home() / ".cache"))
    private = runtime / "nt-fs-supervisor"
    private.mkdir(mode=0o700, parents=True, exist_ok=True)
    info = private.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or (
        info.st_mode & 0o077
    ):
        raise Refused("Unsafe supervisor lock directory ownership/permissions")
    token = hashlib.sha256(os.fsencode(str(view))).hexdigest()[:32]
    lock_path = private / (token + ".lock")
    fd = os.open(lock_path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    try:
        data = os.fstat(fd)
        if not stat.S_ISREG(data.st_mode) or data.st_uid != os.geteuid() or (
            data.st_mode & 0o077
        ):
            raise Refused("Untrusted supervisor lock file")
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise Refused("Another supervisor owns this NT filesystem view") from None
        return fd
    except BaseException:
        os.close(fd)
        raise


def require_program(name: str) -> str:
    path = shutil.which(name)
    if not path:
        raise Refused(f"Missing required program: {name}")
    return path


def mounted(view: Path, mountpoint: str) -> bool:
    return subprocess.run([mountpoint, "-q", "--", str(view)],
                          check=False, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


def group_exists(pgid: int) -> bool:
    try:
        os.killpg(pgid, 0)
    except ProcessLookupError:
        return False
    return True


def stop_group(proc: subprocess.Popen[bytes] | None) -> None:
    if proc is None:
        return
    # The launcher is a session leader, but its background children can
    # remain in that group after the leader has exited and been reaped.
    # Waiting for the leader alone therefore does not clean up the session.
    pgid = proc.pid
    try:
        os.killpg(pgid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    else:
        deadline = time.monotonic() + 1.0
        while group_exists(pgid) and time.monotonic() < deadline:
            time.sleep(0.05)
        # Always escalate if the group remains, even when its leader exited
        # earlier or a descendant ignored SIGTERM.
        if group_exists(pgid):
            try:
                os.killpg(pgid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    if proc.poll() is None:
        proc.wait(timeout=2.0)


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
                raise Refused("FUSE mount remains active after cleanup; refusing success")


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
    old_handlers = {
        signum: signal.getsignal(signum)
        for signum in (signal.SIGTERM, signal.SIGHUP)
    }
    try:
        for signum in old_handlers:
            signal.signal(signum, handle_termination)
        lower, view = canonical_grants(args.lower, args.view)
        lock_fd = lock_session(view)
        try:
            return run_session(lower, view, program)
        finally:
            os.close(lock_fd)
    except SessionTerminated as interrupted:
        return 128 + interrupted.signum
    except (Refused, OSError) as error:
        print(f"REFUSED: {error}", file=sys.stderr)
        return 3
    finally:
        for signum, handler in old_handlers.items():
            signal.signal(signum, handler)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
