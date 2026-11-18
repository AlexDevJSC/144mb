# SPDX-License-Identifier: MIT
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "fs4_windows_reference_validate.py"
SPEC = importlib.util.spec_from_file_location("fs4_validate_reference", SCRIPT)
MOD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MOD)


class AcceptanceGatesTests(unittest.TestCase):
    def test_physical_gate_requires_explicit_operator(self):
        script = ROOT / "scripts" / "fedora-filesystem4-acceptance.sh"
        result = subprocess.run(["bash", "-n", str(script)])
        self.assertEqual(result.returncode, 0)
        refusal = subprocess.run(["bash", str(script)], capture_output=True)
        self.assertEqual(refusal.returncode, 2)
        # Force CI: even a Fedora host with SELinux must not turn this
        # synthetic smoke test into an actual physical acceptance run.
        ci = subprocess.run(["bash", str(script), "--physical-confirmed"],
                            env={"PATH": "/usr/bin:/bin", "CI": "true"},
                            capture_output=True)
        self.assertEqual(ci.returncode, 3)

    def test_service_physical_gate_cannot_be_pretended_in_ci(self):
        script = ROOT / "scripts" / "fedora-fs4-service-acceptance.py"
        syntax = subprocess.run(["python3", "-c",
                                 "import ast,sys; ast.parse(open(sys.argv[1], encoding='utf-8').read())",
                                 str(script)])
        self.assertEqual(syntax.returncode, 0)
        missing_confirmation = subprocess.run(["python3", str(script)],
                                               capture_output=True)
        self.assertEqual(missing_confirmation.returncode, 2)
        denied_ci = subprocess.run(
            ["python3", str(script), "--physical-confirmed"],
            env={"PATH": "/usr/bin:/bin", "CI": "true"},
            capture_output=True)
        self.assertEqual(denied_ci.returncode, 3)

    def test_reference_is_validated_not_assumed(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "reference.json"
            content = {
                "schema": "filesystem4-windows-reference-v1",
                "volume_filesystem": "NTFS",
                "case_insensitive_exact_name": True,
                "rename_preserves_content": True,
                "hardlink_created": True,
                "hardlink_readable": True,
                "readonly_attribute_visible": True,
                "short_path_generated": False,
                "short_path": None
            }
            source.write_text(json.dumps(content))
            self.assertEqual(MOD.check(str(source)), content)
            content["volume_filesystem"] = "ReFS"
            source.write_text(json.dumps(content))
            with self.assertRaises(ValueError):
                MOD.check(str(source))


if __name__ == "__main__":
    unittest.main()
