/* Bones ModKit v0.4 ABI 1 mock service suite (not actual Bones runtime). */
#include "../include/bones/modkit.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

int32_t bones_mod_brain_query(uint32_t abi, BonesModDescriptor *out);

typedef struct {
    uint64_t tick;
    BonesModActor actors[4];
    uint32_t count;
    BonesModCommand commands[256];
    uint32_t written;
    int32_t reject;
} HostMock;

static uint64_t get_tick(void *ctx) { return ((HostMock *)ctx)->tick; }
static int32_t actor_count(void *ctx) { return (int32_t)((HostMock *)ctx)->count; }
static int32_t actor_at(void *ctx,uint32_t i,BonesModActor *out) {
    HostMock *h = (HostMock *)ctx;
    if (!out || out->size != sizeof(*out) || i >= h->count) return BONES_MOD_EINVAL;
    *out = h->actors[i]; return BONES_MOD_OK;
}
static int32_t submit_move(void *ctx,const BonesModCommand *c) {
    HostMock *h = (HostMock *)ctx;
    uint32_t i;
    if (h->reject) return h->reject;
    if (!c || c->size != sizeof(*c) || c->issuer != 0 || c->tick != h->tick+1 ||
        c->sequence != c->tick || c->forward < -1 || c->forward > 1 ||
        c->strafe < -1 || c->strafe > 1 || c->yaw_milliradians < -300 ||
        c->yaw_milliradians > 300 || c->jump > 1 || c->sprint > 1 ||
        h->written >= 256) return BONES_MOD_EINVAL;
    for (i=0;i<h->written;i++) if (h->commands[i].actor_id==c->actor_id &&
                                      h->commands[i].tick==c->tick) return BONES_MOD_EINVAL;
    h->commands[h->written++] = *c;
    return BONES_MOD_OK;
}
static void mock_actor(HostMock *h, uint32_t i, uint64_t id, uint32_t kind, uint32_t owner) {
    memset(&h->actors[i], 0, sizeof(BonesModActor));
    h->actors[i].size = sizeof(BonesModActor);
    h->actors[i].id = id;
    h->actors[i].kind = kind;
    h->actors[i].owner = owner;
}
static BonesModServices services_for(HostMock *h) {
    BonesModServices s={0};
    s.size = sizeof(s); s.abi = BONES_MOD_ABI;
    s.permissions = BONES_PERMISSION_READ_ACTORS | BONES_PERMISSION_READ_TIME |
                    BONES_PERMISSION_SEND_COMMANDS;
    s.host_context = h;
    s.tick = get_tick;
    s.actor_count = actor_count;
    s.actor_at = actor_at;
    s.submit_move = submit_move;
    return s;
}
int main(void) {
    HostMock host={0};
    BonesModDescriptor desc={0};
    BonesModServices api;
    uint32_t i;
    desc.size=sizeof(desc);
    assert(bones_mod_brain_query(999,&desc)==BONES_MOD_EINVAL);
    desc.size=0; assert(bones_mod_brain_query(BONES_MOD_ABI,&desc)==BONES_MOD_EINVAL);
    desc.size=sizeof(desc);assert(bones_mod_brain_query(BONES_MOD_ABI,&desc)==BONES_MOD_OK);
    assert(desc.abi==BONES_MOD_ABI && desc.role==BONES_ROLE_BRAIN);
    assert(desc.state_schema_version==0 && desc.implementation_version>=2);
    assert(strcmp(desc.id,"brain")==0 && desc.required_roles==0);
    assert(desc.before_tick && desc.start && desc.stop);
    assert(!desc.after_tick && !desc.present && !desc.build_scene);
    assert(desc.requested_permissions==(BONES_PERMISSION_READ_ACTORS |
                    BONES_PERMISSION_READ_TIME | BONES_PERMISSION_SEND_COMMANDS));
    mock_actor(&host, 0, 1, 1, 1); /* local player: never submit */
    mock_actor(&host, 1, 12, 2, 0); /* neutral NPC: brain-owned */
    mock_actor(&host, 2, 18, 2, 1); /* player-owned: never submit */
    mock_actor(&host, 3, 17, 3, 0); /* world object: never submit */
    host.count=4;host.tick=0;
    api=services_for(&host);
    assert(desc.start(desc.user_context,&api)==BONES_MOD_OK);
    desc.before_tick(desc.user_context);
    assert(host.written==1 && host.commands[0].actor_id==12 &&
           host.commands[0].tick==1 && host.commands[0].sequence==1 &&
           host.commands[0].issuer==0 && host.commands[0].forward==1);
    host.tick=1;
    desc.before_tick(desc.user_context);
    assert(host.written==2 && host.commands[1].tick==2);
    /* Ordering of enumeration does not change selected actor/command. */
    {
        BonesModActor temp=host.actors[0];
        host.actors[0]=host.actors[1];host.actors[1]=temp;
        host.tick=2;
        desc.before_tick(desc.user_context);
        assert(host.written==3 && host.commands[2].actor_id==12 &&
               host.commands[2].tick==3);
    }
    /* 22:00 -> guard asleep -> no command; 6:01 -> no patrol. */
    host.tick=(1320u-480u)*60u;
    desc.before_tick(desc.user_context);
    assert(host.written==3);
    host.tick=(1440u+360u-480u)*60u;
    desc.before_tick(desc.user_context);
    assert(host.written==3);
    host.tick=100;host.reject=BONES_MOD_DENIED;
    desc.before_tick(desc.user_context);
    assert(host.written==3); /* fail closed; callback cannot propagate failure */
    host.reject=0;host.tick=UINT64_MAX;
    desc.before_tick(desc.user_context);
    assert(host.written==3);
    host.tick=1;host.count=33;
    desc.before_tick(desc.user_context);
    assert(host.written==3);
    host.count=4;
    desc.stop(desc.user_context);
    desc.before_tick(desc.user_context);
    assert(host.written==3);
    /* Invalid services fail rather than calling foreign function pointers. */
    api=services_for(&host);api.permissions &= ~BONES_PERMISSION_SEND_COMMANDS;
    assert(desc.start(desc.user_context,&api)==BONES_MOD_EINVAL);
    api=services_for(&host);api.submit_move=NULL;
    assert(desc.start(desc.user_context,&api)==BONES_MOD_EINVAL);
    for(i=0;i<host.written;i++)assert(host.commands[i].issuer==0);
    puts("PASS Brain/Bones 0.4 ModKit ABI1: descriptor, ownership, phase, tick, bounds, fallback schedule, no player commands");
    return 0;
}
