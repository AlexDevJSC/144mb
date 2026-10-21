#!/usr/bin/env python3
"""Synchronize Mouth sources into Bones ModKit 0.4 static source provider.

Development-time only: no runtime Python, DLL discovery or dynamic install.
Never touches Bones core, public ABI, or other specialist directories.
Default --check compares files; --apply is an explicit operation.
--pack writes a drop-in ZIP with modules/mouth/ paths (after passing check).
"""
import argparse
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED

REPO = Path(__file__).resolve().parents[2]
MOUTH = REPO / "Mouth"
DEST = REPO / "BONES" / "modules" / "mouth"

def source_files():
    head = (MOUTH / "include" / "mouth_lan.h").read_text(encoding="utf-8")
    code = (MOUTH / "src" / "mouth_lan.c").read_text(encoding="utf-8")
    entry = (MOUTH / "adapter" / "modkit.c").read_text(encoding="utf-8")
    assert '#include "bones/modkit.h"' in head, "Unexpected upstream header"
    assert '#include "../include/mouth_lan.h"' in code, "Unexpected upstream source"
    head = head.replace('#include "bones/modkit.h"',
                        '#include "../../include/bones/modkit.h"')
    code = code.replace('#include "../include/mouth_lan.h"',
                        '#include "mouth_lan.h"')
    return {"modkit.c": entry, "mouth_lan.h": head, "mouth_lan.c": code}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--check", action="store_true",
                       help="Check that BONES/modules/mouth matches Mouth sources")
    group.add_argument("--apply", action="store_true",
                       help="Install/replace ONLY 3 Mouth files under Bones")
    group.add_argument("--pack", action="store_true",
                       help="Create source ZIP after checking synchronization")
    args = parser.parse_args()
    expected = source_files()
    if args.apply:
        DEST.mkdir(parents=True, exist_ok=True)
        for filename, content in expected.items():
            p = DEST / filename
            if not p.is_file() or p.read_text(encoding="utf-8") != content:
                p.write_text(content, encoding="utf-8", newline="")
                print("Updated:", p.relative_to(REPO))
        print("Source files installed; recompile Bones and run tests.")
        return
    outdated = [filename for filename, content in expected.items()
                if not (DEST / filename).is_file() or
                (DEST / filename).read_text(encoding="utf-8") != content]
    if outdated:
        parser.exit(1, "Out-of-sync Mouth: " + ", ".join(outdated)
                    + "\nRun with --apply, review diff, and commit both copies.\n")
    print("OK: Bones Mouth source bundle matches canonical sources.")
    if args.pack:
        build = MOUTH / "build"
        build.mkdir(exist_ok=True)
        target = build / "MOUTH_Bones_ModKit_0_4.zip"
        with ZipFile(target, "w", ZIP_DEFLATED) as archive:
            for filename in expected:
                archive.write(DEST / filename, arcname="modules/mouth/" + filename)
            readme = DEST / "README.txt"
            if readme.is_file():
                archive.write(readme, arcname="modules/mouth/README.txt")
        print("Drop-in module archive:", target.relative_to(REPO))

if __name__ == "__main__":
    main()
