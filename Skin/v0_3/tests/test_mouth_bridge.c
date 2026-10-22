#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../../BONES/modules/skin/skin_mouth_bridge.h"
static uint64_t tick(void *p){(void)p;return 1;}
static int32_t count(void *p){(void)p;return 0;}
static int32_t at(void *p,uint32_t i,BonesModActor *a){(void)p;(void)i;(void)a;return 0;}
MouthSessionResult mouth_session_get_status(const MouthLan *l,MouthSessionInfo *out){
    (void)l;assert(out&&out->size==sizeof(*out));out->phase=MOUTH_SESSION_CONNECTED;out->error=0;out->port=14444;return MOUTH_SESSION_OK;
}
int main(void){
    BonesModDescriptor d={0};BonesModServices s={0};MouthLan lan={0};
    d.size=sizeof(d);assert(bones_mod_skin_query(BONES_MOD_ABI,&d)==BONES_MOD_OK);
    s.size=sizeof(s);s.abi=BONES_MOD_ABI;s.permissions=d.requested_permissions;
    s.tick=tick;s.actor_count=count;s.actor_at=at;
    assert(d.start(d.user_context,&s)==0);
    assert(bones_skin_ui_refresh_mouth(&lan,1)==0);
    assert(bones_skin_ui_state()->lan_phase==MOUTH_SESSION_CONNECTED);
    assert(bones_skin_ui_state()->lan_current_port==14444);
    d.stop(d.user_context);
    puts("PASS optional typed Mouth bridge: uses MouthSessionInfo, no network access");
    return 0;
}
