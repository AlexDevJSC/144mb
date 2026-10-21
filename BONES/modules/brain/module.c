/* BRAIN integration SHELL: replace/extend in this folder, not in Bones core.
   This is not a functional BRAIN implementation. */
#include "../../include/bones/foundation.h"
BonesSubsystem bones_mod_brain_create(void *foundation_context) {
    BonesSubsystem m={0};
    m.version=BONES_SUBSYSTEM_API_VERSION;
    m.struct_size=(uint32_t)sizeof(m);
    m.slot=BONES_SLOT_BRAIN;
    m.context=foundation_context; /* BonesFoundation*, owned by app */
    return m;
}
