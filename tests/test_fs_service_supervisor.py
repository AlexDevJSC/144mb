#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reject ambiguous/legacy PE fallback in dedicated-UID session mode."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/nt-fs-supervisor.py"
spec = importlib.util.spec_from_file_location("nt_fs_supervisor_svc", SCRIPT)
supervisor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(supervisor)


class ServiceSupervisor(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), *args],
                              text=True, capture_output=True, timeout=8)

    def test_direct_backing_cannot_be_supplied_to_service_mode(self):
        with tempfile.TemporaryDirectory() as path:
            p = self.run_cli("--lower", path, "--view", path,
                             "--service-socket", "/run/not-existent.sock",
                             "--", "/bin/true")
            self.assertEqual(p.returncode, 3)
            self.assertIn("must not receive a backing path", p.stderr)

    def test_pe_refused_even_before_launch(self):
        with tempfile.TemporaryDirectory() as path:
            p = self.run_cli("--view", path,
                             "--service-socket", "/run/not-existent.sock",
                             "--pe", "--", "game.exe")
            self.assertEqual(p.returncode, 3)
            self.assertIn("PE storage IPC backend not implemented", p.stderr)

    def test_legacy_without_lower_refuses(self):
        with tempfile.TemporaryDirectory() as path:
            p = self.run_cli("--view", path, "--", "/bin/true")
            self.assertEqual(p.returncode, 3)
            self.assertIn("Legacy mode requires --lower", p.stderr)

    def test_missing_service_fail_closed(self):
        with tempfile.TemporaryDirectory() as path:
            with mock.patch.object(supervisor.os, "geteuid", return_value=1000):
                with self.assertRaises(supervisor.Refused):
                    supervisor.service_attestation(path + "/absent.sock",
                                                    "ntnative-storage")

    def test_no_client_is_started_before_service_identity(self):
        with tempfile.TemporaryDirectory() as view:
            with mock.patch.object(supervisor.os, "geteuid", return_value=1000), \
                 mock.patch.object(supervisor, "service_attestation",
                                   side_effect=supervisor.Refused("no daemon")), \
                 mock.patch.object(supervisor.subprocess, "Popen") as start:
                # Existing Landlock program is not required for this test;
                # rejection is already proved by the service identity check.
                with self.assertRaises(supervisor.Refused):
                    supervisor.run_service_view(Path(view), ["/bin/true"],
                                                "/run/unavailable.sock",
                                                "ntnative-storage")
                start.assert_not_called()


if __name__ == "__main__":
    unittest.main()
