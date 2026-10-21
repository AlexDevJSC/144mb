#ifndef EAR_BONES_MODULE_H
#define EAR_BONES_MODULE_H

/* Optional adapter. Must be compiled against the ACTUAL Bones release headers.
   The Bones sources were not available in the present tool environment. */
#include "bones/bones.h"
/* This legacy adapter uses BonesModule v1. Bones API v2 has BonesSubsystem.
   Never silently present this as an adapter for the modern module model. */
#if BONES_API_VERSION != 1u
#error "Legacy EarBonesModule supports only Bones API v1; see ear_bones_contacts.h and contract/CONTRATO_EYE_EAR_BONES_v1.txt for Bones v2 extension."
#endif
#include "ear_bones_bridge.h"

typedef struct {
    EarBonesBridge bridge;
    BonesModule module;
} EarBonesModule;

void ear_bones_module_init(EarBonesModule *m, EarEngine *audio,
                           uint32_t world_seed);

#endif
