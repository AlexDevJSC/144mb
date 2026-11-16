#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""NT144 QA status accounting. An exit-zero make with SKIP is INCOMPLETE.

This script is intentionally a log classifier, NOT a Windows compatibility
certificate. The native Fedora stage4 gate supplies its required PASS
sentinels and never converts missing hardware into a simulated PASS.
"""
import argparse
import json
import re
from pathlib import Path

SKIP = re.compile(r"(?<![A-Za-z])SKIP\b", re.ASCII)
FAIL = re.compile(r"^\s*(?:FAIL(?:ED)?\b|make:\s*\*\*|runtime error:)", re.I)


def classify(logs: list[str], required: list[str], allowed: list[str],
             resolved: dict[str, str] | None = None) -> dict:
    resolved = resolved or {}
    skips = []
    resolved_skips = []
    failures = []
    joined = "\n".join(logs)
    for line in logs:
        text = line.strip()
        if SKIP.search(text):
            if not any(fragment in text for fragment in allowed):
                proof = next((positive for negative, positive in resolved.items()
                              if negative in text and positive in joined), None)
                if proof is None:
                    skips.append(text)
                else:
                    resolved_skips.append({"skip": text, "positive_evidence": proof})
        if FAIL.search(text):
            failures.append(text)
    missing = [text for text in required if text not in joined]
    if failures:
        status = "FAIL"
    elif missing or skips:
        status = "INCOMPLETE"
    else:
        status = "PASS"
    return {
        "status": status,
        "failures": failures,
        "skipped_required": skips,
        "resolved_skips": resolved_skips,
        "missing_evidence": missing,
        "required_evidence": required,
        "allowed_skips": allowed,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path, help="captured suite stdout/stderr")
    parser.add_argument("--require", action="append", default=[], help="literal required PASS sentinel")
    parser.add_argument("--allow-skip", action="append", default=[],
                        help="explicitly optional SKIP substring; never implied")
    parser.add_argument("--resolved-by", action="append", default=[], metavar="SKIP::PASS",
                        help="a named omitted run is superseded only by exact observed positive evidence")
    parser.add_argument("--json-out", type=Path, help="persist machine-readable tri-state result")
    args = parser.parse_args()
    lines = []
    for filename in args.logs:
        try:
            lines.extend(filename.read_text(encoding="utf-8", errors="replace").splitlines())
        except OSError as exc:
            parser.error(f"cannot inspect {filename}: {exc}")
    resolved = {}
    for pair in args.resolved_by:
        if "::" not in pair:
            parser.error("--resolved-by requires literal SKIP::PASS")
        negative, positive = pair.split("::", 1)
        if not negative or not positive:
            parser.error("--resolved-by needs nonempty evidence strings")
        resolved[negative] = positive
    result = classify(lines, args.require, args.allow_skip, resolved)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n",
                                 encoding="utf-8")
    return {"PASS": 0, "FAIL": 1, "INCOMPLETE": 3}[result["status"]]


if __name__ == "__main__":
    raise SystemExit(main())
