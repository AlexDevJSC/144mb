#include "../include/bones/modkit_host.h"
#include "../src/demo/test_house.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static BonesFoundation f;
static uint64_t npc;
static int tick_calls, frame_calls, start_calls, stop_calls, event_calls;
static int snapshot_ok, facts_ok, command_denied, event_denied, manifest_ok;
static const BonesModServices *brain_api,*ear_api,*skin_api,*eye_api;
static void init(void){
    assert(bones_foundation_init(&f,0x1440001u,1234u,1u));
    assert(bones_foundation_spawn(&f,BONES_KIND_NPC,0,(BonesVec3){0,0,-3},&npc));
    tick_calls=frame_calls=start_calls=stop_calls=event_calls=0;
    snapshot_ok=facts_ok=command_denied=event_denied=manifest_ok=0;
    brain_api=ear_api=skin_api=eye_api=NULL;
}
static int32_t basic_start(void *ctx,const BonesModServices *s){
    const BonesModServices **p=ctx;
    assert(s->size==sizeof(*s) && s->abi==BONES_MOD_ABI);
    if(p)*p=s;
    start_calls++;return BONES_MOD_OK;
}
static void basic_stop(void *ctx){(void)ctx;stop_calls++;}
static int32_t brain_start(void *u,const BonesModServices *s){
    (void)u;assert((s->permissions & BONES_PERMISSION_SEND_COMMANDS)!=0);
    brain_api=s;return basic_start(NULL,s);
}
static void brain_before(void *u){
    BonesModCommand c={0};BonesModActor a={0};(void)u;
    a.size=sizeof(a);assert(brain_api->actor_get(brain_api->host_context,npc,&a)==BONES_MOD_OK);
    assert(a.owner==0);
    c.size=sizeof(c);c.actor_id=npc;c.tick=brain_api->tick(brain_api->host_context)+1;
    c.sequence=c.tick;c.issuer=2;c.forward=1;
    if(brain_api->submit_move(brain_api->host_context,&c)==BONES_MOD_DENIED)command_denied++;
    c.issuer=0;
    assert(brain_api->submit_move(brain_api->host_context,&c)==BONES_MOD_OK);
    tick_calls++;
}
static int32_t ear_start(void *u,const BonesModServices *s){ear_api=s;return basic_start(u,s);}
static void ear_event(void *u,const BonesModEvent *event){
    BonesModEvent fake={0};(void)u;
    if(event->type==BONES_EVT_JUMP||event->type==BONES_EVT_LAND)event_calls++;
    fake.size=sizeof(fake);fake.type=BONES_EVT_CUSTOM;
    if(ear_api->emit_event(ear_api->host_context,&fake)==BONES_MOD_DENIED)event_denied++;
}
static int32_t skin_start(void *u,const BonesModServices *s){skin_api=s;return basic_start(u,s);}
static void skin_after(void *u){
    uint8_t buffer[BONES_FOUNDATION_SNAPSHOT_MAX];uint32_t written=0;
    BonesModFact fact={0};BonesModInfo info={0};int32_t count;
    (void)u;
    if(skin_api->snapshot_export(skin_api->host_context,buffer,sizeof(buffer),&written)==BONES_MOD_OK && written>80)
        snapshot_ok++;
    fact.size=sizeof(fact);
    count=skin_api->facts_since(skin_api->host_context,0,&fact,1);
    if(count==1 && fact.kind==BONES_FACT_SPAWN && fact.subject==npc) facts_ok++;
    info.size=sizeof(info);
    if(skin_api->module_count(skin_api->host_context)==4 &&
       skin_api->module_at(skin_api->host_context,0,&info)==BONES_MOD_OK &&
       info.implementation_version==1) manifest_ok++;
}
static int32_t eye_start(void *u,const BonesModServices *s){eye_api=s;return basic_start(u,s);}
static void eye_build(void *u) {
    BonesModQuad q={0};int32_t rc;uint32_t i;(void)u;
    q.size=sizeof(q);q.material_id=1;
    for(i=0;i<4;i++)q.vertices[i]=(BonesModVertex){(float)i,0,3,1,1};
    rc=eye_api->scene_add_quad(eye_api->host_context,&q);
    assert(rc==BONES_MOD_OK);
    q.material_id=999;
    assert(eye_api->scene_add_quad(eye_api->host_context,&q)==BONES_MOD_EINVAL);
}
static void eye_present(void *u,const BonesModFrame *frame){
    (void)u;assert(frame->size==sizeof(*frame));assert(frame->quad_count>=1);
    assert(frame->quads[frame->quad_count-1].material_id==1);
    frame_calls++;
}
static int32_t fill(BonesModDescriptor *out,const char *id,uint32_t role,uint32_t perms) {
    if(!out||out->size!=sizeof(*out))return BONES_MOD_EINVAL;
    memset(out,0,sizeof(*out));out->size=sizeof(*out);out->abi=BONES_MOD_ABI;
    strncpy(out->id,id,sizeof(out->id)-1);out->role=role;out->implementation_version=1;out->requested_permissions=perms;
    out->stop=basic_stop;return BONES_MOD_OK;
}
static int32_t q_brain(uint32_t abi,BonesModDescriptor *out){
    if(abi!=BONES_MOD_ABI)return BONES_MOD_EINVAL;
    fill(out,"brain_mock",BONES_ROLE_BRAIN,BONES_PERMISSION_SEND_COMMANDS|BONES_PERMISSION_READ_ACTORS|BONES_PERMISSION_READ_TIME);
    out->start=brain_start;out->before_tick=brain_before;return BONES_MOD_OK;
}
static int32_t q_ear(uint32_t abi,BonesModDescriptor *out){
    if(abi!=BONES_MOD_ABI)return BONES_MOD_EINVAL;
    fill(out,"ear_mock",BONES_ROLE_EAR,BONES_PERMISSION_READ_ACTORS);
    out->start=ear_start;out->on_event=ear_event;return BONES_MOD_OK;
}
static int32_t q_skin(uint32_t abi,BonesModDescriptor *out){
    if(abi!=BONES_MOD_ABI)return BONES_MOD_EINVAL;
    fill(out,"skin_mock",BONES_ROLE_SKIN,BONES_PERMISSION_READ_SNAPSHOT|BONES_PERMISSION_READ_FACTS|BONES_PERMISSION_LIST_MODULES);
    out->start=skin_start;out->after_tick=skin_after;return BONES_MOD_OK;
}
static int32_t q_eye(uint32_t abi,BonesModDescriptor *out){
    if(abi!=BONES_MOD_ABI)return BONES_MOD_EINVAL;
    fill(out,"eye_mock",BONES_ROLE_EYE,BONES_PERMISSION_WRITE_SCENE);
    out->start=eye_start;out->build_scene=eye_build;out->present=eye_present;return BONES_MOD_OK;
}
static int32_t q_bad_perms(uint32_t abi,BonesModDescriptor *out){
    if(abi!=BONES_MOD_ABI)return BONES_MOD_EINVAL;
    fill(out,"ear_bad",BONES_ROLE_EAR,BONES_PERMISSION_SEND_COMMANDS);return BONES_MOD_OK;
}
static int32_t q_missing(uint32_t abi,BonesModDescriptor *out){
    if(abi!=BONES_MOD_ABI)return BONES_MOD_EINVAL;
    fill(out,"dependent",BONES_ROLE_EXTENSION,BONES_PERMISSION_READ_TIME);
    out->required_roles=BONES_ROLE_BIT(BONES_ROLE_MOUTH);return BONES_MOD_OK;
}
static int32_t q_cycle_brain(uint32_t abi,BonesModDescriptor *out){
    if(abi!=BONES_MOD_ABI)return BONES_MOD_EINVAL;
    fill(out,"cycle_b",BONES_ROLE_BRAIN,0);out->required_roles=BONES_ROLE_BIT(BONES_ROLE_EAR);return BONES_MOD_OK;
}
static int32_t q_cycle_ear(uint32_t abi,BonesModDescriptor *out){
    if(abi!=BONES_MOD_ABI)return BONES_MOD_EINVAL;
    fill(out,"cycle_e",BONES_ROLE_EAR,0);out->required_roles=BONES_ROLE_BIT(BONES_ROLE_BRAIN);return BONES_MOD_OK;
}
static int32_t fail_start(void *u,const BonesModServices *s){(void)u;(void)s;start_calls++;return -77;}
static int32_t q_failed(uint32_t abi,BonesModDescriptor *out){
    if(abi!=BONES_MOD_ABI)return BONES_MOD_EINVAL;
    fill(out,"failed",BONES_ROLE_EXTENSION,0);out->required_roles=BONES_ROLE_BIT(BONES_ROLE_BRAIN);
    out->start=fail_start;return BONES_MOD_OK;
}
static void test_lifecycle_and_physics(void){
    BonesModHost *h;BonesInput in={0};uint32_t i;
    init();h=bones_mod_host_create(&f);assert(h);
    assert(bones_mod_host_add(h,q_eye)==0);
    assert(bones_mod_host_add(h,q_skin)==0);
    assert(bones_mod_host_add(h,q_ear)==0);
    assert(bones_mod_host_add(h,q_brain)==0);
    assert(bones_mod_host_add(h,q_brain)==BONES_MOD_EINVAL);
    assert(bones_mod_host_start(h)==0);
    assert(start_calls==4);
    assert(bones_mod_host_frame(h)==0 && frame_calls==1);
    in.jump=1;
    for(i=0;i<60;i++) {
        if(i)in.jump=0;
        assert(bones_mod_host_step(h,&in)==0);
    }
    assert(tick_calls==60 && command_denied==60);
    assert(event_calls>=2 && event_denied==event_calls);
    assert(snapshot_ok>0 && facts_ok>0 && manifest_ok>0);
    assert(bones_foundation_find(&f,npc)->body.feet.z>-3);
    bones_mod_host_stop(h);assert(stop_calls==4);
    assert(bones_mod_host_step(h,NULL)==BONES_MOD_BAD_STATE);
    bones_mod_host_destroy(h);
    puts("PASS ModKit: capacidades, escena, Brain, Skin, Ear, ciclo, fisicas");
}
static void test_dependencies_and_rollback(void){
    BonesModHost *h;
    init();h=bones_mod_host_create(&f);assert(h);
    assert(bones_mod_host_add(h,q_bad_perms)==BONES_MOD_EINVAL);
    assert(bones_mod_host_add(h,q_missing)==0);
    assert(bones_mod_host_start(h)==BONES_MOD_MISSING);
    bones_mod_host_destroy(h);
    h=bones_mod_host_create(&f);assert(h);
    assert(bones_mod_host_add(h,q_cycle_brain)==0);
    assert(bones_mod_host_add(h,q_cycle_ear)==0);
    assert(bones_mod_host_start(h)==BONES_MOD_EINVAL);
    assert(start_calls==0);
    bones_mod_host_destroy(h);
    h=bones_mod_host_create(&f);assert(h);
    assert(bones_mod_host_add(h,q_brain)==0);
    assert(bones_mod_host_add(h,q_failed)==0);
    assert(bones_mod_host_start(h)==-77);
    assert(start_calls==2&&stop_calls==1);
    bones_mod_host_destroy(h);
    puts("PASS ModKit: permisos denegados, dependencias, ciclos y rollback");
}
static void test_builtin_ports(void){
    BonesModHost *h;
    init();h=bones_mod_host_create(&f);assert(h);
    assert(bones_mod_register_builtins(h)==0);
    assert(bones_mod_host_count(h)==5);
    assert(bones_mod_host_start(h)==0);
    assert(bones_mod_host_step(h,NULL)==0);
    assert(bones_mod_host_frame(h)==0);
    bones_mod_host_destroy(h);
    puts("PASS ModKit: cinco puertos registrables y Eye heredado renderiza");
}
static uint8_t cold_buffer[BONES_FOUNDATION_SNAPSHOT_MAX];
static uint32_t cold_length;
static int cold_restore_ok;
static int32_t cold_start(void *u,const BonesModServices *services){
    (void)u;skin_api=services;
    cold_restore_ok=(services->snapshot_restore_at_start(services->host_context,cold_buffer,cold_length)==BONES_MOD_OK);
    return cold_restore_ok?BONES_MOD_OK:BONES_MOD_EINVAL;
}
static int32_t q_cold_skin(uint32_t abi,BonesModDescriptor *out){
    if(abi!=BONES_MOD_ABI)return BONES_MOD_EINVAL;
    fill(out,"cold_skin",BONES_ROLE_SKIN,BONES_PERMISSION_RESTORE_SNAPSHOT);
    out->start=cold_start;return BONES_MOD_OK;
}
static void test_skin_cold_restore(void){
    BonesModHost *h;BonesInput idle={0};size_t written=0;uint64_t expected;uint32_t i;
    init();
    for(i=0;i<5;i++)assert(bones_foundation_step(&f,&idle));
    expected=bones_foundation_hash(&f);
    assert(bones_foundation_export(&f,cold_buffer,sizeof(cold_buffer),&written));
    cold_length=(uint32_t)written;
    assert(bones_foundation_init(&f,0x1440001u,1234u,1u));
    cold_restore_ok=0;
    h=bones_mod_host_create(&f);assert(h);
    assert(bones_mod_host_add(h,q_cold_skin)==0);
    assert(bones_mod_host_start(h)==0);
    assert(cold_restore_ok && f.engine.tick==5 && bones_foundation_hash(&f)==expected);
    assert(skin_api->snapshot_restore_at_start(skin_api->host_context,cold_buffer,cold_length)==BONES_MOD_BAD_STATE);
    bones_mod_host_destroy(h);
    puts("PASS ModKit: Skin recupera estado SOLO en arranque en frio");
}
int main(void){test_lifecycle_and_physics();test_dependencies_and_rollback();test_builtin_ports();test_skin_cold_restore();return 0;}
