# SPDX-License-Identifier: MIT
import importlib.util
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1] / "scripts" / "analyze-fedora-qa.py"
SPEC = importlib.util.spec_from_file_location("fedora_qa_analyzer", SOURCE)
ANALYZER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ANALYZER)


class FedoraQAReportTests(unittest.TestCase):
    def test_29_block_classification(self):
        completed = "".join(
            f"[{i}/29] [SINTETICA] Componente 7: Ok {i}\n"
            "PASS test\nRESULTADO: PASS (salida 0)\n"
            for i in range(1, 28)
        )
        log = (
            completed +
            "[28/29] [REAL_RUNTIME] Componente 1: PE isolated\n"
            "Unsupported guest import KERNEL32.dll!lstrcmpW\n"
            "RESULTADO: FALLO (salida 2)\n"
            "[29/29] [REAL_RUNTIME] Componente 4: Core test\n"
            "SKIP native FluidSynth ABI: libfluidsynth.so.3 unavailable\n"
            "SKIP positive multithread priority transition: host cannot safely restore nice\n"
            "RESULTADO: INCOMPLETO (salida 0)\n"
        )
        result = ANALYZER.parse(log)
        self.assertEqual(result["counts"]["PASS"], 27)
        self.assertEqual(result["counts"]["FALLO"], 1)
        self.assertEqual(result["counts"]["INCOMPLETO"], 1)
        problems = result["blocks"][27]["findings"]
        self.assertEqual(problems[0]["type"], "MISSING_API")
        self.assertEqual(problems[0]["detail"], "KERNEL32.dll!lstrcmpW")
        types = {x["type"] for x in result["blocks"][28]["findings"]}
        self.assertIn("MISSING_DEPENDENCY", types)
        self.assertIn("HOST_PRIORITY", types)

    def test_skips_are_not_hidden_by_pass(self):
        result = ANALYZER.parse(
            "[1/2] [REAL_RUNTIME] Componente 4: CPU\n"
            "PASS single group, cross-group=SKIP (one group permitted)\n"
            "RESULTADO: PASS\n"
            "[2/2] [SINTETICA] Componente 7: Missing result\n"
        )
        self.assertEqual(result["counts"]["INCOMPLETO"], 1)
        self.assertIn("HOST_TOPOLOGY",
                      {f["type"] for f in result["blocks"][0]["findings"]})
        self.assertEqual(result["blocks"][1]["status"], "INCOMPLETO")

    def test_unexpected_skip_stays_visible(self):
        issues = ANALYZER.classify("SKIP unrecognized backend check\n")
        self.assertEqual(issues[0]["type"], "SKIP_UNCLASSIFIED")


if __name__ == "__main__":
    unittest.main()
