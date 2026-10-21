/* Skin ModKit ABI1 unity entry. The 0.4.0 build only collects modkit.c.
   Keep the implementation under modules/skin: no changes to Bones core. */
#include "../../include/bones/modkit.h"
#include "skin_font.c"
#include "skin_ui2.c"
#include "skin_modkit_impl.c"
#ifdef SKIN_MODKIT_DYNAMIC
/* Only when building Skin as a standalone DLL/SO, never for all builtins. */
BONES_MOD_EXPORT int32_t BONES_MOD_CALL bones_mod_query_v1(
    uint32_t abi, BonesModDescriptor *out) { return bones_mod_skin_query(abi,out); }
#endif
