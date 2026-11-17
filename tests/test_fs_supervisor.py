# SPDX-License-Identifier: MIT
"""Synthetic fail-closed supervisor and grant validation, no FUSE mount needed."""
import importlib.util
import os
import pathlib
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "scripts" / "nt-fs-supervisor.py"
SPEC = importlib.util.spec_from_file_location("nt_fs_supervisor", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class FileSystemSupervisorTests(unittest.TestCase):
    def test_disjoint_grants(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            (root / "backing").mkdir()
            (root / "view").mkdir()
            lower, view = MODULE.canonical_grants(str(root / "backing"),
                                                   str(root / "view"))
            self.assertNotEqual(lower, view)

    def test_reject_same_nested_and_symlink_alias(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            (root / "backing").mkdir()
            (root / "backing" / "nested").mkdir()
            (root / "alias").symlink_to(root / "backing", target_is_directory=True)
            cases = [
                (root / "backing", root / "backing"),
                (root / "backing", root / "backing" / "nested"),
                (root / "backing", root / "alias"),
            ]
            for lower, view in cases:
                with self.subTest(case=(lower, view)):
                    with self.assertRaises(MODULE.Refused):
                        MODULE.canonical_grants(str(lower), str(view))

    def test_reject_absent_directories_and_missing_command(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            with self.assertRaises(MODULE.Refused):
                MODULE.canonical_grants(str(root), str(root / "none"))
        with self.assertRaises(SystemExit) as error:
            MODULE.main(["--lower", "/tmp", "--view", "/tmp", "--"])
        self.assertEqual(error.exception.code, 2)

    def test_two_sessions_cannot_own_same_view(self):
        with tempfile.TemporaryDirectory() as temp:
            view = pathlib.Path(temp) / "view"
            view.mkdir()
            with mock.patch.dict(os.environ, {"XDG_RUNTIME_DIR": temp}):
                first = MODULE.lock_session(view)
                try:
                    with self.assertRaises(MODULE.Refused):
                        MODULE.lock_session(view)
                finally:
                    os.close(first)
                second = MODULE.lock_session(view)
                os.close(second)

    def test_cleanup_after_leader_exits_with_background_child(self):
        # A dead, reaped leader must not suppress cleanup of its process
        # group; use a real Linux child without requiring a FUSE mount.
        child_code = "import time; time.sleep(30)"
        launcher = (
            "import subprocess, sys; "
            f"p = subprocess.Popen([{sys.executable!r}, '-c', {child_code!r}], "
            "stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); "
            "print(p.pid, flush=True)"
        )
        leader = subprocess.Popen([sys.executable, "-c", launcher],
                                  stdout=subprocess.PIPE,
                                  start_new_session=True)
        try:
            child_pid = int(leader.stdout.readline().strip())
            self.assertEqual(leader.wait(timeout=3), 0)
            MODULE.stop_group(leader)
            for _ in range(20):
                try:
                    state = pathlib.Path(f"/proc/{child_pid}/stat").read_text().rpartition(") ")[2].split()[0]
                except FileNotFoundError:
                    break
                if state == "Z":
                    break
                time.sleep(0.05)
            else:
                self.fail("background child survived supervisor cleanup")
        finally:
            try:
                os.killpg(leader.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            leader.wait(timeout=3)
            if leader.stdout:
                leader.stdout.close()

    def test_termination_signal_unwinds_and_releases_session_lock(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            (root / "lower").mkdir()
            (root / "view").mkdir()

            def interrupted(*_args):
                handler = signal.getsignal(signal.SIGTERM)
                self.assertIs(handler, MODULE.handle_termination)
                handler(signal.SIGTERM, None)

            with mock.patch.dict(os.environ, {"XDG_RUNTIME_DIR": temp}):
                with mock.patch.object(MODULE, "run_session", side_effect=interrupted):
                    result = MODULE.main([
                        "--lower", str(root / "lower"), "--view",
                        str(root / "view"), "--", "/bin/true"])
                self.assertEqual(result, 128 + signal.SIGTERM)
                reopened = MODULE.lock_session(root / "view")
                os.close(reopened)

    def test_pe_mode_requires_exact_private_profile(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            profile = root / "nt-native-linux" / "C:"
            profile.mkdir(parents=True)
            os.chmod(profile, 0o700)
            other = root / "other"
            other.mkdir()
            with mock.patch.dict(os.environ, {
                    "HOME": temp, "XDG_STATE_HOME": temp}):
                MODULE.require_pe_profile(profile)
                with self.assertRaises(MODULE.Refused):
                    MODULE.require_pe_profile(other)
                os.chmod(profile, 0o755)
                with self.assertRaises(MODULE.Refused):
                    MODULE.require_pe_profile(profile)
                os.chmod(profile, 0o700)
                profile.rmdir()
                profile.symlink_to(other, target_is_directory=True)
                with self.assertRaises(MODULE.Refused):
                    MODULE.require_pe_profile(other)

    def test_pe_mode_parser_forwards_to_session_without_running_guest(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            (root / "lower").mkdir()
            (root / "view").mkdir()
            with mock.patch.dict(os.environ, {"XDG_RUNTIME_DIR": temp}):
                with mock.patch.object(MODULE, "run_session",
                                       return_value=7) as run:
                    status = MODULE.main([
                        "--lower", str(root / "lower"), "--view",
                        str(root / "view"), "--pe", "--",
                        "/tmp/safe-test.exe", "--flag"])
                    self.assertEqual(status, 7)
                    self.assertTrue(run.call_args.args[3])
                    self.assertEqual(run.call_args.args[2],
                                     ["/tmp/safe-test.exe", "--flag"])

    def test_unverified_mount_refusal_without_executing_guest(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            (root / "backing").mkdir()
            (root / "view").mkdir()
            # This test is a no-op on an ordinary CI host. It does not need
            # /dev/fuse and may not start any untrusted guest application.
            self.assertFalse(MODULE.mounted(root / "view", "mountpoint"))


if __name__ == "__main__":
    unittest.main()
