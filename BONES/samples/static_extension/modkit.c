/* Copy this folder to modules/clock_ext/ and rerun generator: no core edits. */
#include "../../include/bones/modkit.h"
#include <string.h>
static const BonesModServices *services;
static int32_t start(void *ctx,const BonesModServices *api){
    (void)ctx;services=api;
    return (api->abi==BONES_MOD_ABI)?BONES_MOD_OK:BONES_MOD_EINVAL;
}
static void after_tick(void *ctx){(void)ctx;if(services)(void)services->tick(services->host_context);}
static void stop(void *ctx){(void)ctx;services=NULL;}
int32_t bones_mod_clock_ext_query(uint32_t abi,BonesModDescriptor *out){
    if(abi!=BONES_MOD_ABI||!out||out->size!=sizeof(*out))return BONES_MOD_EINVAL;
    memset(out,0,sizeof(*out));out->size=sizeof(*out);out->abi=abi;
    memcpy(out->id,"clock_ext",10);out->role=BONES_ROLE_EXTENSION;out->implementation_version=1;
    out->requested_permissions=BONES_PERMISSION_READ_TIME;
    out->start=start;out->after_tick=after_tick;out->stop=stop;
    return BONES_MOD_OK;
}
