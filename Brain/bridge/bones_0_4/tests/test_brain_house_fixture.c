/* BRAIN + BONES 0.4.1 REAL Foundation/ModKit and actual house fixture.
   Execute only against Soul-approved Bones PR #11 or its merge commit.
   NO mocked wall; collisions are evaluated by BonesFoundation and real BonesWorld.
   Does not replace a Windows interactive playtest. */
#include "bones/modkit_host.h"
#include "../modules/brain/brain_mod_diagnostics.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Intentionally use Bones' new demo fixture; its absence is a build FAILURE,
   never silently substitute a synthetic wall. */
#include "test_house.h"

int32_t bones_mod_brain_query(uint32_t, BonesModDescriptor *);
static BonesFoundation f;
typedef struct {
    float x,z,yaw;
    uint64_t accepted,rejected,recoveries;
    uint64_t player_seq,remote_seq;
} Trial;

static int overlaps_solid(const BonesWorld *world, const BonesPlayer *body) {
    uint32_t i;
    for(i=0;i<world->collider_count;i++) {
        const BonesCollider *c=&world->colliders[i];
        float cx,cz,dx,dz;
        if(!c->solid || body->feet.y >= c->bounds.max.y ||
           body->feet.y + body->height <= c->bounds.min.y)continue;
        cx=fmaxf(c->bounds.min.x,fminf(body->feet.x,c->bounds.max.x));
        cz=fmaxf(c->bounds.min.z,fminf(body->feet.z,c->bounds.max.z));
        dx=body->feet.x-cx; dz=body->feet.z-cz;
        if(dx*dx+dz*dz<body->radius*body->radius-0.0001f)return 1;
    }
    return 0;
}

/* Pre-advance 60 ticks so patrol begins on its straight section, aligned
   with the actual house entrance at X=0, front Z=2. No NPC gets teleported
   through a door; the host resolves every movement via BonesFoundation. */
static Trial trial(int open_door, int player_input, unsigned reject_first) {
    BonesModHost *host;
    BonesInput input={0};
    BonesVec3 start={0.0f,0.0f,0.0f};
    BrainModDiagnostics info={0};
    const BonesEntity *npc,*remote;
    BonesScene scene={0};
    uint64_t npc_id, remote_id;
    unsigned i;
    Trial out={0};
    assert(bones_foundation_init(&f,UINT64_C(0x144099),UINT64_C(0xBABE),1));
    assert(bones_house_register_colliders(&f.engine.world));
    assert(f.engine.world.collider_count==7u+BONES_HOUSE_COLLIDERS);
    assert(bones_house_append_world_scene(&scene,&f.engine.world));
    assert(scene.quad_count==BONES_HOUSE_QUADS);
    if(open_door){
        assert(bones_house_open_door(&f.engine.world));
        memset(&scene,0,sizeof(scene));
        assert(bones_house_append_world_scene(&scene,&f.engine.world));
        assert(scene.quad_count==BONES_HOUSE_QUADS-1u);
    }
    for(i=0;i<60;i++)assert(bones_foundation_step(&f,NULL));
    assert(bones_foundation_spawn(&f,BONES_KIND_NPC,0,start,&npc_id));
    assert(bones_foundation_spawn(&f,BONES_KIND_PLAYER,2,
                                  (BonesVec3){5,0,0},&remote_id));
    host=bones_mod_host_create(&f); assert(host);
    assert(bones_mod_host_add(host,bones_mod_brain_query)==BONES_MOD_OK);
    assert(bones_mod_host_start(host)==BONES_MOD_OK);
    for(i=0;i<95;i++) {
        uint64_t at=f.engine.tick;
        memset(&input,0,sizeof(input));
        if(player_input){
            input.forward=(uint8_t)((i%18u)<9u); /* real WASD channel */
            input.right=(uint8_t)((i%30u)<10u);
            input.yaw_right=1;
            input.jump=(uint8_t)(i==3u || i==46u);
        }
        /* This is a valid, higher-priority already-scheduled host order;
           Brain must observe EINVAL and NOT infer an obstacle. */
        if(i<reject_first){
            BonesCommand competing={0};
            competing.actor_id=npc_id;
            competing.tick=at+1;
            competing.sequence=at+1;
            competing.issuer=0;
            assert(bones_foundation_submit(&f,&competing));
        }
        assert(bones_mod_host_step(host,&input)==BONES_MOD_OK);
        npc=bones_foundation_find(&f,npc_id);
        remote=bones_foundation_find(&f,remote_id);
        assert(npc && remote);
        assert(!overlaps_solid(&f.engine.world,&npc->body));
        assert(remote->last_sequence==0); /* Brain never drives client actors. */
        assert(npc->last_sequence==f.engine.tick);
        if(!open_door && fabsf(npc->body.feet.x)<0.32f)
            assert(npc->body.feet.z<1.63f); /* actual closed door collider */
    }
    npc=bones_foundation_find(&f,npc_id);
    remote=bones_foundation_find(&f,remote_id);
    assert(npc && remote);
    out.x=npc->body.feet.x;
    out.z=npc->body.feet.z;
    out.yaw=npc->body.yaw;
    out.player_seq=f.entities[0].last_sequence;
    out.remote_seq=remote->last_sequence;
    info.size=sizeof(info);
    assert(brain_mod_diagnostics_snapshot(&info));
    out.accepted=info.commands_accepted;
    out.rejected=info.commands_rejected;
    out.recoveries=info.obstacle_recoveries;
    assert(out.accepted+out.rejected==95u);
    assert(out.rejected==reject_first);
    bones_mod_host_destroy(host);
    return out;
}

int main(void) {
    const Trial closed=trial(0,0,0);
    const Trial closed_moving=trial(0,1,0);
    const Trial opened=trial(1,0,0);
    const Trial rejected=trial(0,0,12);
    /* Both closed door scenarios must be physically equivalent despite
       camera/player WASD+Space changing. */
    assert(fabsf(closed.x-closed_moving.x)<0.0005f);
    assert(fabsf(closed.z-closed_moving.z)<0.0005f);
    assert(fabsf(closed.yaw-closed_moving.yaw)<0.0005f);
    assert(closed.player_seq==0 && closed_moving.player_seq==0);
    assert(closed.remote_seq==0 && closed_moving.remote_seq==0);
    assert(closed.z<1.70f); /* wall/door remain blocking at end */
    assert(opened.z>2.3f); /* passes through house's actual open portal */
    assert(rejected.accepted==83 && rejected.rejected==12);
    printf("REAL HOUSE: closed (%.2f, %.2f), open (%.2f, %.2f), recoveries %llu, denied %llu\n",
           closed.x,closed.z,opened.x,opened.z,
           (unsigned long long)closed.recoveries,(unsigned long long)rejected.rejected);
    puts("PASS REAL BONES 0.4.1: solid house, door state, ModKit movement, owner, WASD+jump independence");
    return 0;
}
