#!/usr/bin/env python3
"""Sync the exact Bones v0.4.0 ZIP tree into BONES/ of a local git checkout.
Usage:
 python scripts/sync_from_zip.py /path/PROYECTO_144_Bones_ModKit_v0_4_0.zip
Run from the repository root. Review changes before git commit/push.
"""
import argparse
import hashlib
from pathlib import Path
import shutil
import zipfile

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "BONES"
PREFIX = "PROYECTO_144_Bones_ModKit_v0_4_0/"
PARSER = argparse.ArgumentParser()
PARSER.add_argument("zip", type=Path)
PARSER.add_argument("--apply", action="store_true", help="Copy files into BONES (default: dry run)")
args = PARSER.parse_args()
if not (ROOT / ".git").exists():
    raise SystemExit("ERROR: expected to run within a git checkout")
if not args.zip.is_file():
    raise SystemExit(f"ERROR: ZIP not found: {args.zip}")
sha = hashlib.sha256(args.zip.read_bytes()).hexdigest()
with zipfile.ZipFile(args.zip) as z:
    files = [(m, m.filename[len(PREFIX):]) for m in z.infolist()
             if m.filename.startswith(PREFIX) and not m.is_dir()]
    if not files:
        raise SystemExit("ERROR: no v0.4.0 project files found in archive")
    for m, rel in files:
        target = (OUT / rel).resolve()
        if not target.is_relative_to(OUT.resolve()):
            raise SystemExit(f"ERROR: path traversal: {rel}")
        if m.file_size > 5_000_000:
            raise SystemExit(f"ERROR: unexpected large file: {rel}")
    print(f"ZIP SHA256: {sha}; files: {len(files)}")
    print("FILES:", *[rel for _, rel in files], sep="\n  ")
    if not args.apply:
        raise SystemExit("DRY RUN. Add --apply to write. Review git diff afterwards.")
    for m, rel in files:
        target = OUT / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        with z.open(m) as source, target.open("wb") as destination:
            shutil.copyfileobj(source, destination)
    print(f"Copied {len(files)} files. Review git status and git diff; commit and push deliberately.")
