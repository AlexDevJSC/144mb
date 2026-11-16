#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Classify actual Fedora QA block results without turning SKIPs into PASS.

Usage: python3 scripts/analyze-fedora-qa.py run.log [--json] [--strict]
--strict: exit 1 for failures, 3 for incomplete-only, 0 for full PASS.
"""
import argparse
import json
import re
from pathlib import Path

START = re.compile(r"^\[(\d+)/(\d+)\]\s+\[([^\]]+)\]\s+(.*)$", re.M)
RESULT = re.compile(r"^RESULTADO:\s*(PASS|FALLO|INCOMPLETO|TIMEOUT|CANCELADO)\b", re.M)
IMPORT = re.compile(r"Unsupported guest import\s+([^\s!]+)!([^\s]+)")

def classify(body: str) -> list[dict[str, str]]:
    findings = []
    seen = set()
    for dll, symbol in IMPORT.findall(body):
        key = ("MISSING_API", dll + "!" + symbol)
        if key not in seen:
            findings.append({"type": "MISSING_API", "detail": dll + "!" + symbol,
                             "next": "Implement and register this exported API; rerun isolated PE"})
            seen.add(key)
    entries = [
        ("FluidSynth", "MISSING_DEPENDENCY",
         "Install Fedora fluidsynth-libs; run make check-audio-required"),
        ("SKIP positive boost", "HOST_PRIORITY",
         "Run opt-in reversible priority tests with delegated RLIMIT_NICE"),
        ("SKIP positive multithread", "HOST_PRIORITY",
         "Run opt-in reversible priority tests with delegated RLIMIT_NICE"),
        ("cross-group=SKIP", "HOST_TOPOLOGY",
         "Requires physical allowed CPUs spanning two 64-CPU groups"),
        ("SKIP process group inheritance", "HOST_TOPOLOGY",
         "Requires physical allowed CPUs spanning two 64-CPU groups"),
        ("SKIP external-PE sandbox", "HOST_SANDBOX",
         "Enable Bubblewrap user namespaces; do not bypass isolation"),
    ]
    for needle, kind, next_step in entries:
        if needle.lower() in body.lower():
            key = (kind, next_step)
            if key not in seen:
                findings.append({"type": kind, "detail": needle, "next": next_step})
                seen.add(key)
    for line in body.splitlines():
        if line.startswith("SKIP") and not any(
            x["detail"].lower() in line.lower() for x in findings
        ):
            # Even unknown SKIPs remain visible to the operator.
            findings.append({"type": "SKIP_UNCLASSIFIED", "detail": line[:220],
                             "next": "Review this skipped check; not a PASS"})
    return findings

def parse(text: str) -> dict:
    text = re.sub(r"\x1b\[[0-9;]*m", "", text)
    starts = list(START.finditer(text))
    blocks = []
    for index, match in enumerate(starts):
        body = text[match.end(): starts[index + 1].start() if index + 1 < len(starts) else len(text)]
        results = RESULT.findall(body)
        blocks.append({
            "index": int(match.group(1)),
            "total": int(match.group(2)),
            "kind": match.group(3),
            "title": match.group(4).strip(),
            "status": results[0] if results else "INCOMPLETO",
            "findings": classify(body)
        })
    counts = {key: sum(item["status"] == key for item in blocks)
              for key in ("PASS", "FALLO", "INCOMPLETO", "TIMEOUT", "CANCELADO")}
    if starts:
        expected = int(starts[0].group(2))
        if len(blocks) != expected:
            counts["INCOMPLETO"] += expected - len(blocks)
    else:
        counts["INCOMPLETO"] = 1
    return {"expected_blocks": int(starts[0].group(2)) if starts else 0,
            "observed_blocks": len(blocks), "counts": counts, "blocks": blocks}

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="Plain-text output from nt144-qa")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--strict", action="store_true")
    args = parser.parse_args()
    try:
        report = parse(args.log.read_text(encoding="utf-8", errors="replace"))
    except OSError as error:
        parser.error(str(error))
    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        print(f"Fedora QA: {report['observed_blocks']}/{report['expected_blocks']} blocks")
        print(" ".join(f"{key}={value}" for key, value in report["counts"].items()))
        for block in report["blocks"]:
            if block["status"] == "PASS" and not block["findings"]:
                continue
            print(f"[{block['index']}] {block['status']}: {block['title']}")
            for finding in block["findings"]:
                print(f"  {finding['type']}: {finding['detail']}")
                print(f"    Next: {finding['next']}")
    if not args.strict:
        return 0
    if report["counts"]["FALLO"] or report["counts"]["TIMEOUT"] or report["counts"]["CANCELADO"]:
        return 1
    return 3 if report["counts"]["INCOMPLETO"] else 0

if __name__ == "__main__":
    raise SystemExit(main())
