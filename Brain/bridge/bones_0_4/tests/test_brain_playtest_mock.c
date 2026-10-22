/* Behavior/authority regression with a SIMULATED host + rectangular wall.
   Does not prove Bones collider behavior. See test_brain_real_bones.c. */
#include "../include/bones/modkit.h"
#include "../modules/brain/brain_mod_diagnostics.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int32_t bones_mod_brain_query(uint32_t, BonesModDescriptor *);
typedef struct {
    BonesModActor actors[4];
    uint32_t actor_total;
    uint64_t tick;
    int read_failure;
    int deny;
    int wall;
    int has_command;
    BonesModCommand command;
    unsigned submit_calls;
    unsigned forward_calls;
    unsigned turn_calls;
} Mock;
static uint64_t tick_now(void *ctx){return ((Mock*)ctx)->tick;}
static int32_t count(void *ctx){return (int32_t)((Mock*)ctx)->actor_total;}
static int32_t actor_at(void *ctx,uint32_t idx,BonesModActor *out) {
    Mock *m=(Mock*)ctx;
    if(m->read_failure && idx==1) return BONES_MOD_BAD_STATE;
    if(!out || out->size!=sizeof(*out) || idx>=m->actor_total) return BONES_MOD_EINVAL;
    *out=m->actors[idx];return BONES_MOD_OK;
}
static int32_t submit(void *ctx,const BonesModCommand *cmd) {
    Mock *m=(Mock*)ctx;
    assert(cmd && cmd->size==sizeof(*cmd));
    assert(cmd->issuer==0 && cmd->tick==m->tick+1 && cmd->sequence==cmd->tick);
    assert(cmd->actor_id==100 && cmd->forward>=0 && cmd->forward<=1);
    assert(cmd->yaw_milliradians>=-300 && cmd->yaw_milliradians<=300);
    m->submit_calls++;
    if(m->deny) return BONES_MOD_DENIED;
    assert(!m->has_command);
    m->command=*cmd;m->has_command=1;
    if(cmd->forward)m->forward_calls++;
    else m->turn_calls++;
    return BONES_MOD_OK;
}
static BonesModServices services(Mock *m){
    BonesModServices s={0};s.size=sizeof(s);s.abi=BONES_MOD_ABI;
    s.permissions=BONES_PERMISSION_READ_ACTORS|BONES_PERMISSION_READ_TIME|
                  BONES_PERMISSION_SEND_COMMANDS;
    s.host_context=m;s.tick=tick_now;s.actor_count=count;
    s.actor_at=actor_at;s.submit_move=submit;return s;
}
static void setup(Mock *m){
    memset(m,0,sizeof(*m));m->actor_total=3;
    m->actors[0].size=sizeof(BonesModActor);
    m->actors[0].id=1;m->actors[0].kind=1;m->actors[0].owner=1;
    m->actors[1].size=sizeof(BonesModActor);
    m->actors[1].id=100;m->actors[1].kind=2;m->actors[1].owner=0;
    m->actors[1].x=0;m->actors[1].z=-3.0f;
    m->actors[2].size=sizeof(BonesModActor);
    m->actors[2].id=101;m->actors[2].kind=2;m->actors[2].owner=7;
}
static BrainModDiagnostics diagnostic(void){
    BrainModDiagnostics d={0};d.size=sizeof(d);
    assert(brain_mod_diagnostics_snapshot(&d));
    assert(d.version==BRAIN_MOD_DIAGNOSTICS_VERSION);
    return d;
}
/* Fake physics is intentionally simple; Bones owns real movement/collision. */
static void step(Mock *m,const BonesModDescriptor *d){
    const float speed=.08f;
    m->has_command=0;
    d->before_tick(d->user_context);
    if(m->has_command) {
        const BonesModCommand *c=&m->command;
        BonesModActor *npc=&m->actors[1];
        float newx,newz;
        assert(c->actor_id==npc->id);
        npc->yaw+=(float)c->yaw_milliradians*.001f;
        newx=npc->x+sinf(npc->yaw)*speed*(float)c->forward;
        newz=npc->z+cosf(npc->yaw)*speed*(float)c->forward;
        /* Impassable wall: z=-1.5, spanning x[-5,5]. */
        if(!m->wall || !(newz >= -1.5f && newz<=-1.25f && fabsf(newx)<5.0f)){
            npc->x=newx;npc->z=newz;
        }
    }
    m->tick++;
}
static void scene(const char *name,int wall,int initial_denials){
    Mock m;
    BonesModServices api;
    BonesModDescriptor d={0};
    BrainModDiagnostics stats;
    unsigned i;
    setup(&m);m.wall=wall;
    api=services(&m);d.size=sizeof(d);
    assert(bones_mod_brain_query(BONES_MOD_ABI,&d)==BONES_MOD_OK);
    assert(d.start(d.user_context,&api)==BONES_MOD_OK);
    for(i=0;i<100;i++){
        float px=m.actors[0].x,pz=m.actors[0].z;
        m.actors[0].yaw=(float)((i%4)*1.5707963); /* player camera regression */
        m.deny=(i<(unsigned)initial_denials);
        step(&m,&d);
        assert(m.actors[0].x==px && m.actors[0].z==pz);
        if(wall)assert(m.actors[1].z<=-1.25f);
    }
    stats=diagnostic();
    assert(stats.commands_accepted==(uint64_t)(100-initial_denials));
    assert(stats.commands_rejected==(uint64_t)initial_denials);
    assert(stats.actors_skipped_not_owned>=200);
    if(wall && !initial_denials) {
        assert(stats.obstacle_recoveries>0);
        assert(m.turn_calls>0);
    } else if(!wall) {
        assert(stats.obstacle_recoveries==0);
        assert(m.actors[1].z>-3.0f);
    }
    printf("PASS MOCK %s: accept=%llu deny=%llu recovery=%llu npc=(%.2f,%.2f)\n",
        name,(unsigned long long)stats.commands_accepted,
        (unsigned long long)stats.commands_rejected,
        (unsigned long long)stats.obstacle_recoveries,
        m.actors[1].x,m.actors[1].z);
    d.stop(d.user_context);
}
int main(void){
    Mock m;BonesModServices s;BonesModDescriptor d={0};BrainModDiagnostics out;
    scene("open",0,0);scene("wall",1,0);scene("reject_then_recover",0,12);
    setup(&m);m.read_failure=1;s=services(&m);
    d.size=sizeof(d);assert(bones_mod_brain_query(BONES_MOD_ABI,&d)==BONES_MOD_OK);
    assert(d.start(d.user_context,&s)==BONES_MOD_OK);
    step(&m,&d);out=diagnostic();assert(out.actor_read_failures==1);
    assert(out.commands_accepted==0);d.stop(d.user_context);
    out.size=0;assert(!brain_mod_diagnostics_snapshot(&out));
    puts("PASS MOCK: read failure accounted, diagnostics reject wrong ABI size");
    return 0;
}
