/*
 * Mouth for Bones ModKit 0.4.0, ABI 1.
 *
 * Bones' standard source registry compiles ONLY modules/*/modkit.c.
 * This intentionally includes the transport implementation in the same
 * translation unit: do not add mouth_lan.c separately to the link.
 * The original, independently tested transport lives in Mouth/src/.
 *
 * Offline default: registering Mouth before any LAN configuration is safe
 * and does not open a socket. The game host must call
 * mouth_lan_configure_host(&owned_context, &config) BEFORE registration
 * to enable UDP. Bones must spawn the remote actor first.
 *
 * No Bones core sources or ABI are modified.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "mouth_lan.h"
#include <string.h>
#include "mouth_lan.c"

int32_t bones_mod_mouth_query(uint32_t abi, BonesModDescriptor *out) {
    /* In configured host mode, use Mouth's real ModKit implementation. */
    if (mouth_lan_query(abi, out) == BONES_MOD_OK) {
        return BONES_MOD_OK;
    }
    /* Preserve Bones' singleplayer path if LAN has not been configured.
       No callbacks/privileges: harmless but discoverable optional provider. */
    if (abi != BONES_MOD_ABI || !out || out->size != sizeof(*out))
        return BONES_MOD_EINVAL;
    memset(out, 0, sizeof(*out));
    out->size = (uint32_t)sizeof(*out);
    out->abi = BONES_MOD_ABI;
    memcpy(out->id, "mouth", sizeof("mouth"));
    out->role = BONES_ROLE_MOUTH;
    out->implementation_version = 3u;
    out->state_schema_version = 0u;
    return BONES_MOD_OK;
}
