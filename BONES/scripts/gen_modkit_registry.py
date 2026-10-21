#!/usr/bin/env python3
"""Generate deterministic, opt-in, source-linked ModKit registration.
Only modules/*/modkit.c are linked. No runtime Python, filesystem, or DLLs.
Do NOT confuse with the legacy module.c registry.
"""
from pathlib import Path
import re
root=Path(__file__).resolve().parents[1]
slots=('eye','ear','brain','skin','mouth')
extra=sorted(p.name for p in (root/'modules').iterdir()
             if p.is_dir() and p.name not in slots and (p/'modkit.c').is_file())
if any(not re.fullmatch(r'[a-z][a-z0-9_]*', name) for name in extra):
    raise SystemExit('ERROR: nombres de módulos solo [a-z][a-z0-9_]*')
found=[s for s in slots if (root/'modules'/s/'modkit.c').is_file()] + extra
lines=['/* GENERATED; do not edit by hand. */','#include "../../include/bones/modkit_host.h"']
for s in found: lines.append(f'extern int32_t bones_mod_{s}_query(uint32_t, BonesModDescriptor *);')
lines += ['int32_t bones_mod_register_builtins(BonesModHost *host) {','    int32_t rc;','    if (!host) return BONES_MOD_EINVAL;']
for s in found: lines += [f'    rc=bones_mod_host_add(host,bones_mod_{s}_query);', '    if (rc!=BONES_MOD_OK) return rc;']
lines += ['    (void)rc;', '    return BONES_MOD_OK;', '}', '']
(root/'src/generated/modkit_registry.c').write_text('\n'.join(lines),encoding='utf-8')
print('ModKit providers:',', '.join(found))
