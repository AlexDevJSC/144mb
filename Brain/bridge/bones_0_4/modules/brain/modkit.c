/* Brain v0.3.1 — Bones v0.4 ModKit ABI1; no Bones core changes.
   Bones' static build compiles only modules/brain/modkit.c; the original
   NPC decision source is included exactly once, NOT separately linked. */
#include "../../include/bones/modkit.h"
#include "brain.h"
#include "brain_mod_diagnostics.h"
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "brain_npc.c"

#define BRAIN_MOD_TICKS_PER_MINUTE 60u /* Placeholder, not canonical game time. */
#define BRAIN_MOD_START_MINUTE 480u     /* 08:00 start in demo. */
#define BRAIN_MOD_NPC_KIND 2u           /* BONES_KIND_NPC in Bones 0.4. */
#define BRAIN_STALL_TICKS 8u            /* Prevent endlessly walking into wall. */
#define BRAIN_TURN_TICKS 14u            /* Bounded, local obstacle recovery. */
#define BRAIN_PROGRESS_MIN_SQ 0.0001f  /* 1 cm / tick progress tolerance. */

typedef struct {
    uint64_t id;
    uint64_t last_tick; /* A previous ACCEPTED forward command only. */
    uint64_t seen_tick;
    float prev_x,prev_z;
    uint8_t forward_previous;
    uint8_t stall_count;
    uint8_t turn_remaining;
} BrainPatrolTrack;

typedef struct {
    const BonesModServices *services;
    BrainModDiagnostics diagnostic;
    BrainPatrolTrack actors[BONES_MOD_MAX_ACTORS];
} BrainModRuntime;
static BrainModRuntime brain_runtime; /* Single world per Bones 0.4 host. */

static void bump(uint64_t *counter) {
    if (*counter != UINT64_MAX) ++*counter;
}
int brain_mod_diagnostics_snapshot(BrainModDiagnostics *out) {
    if (!out || out->size != sizeof(*out)) return 0;
    *out = brain_runtime.diagnostic;
    return 1;
}
static int32_t brain_start(void *user,const BonesModServices *s) {
    BrainModRuntime *state=(BrainModRuntime *)user;
    const uint32_t permissions = BONES_PERMISSION_READ_ACTORS |
                                 BONES_PERMISSION_READ_TIME |
                                 BONES_PERMISSION_SEND_COMMANDS;
    if (!state || !s || s->size!=sizeof(*s) || s->abi!=BONES_MOD_ABI ||
        (s->permissions & permissions)!=permissions || !s->host_context ||
        !s->tick || !s->actor_count || !s->actor_at || !s->submit_move)
        return BONES_MOD_EINVAL;
    memset(state,0,sizeof(*state));
    state->diagnostic.size=sizeof(state->diagnostic);
    state->diagnostic.version=BRAIN_MOD_DIAGNOSTICS_VERSION;
    state->services=s;
    return BONES_MOD_OK;
}
static void brain_stop(void *user) {
    BrainModRuntime *state=(BrainModRuntime *)user;
    if (state) {
        state->services=NULL;
        memset(state->actors,0,sizeof(state->actors));
    }
}
static BrainPatrolTrack *tracking(BrainModRuntime *state,uint64_t id) {
    uint32_t i;
    BrainPatrolTrack *empty=NULL;
    for(i=0;i<BONES_MOD_MAX_ACTORS;i++) {
        BrainPatrolTrack *p=&state->actors[i];
        if(p->id==id) return p;
        if(!p->id && !empty) empty=p;
    }
    if(empty){memset(empty,0,sizeof(*empty));empty->id=id;}
    return empty;
}
static void brain_before_tick(void *user) {
    BrainModRuntime *state=(BrainModRuntime *)user;
    const BonesModServices *s;
    uint64_t tick,minute;
    int32_t count,index;
    BrainClock clock;
    if(!state || !(s=state->services)) return;
    tick=s->tick(s->host_context);
    if(tick==UINT64_MAX ||
       tick/BRAIN_MOD_TICKS_PER_MINUTE>UINT64_MAX-BRAIN_MOD_START_MINUTE) return;
    count=s->actor_count(s->host_context);
    if(count<0 || count>(int32_t)BONES_MOD_MAX_ACTORS){
        bump(&state->diagnostic.rejected_actor_counts);return;
    }
    minute=BRAIN_MOD_START_MINUTE+tick/BRAIN_MOD_TICKS_PER_MINUTE;
    clock.minute=minute;
    clock.tick_remainder=(uint32_t)(tick%BRAIN_MOD_TICKS_PER_MINUTE);
    clock.ticks_per_minute=BRAIN_MOD_TICKS_PER_MINUTE;
    for(index=0;index<count;index++){
        BonesModActor actor;
        BrainNpc npc;
        BrainIntent intention;
        BrainPatrolTrack *p;
        BonesModCommand cmd;
        uint64_t phase;
        int rc;
        memset(&actor,0,sizeof(actor));actor.size=sizeof(actor);
        if(s->actor_at(s->host_context,(uint32_t)index,&actor)!=BONES_MOD_OK){
            bump(&state->diagnostic.actor_read_failures);continue;
        }
        if(actor.owner!=0){bump(&state->diagnostic.actors_skipped_not_owned);continue;}
        if(actor.kind!=BRAIN_MOD_NPC_KIND || !actor.id ||
           !isfinite(actor.x) || !isfinite(actor.z))continue;
        memset(&npc,0,sizeof(npc));npc.id=actor.id;
        npc.role=BRAIN_ROLE_GUARD; /* No role/health in BonesModActor. */
        npc.health_percent=100;
        if(brain_npc_decide(&npc,&clock,NULL,&intention)!=BRAIN_OK ||
           intention.activity!=BRAIN_ACT_PATROL)continue;
        p=tracking(state,actor.id);
        /* When actor slots recycle, a failed/old command cannot count as a
           wall impact. In particular, rejected orders must not trigger turns. */
        if(p){
            if(p->last_tick!=0 && tick==p->last_tick && p->forward_previous){
                const float dx=actor.x-p->prev_x,dz=actor.z-p->prev_z;
                if(dx*dx+dz*dz < BRAIN_PROGRESS_MIN_SQ) {
                    if(p->stall_count < BRAIN_STALL_TICKS) p->stall_count++;
                } else p->stall_count=0;
                if(p->stall_count>=BRAIN_STALL_TICKS){
                    p->turn_remaining=BRAIN_TURN_TICKS;
                    p->stall_count=0;
                    bump(&state->diagnostic.obstacle_recoveries);
                }
            } else if(p->last_tick && tick!=p->last_tick) {
                /* skipped ticks, rejected command or out-of-order tick */
                p->stall_count=0;
                p->turn_remaining=0;
            }
            p->seen_tick=tick;
        }
        memset(&cmd,0,sizeof(cmd));cmd.size=sizeof(cmd);
        cmd.actor_id=actor.id;cmd.tick=tick+1;cmd.sequence=tick+1;
        cmd.issuer=0;
        phase=(tick+(actor.id&UINT64_C(255)))%UINT64_C(240);
        if(p && p->turn_remaining){
            cmd.forward=0;
            cmd.yaw_milliradians=100; /* in place; Bones applies collision */
        } else {
            cmd.forward=1;
            cmd.yaw_milliradians=phase<60?30:0;
        }
        rc=s->submit_move(s->host_context,&cmd);
        if(rc==BONES_MOD_OK){
            bump(&state->diagnostic.commands_accepted);
            if(p){
                p->last_tick=tick+1;
                p->forward_previous=(uint8_t)(cmd.forward!=0);
                p->prev_x=actor.x;p->prev_z=actor.z;
                if(p->turn_remaining)p->turn_remaining--;
            }
        } else {
            bump(&state->diagnostic.commands_rejected);
            /* Do not infer physical obstruction from a rejected order. */
            if(p){p->last_tick=0;p->forward_previous=0;
                  p->stall_count=0;p->turn_remaining=0;}
        }
    }
    /* Reclaim removed NPC IDs; no array-index-based state identity. */
    for(index=0;index<(int32_t)BONES_MOD_MAX_ACTORS;index++) {
        BrainPatrolTrack *p=&state->actors[index];
        if(p->id && p->seen_tick!=tick)memset(p,0,sizeof(*p));
    }
}
int32_t bones_mod_brain_query(uint32_t abi,BonesModDescriptor *out){
    if(abi!=BONES_MOD_ABI || !out || out->size!=sizeof(*out))return BONES_MOD_EINVAL;
    memset(out,0,sizeof(*out));out->size=sizeof(*out);out->abi=BONES_MOD_ABI;
    memcpy(out->id,"brain",sizeof("brain"));
    out->role=BONES_ROLE_BRAIN;
    out->implementation_version=4;
    out->state_schema_version=0;
    out->requested_permissions=BONES_PERMISSION_READ_ACTORS |
        BONES_PERMISSION_READ_TIME | BONES_PERMISSION_SEND_COMMANDS;
    out->user_context=&brain_runtime;
    out->start=brain_start;out->stop=brain_stop;
    out->before_tick=brain_before_tick;
    return BONES_MOD_OK;
}
#ifdef BRAIN_MOD_BUILD_SHARED
BONES_MOD_EXPORT int32_t BONES_MOD_CALL
bones_mod_query_v1(uint32_t abi,BonesModDescriptor *out){
    return bones_mod_brain_query(abi,out);
}
#endif
