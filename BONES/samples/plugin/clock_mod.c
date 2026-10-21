/* Shared-library smoke test: developer-mode optional plugin. No internal Bones headers.
   Linux: cc -shared -fPIC -std=c11 -o build/clock_mod.so samples/plugin/clock_mod.c
   Windows: cl /LD /TC samples\\plugin\\clock_mod.c /Febuild\\clock_mod.dll */
#include "../../include/bones/modkit.h"
#include <string.h>
static const BonesModServices *api;
static int32_t start(void *user,const BonesModServices *services){
    (void)user;
    if(services->abi!=BONES_MOD_ABI||!(services->permissions&BONES_PERMISSION_READ_TIME))return BONES_MOD_EINVAL;
    api=services;return BONES_MOD_OK;
}
static void after(void *user){(void)user; if(api) (void)api->tick(api->host_context);}
static void stop(void *user){(void)user;api=0;}
BONES_MOD_EXPORT int32_t BONES_MOD_CALL bones_mod_query_v1(uint32_t abi,BonesModDescriptor *out){
    if(!out||out->size!=sizeof(*out)||abi!=BONES_MOD_ABI)return BONES_MOD_EINVAL;
    memset(out,0,sizeof(*out));out->size=sizeof(*out);out->abi=abi;
    memcpy(out->id,"clock_probe",12);out->role=BONES_ROLE_EXTENSION;out->implementation_version=1;
    out->requested_permissions=BONES_PERMISSION_READ_TIME;
    out->start=start;out->after_tick=after;out->stop=stop;
    return BONES_MOD_OK;
}
