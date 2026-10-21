/* ModKit 0.4 contract tests. Run from Ear/tests/ against real Bones header.
   The source under test is BONES/modules/ear/modkit.c; no private Bones API. */
#include "../../BONES/include/bones/modkit.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
extern int32_t bones_mod_ear_query(uint32_t,BonesModDescriptor *);
extern int ear_mod_test_render(int16_t *,size_t);
extern uint32_t ear_mod_test_played(void);
extern uint32_t ear_mod_test_invalid(void);
extern uint32_t ear_mod_test_receives(void);
static uint64_t fake_tick(void *p){(void)p;return 42u;}
static int32_t fake_get(void *p,uint64_t id,BonesModActor *out){
    (void)p;
    if(!out||out->size!=sizeof(*out)||id!=11u)return BONES_MOD_MISSING;
    memset(out,0,sizeof(*out));out->size=sizeof(*out);out->id=id;
    out->grounded=1;return BONES_MOD_OK;
}
static BonesModServices service(void){
    BonesModServices s={0};
    s.size=sizeof(s);s.abi=BONES_MOD_ABI;
    s.permissions=BONES_PERMISSION_READ_ACTORS|BONES_PERMISSION_READ_TIME;
    s.tick=fake_tick;s.actor_get=fake_get;
    return s;
}
static int16_t pcm[8192];
static uint64_t hash_pcm(void){
    uint64_t h=UINT64_C(14695981039346656037);size_t i;
    for(i=0;i<8192;i++){
        uint16_t v=(uint16_t)pcm[i];
        h^=(uint8_t)v;h*=UINT64_C(1099511628211);
        h^=(uint8_t)(v>>8);h*=UINT64_C(1099511628211);
    }return h;
}
static uint64_t run(uint32_t type,uint64_t source){
    BonesModDescriptor d={0};BonesModServices s=service();
    BonesModEvent ev={0};uint64_t h;
    d.size=sizeof(d);
    assert(bones_mod_ear_query(BONES_MOD_ABI,&d)==BONES_MOD_OK);
    assert(d.start(d.user_context,&s)==BONES_MOD_OK);
    ev.size=sizeof(ev);ev.type=type;ev.source_id=source;ev.tick=42;
    d.on_event(d.user_context,&ev);
    assert(ear_mod_test_render(pcm,8192));
    h=hash_pcm();
    d.stop(d.user_context);
    return h;
}
int main(void){
    BonesModDescriptor d={0}; BonesModServices s=service();
    BonesModEvent ev={0};uint64_t land1,land2,jump;
    size_t i;int nonzero=0;
    d.size=sizeof(d)-1;
    assert(bones_mod_ear_query(BONES_MOD_ABI,&d)==BONES_MOD_EINVAL);
    d.size=sizeof(d);
    assert(bones_mod_ear_query(BONES_MOD_ABI+1,&d)==BONES_MOD_EINVAL);
    assert(bones_mod_ear_query(BONES_MOD_ABI,&d)==BONES_MOD_OK);
    assert(d.abi==BONES_MOD_ABI && d.role==BONES_ROLE_EAR);
    assert(strcmp(d.id,"ear")==0);
    assert(d.implementation_version==3 && d.state_schema_version==0);
    assert(d.required_roles==0);
    assert(d.requested_permissions==
       (BONES_PERMISSION_READ_ACTORS|BONES_PERMISSION_READ_TIME));
    assert(d.start && d.stop && d.on_event && d.after_tick);
    assert(!d.build_scene && !d.present && !d.before_tick);
    s.permissions=BONES_PERMISSION_READ_TIME;
    assert(d.start(d.user_context,&s)==BONES_MOD_EINVAL);
    s=service();
    assert(d.start(d.user_context,&s)==BONES_MOD_OK);
    d.after_tick(d.user_context);
    ev.size=sizeof(ev);ev.type=BONES_EVT_CUSTOM;ev.source_id=11;ev.tick=42;
    d.on_event(d.user_context,&ev);
    assert(ear_mod_test_receives()==0 && ear_mod_test_played()==0);
    ev.type=BONES_EVT_LAND;ev.source_id=999;
    d.on_event(d.user_context,&ev);
    assert(ear_mod_test_invalid()==1);
    ev.source_id=11;ev.size=0;
    d.on_event(d.user_context,&ev);
    assert(ear_mod_test_invalid()==2);
    ev.size=sizeof(ev);
    d.on_event(d.user_context,&ev);
    assert(ear_mod_test_played()==1 && ear_mod_test_receives()==2);
    assert(ear_mod_test_render(pcm,8192));
    for(i=0;i<8192;++i)if(pcm[i])nonzero=1;
    assert(nonzero);
    d.stop(d.user_context);
    assert(!ear_mod_test_render(pcm,8192));
    land1=run(BONES_EVT_LAND,11);
    land2=run(BONES_EVT_LAND,11);
    jump=run(BONES_EVT_JUMP,11);
    assert(land1==land2 && land1!=jump);
    printf("PASS EAR Bones0.4 ModKit ABI1/permissions/lifecycle/events/PCM\n");
    printf("LAND_HASH %016llx JUMP_HASH %016llx\n",
           (unsigned long long)land1,(unsigned long long)jump);
    return 0;
}
