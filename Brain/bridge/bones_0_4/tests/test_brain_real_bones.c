/* Integration test for BONES real Foundation+ModKit runtime (not a mock).
   Run ONLY against published, reviewed post-playtest Bones physics changes.
   Test is intentionally kept in Brain; no modifications to Bones core. */
#include "bones/modkit_host.h"
#include "../modules/brain/brain_mod_diagnostics.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int32_t bones_mod_brain_query(uint32_t,BonesModDescriptor *);
static BonesFoundation f;
typedef struct {float x,z;float player_yaw;uint64_t accepted,rejected,recovery;} Run;

static Run run_case(int wall,int obstacle,int rotate_player){
    BonesModHost *host;
    BonesInput input={0};
    BonesVec3 at={0.0f,0.0f,-3.0f};
    BonesCollider c={0};
    BrainModDiagnostics stats={0};
    const BonesEntity *npc;
    uint64_t id;
    uint32_t i;
    Run out;
    assert(bones_foundation_init(&f,UINT64_C(0x144033),UINT64_C(0x501),1u));
    /* Offset the deterministic turn phase so the NPC walks straight into
       a test obstacle for the first several seconds. */
    for(i=0;i<60;i++)assert(bones_foundation_step(&f,NULL));
    if(wall || obstacle) {
        c.id=UINT64_C(70001);
        c.solid=1;
        c.render_box=0;
        c.material=BONES_MAT_STONE;
        c.bounds.min=(BonesVec3){obstacle?-0.8f:-5.0f,0.0f,-1.5f};
        c.bounds.max=(BonesVec3){obstacle?0.8f:5.0f,3.0f,-1.3f};
        assert(bones_world_add_collider(&f.engine.world,&c));
    }
    assert(bones_foundation_spawn(&f,BONES_KIND_NPC,0,at,&id));
    assert(bones_foundation_find(&f,id)!=NULL);
    host=bones_mod_host_create(&f);
    assert(host);
    assert(bones_mod_host_add(host,bones_mod_brain_query)==BONES_MOD_OK);
    assert(bones_mod_host_start(host)==BONES_MOD_OK);
    for(i=0;i<65;i++){
        input.yaw_right=(uint8_t)(rotate_player!=0);
        /* Player control intentionally differs across otherwise identical
           simulations. It must not modify NPC commands or their results. */
        assert(bones_mod_host_step(host,&input)==BONES_MOD_OK);
        npc=bones_foundation_find(&f,id);assert(npc);
        if(wall){
            /* Solid wall spans full route; center cannot cross into it. */
            assert(npc->body.feet.z < -1.3f);
        }
        if(obstacle){
            float radius=npc->body.radius;
            const float x=npc->body.feet.x,z=npc->body.feet.z;
            /* A capsule moving around a pillar may pass on either side,
               but may never occupy its solid rectangle with its radius. */
            assert(!(x > c.bounds.min.x-radius+0.001f &&
                     x < c.bounds.max.x+radius-0.001f &&
                     z > c.bounds.min.z-radius+0.001f &&
                     z < c.bounds.max.z+radius-0.001f));
        }
    }
    npc=bones_foundation_find(&f,id);assert(npc);
    out.x=npc->body.feet.x;out.z=npc->body.feet.z;
    out.player_yaw=f.engine.player.yaw;
    stats.size=sizeof(stats);
    assert(brain_mod_diagnostics_snapshot(&stats));
    out.accepted=stats.commands_accepted;
    out.rejected=stats.commands_rejected;
    out.recovery=stats.obstacle_recoveries;
    assert(out.accepted>0);
    assert(out.rejected==0);
    bones_mod_host_destroy(host);
    return out;
}
int main(void){
    Run clear=run_case(0,0,0);
    Run rotated=run_case(0,0,1);
    Run blocked=run_case(1,0,0);
    Run pillar=run_case(0,1,0);
    assert(hypotf(clear.x,clear.z+3.0f)>.20f);
    assert(fabsf(rotated.player_yaw-clear.player_yaw)>.01f);
    assert(fabsf(clear.x-rotated.x)<.0005f);
    assert(fabsf(clear.z-rotated.z)<.0005f);
    printf("REAL BONES: open npc %.3f %.3f / rotated %.3f %.3f\n",
           clear.x,clear.z,rotated.x,rotated.z);
    printf("REAL BONES: wall npc %.3f %.3f / pillar npc %.3f %.3f\n",
           blocked.x,blocked.z,pillar.x,pillar.z);
    printf("REAL BONES: accepted=%llu rejected=%llu recovery=%llu (wall)\n",
           (unsigned long long)blocked.accepted,
           (unsigned long long)blocked.rejected,
           (unsigned long long)blocked.recovery);
    puts("PASS REAL BONES: patrol, collision barriers, player orientation independence");
    return 0;
}
