/* GENERATED; do not edit by hand. */
#include "../../include/bones/modkit_host.h"
extern int32_t bones_mod_eye_query(uint32_t, BonesModDescriptor *);
extern int32_t bones_mod_ear_query(uint32_t, BonesModDescriptor *);
extern int32_t bones_mod_brain_query(uint32_t, BonesModDescriptor *);
extern int32_t bones_mod_skin_query(uint32_t, BonesModDescriptor *);
extern int32_t bones_mod_mouth_query(uint32_t, BonesModDescriptor *);
int32_t bones_mod_register_builtins(BonesModHost *host) {
    int32_t rc;
    if (!host) return BONES_MOD_EINVAL;
    rc=bones_mod_host_add(host,bones_mod_eye_query);
    if (rc!=BONES_MOD_OK) return rc;
    rc=bones_mod_host_add(host,bones_mod_ear_query);
    if (rc!=BONES_MOD_OK) return rc;
    rc=bones_mod_host_add(host,bones_mod_brain_query);
    if (rc!=BONES_MOD_OK) return rc;
    rc=bones_mod_host_add(host,bones_mod_skin_query);
    if (rc!=BONES_MOD_OK) return rc;
    rc=bones_mod_host_add(host,bones_mod_mouth_query);
    if (rc!=BONES_MOD_OK) return rc;
    (void)rc;
    return BONES_MOD_OK;
}
