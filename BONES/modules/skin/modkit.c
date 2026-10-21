/* Placeholder adapter for skin. No skin behaviour is implemented here.
   Replace callbacks in this folder; use only modkit.h service contracts. */
#include "../../include/bones/modkit.h"
#include <string.h>
int32_t bones_mod_skin_query(uint32_t abi,BonesModDescriptor *out) {
    if(abi!=BONES_MOD_ABI||!out||out->size!=sizeof(*out))return BONES_MOD_EINVAL;
    memset(out,0,sizeof(*out));out->size=sizeof(*out);out->abi=abi;
    memcpy(out->id,"skin",sizeof("skin"));out->implementation_version=1;out->role=3;
    out->requested_permissions=BONES_PERMISSION_READ_ACTORS|BONES_PERMISSION_READ_TIME|BONES_PERMISSION_READ_SNAPSHOT|BONES_PERMISSION_READ_FACTS|BONES_PERMISSION_RESTORE_SNAPSHOT;
    return BONES_MOD_OK;
}
