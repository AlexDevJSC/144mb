/* Placeholder adapter for brain. No brain behaviour is implemented here.
   Replace callbacks in this folder; use only modkit.h service contracts. */
#include "../../include/bones/modkit.h"
#include <string.h>
int32_t bones_mod_brain_query(uint32_t abi,BonesModDescriptor *out) {
    if(abi!=BONES_MOD_ABI||!out||out->size!=sizeof(*out))return BONES_MOD_EINVAL;
    memset(out,0,sizeof(*out));out->size=sizeof(*out);out->abi=abi;
    memcpy(out->id,"brain",sizeof("brain"));out->implementation_version=1;out->role=2;
    out->requested_permissions=BONES_PERMISSION_READ_ACTORS|BONES_PERMISSION_READ_TIME|BONES_PERMISSION_SEND_COMMANDS|BONES_PERMISSION_EMIT_EVENTS;
    return BONES_MOD_OK;
}
