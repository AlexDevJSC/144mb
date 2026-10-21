/* EAR integration SHELL: replace/extend in this folder, not in Bones core.
   This is not a functional EAR implementation. */
#include "../../include/bones/foundation.h"
BonesSubsystem bones_mod_ear_create(void *foundation_context) {
    BonesSubsystem m={0};
    m.version=BONES_SUBSYSTEM_API_VERSION;
    m.struct_size=(uint32_t)sizeof(m);
    m.slot=BONES_SLOT_EAR;
    m.context=foundation_context; /* BonesFoundation*, owned by app */
    return m;
}
