#ifndef EAR_BONES_MODULE_H
#define EAR_BONES_MODULE_H

/* Optional adapter. Must be compiled against the ACTUAL Bones release headers.
   The Bones sources were not available in the present tool environment. */
#include "bones/bones.h"
#include "ear_bones_bridge.h"

typedef struct {
    EarBonesBridge bridge;
    BonesModule module;
} EarBonesModule;

void ear_bones_module_init(EarBonesModule *m, EarEngine *audio,
                           uint32_t world_seed);

#endif
