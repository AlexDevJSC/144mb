/* Actual ModKit Brain/Ear against Foundation and the physical demo house.
   No mock actor, physics or command handler. Windows audio hardware optional. */
#include "../include/bones/modkit_host.h"
#include "../modules/brain/brain_mod_diagnostics.h"
#include "../modules/ear/ear_audio_control.h"
#include "../src/demo/test_house.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static int collision_free(const BonesWorld *world,const BonesPlayer *actor) {
    uint32_t i;
    for(i=0;i<world->collider_count;i++) {
        const BonesCollider *c=&world->colliders[i];
        float x=actor->feet.x, z=actor->feet.z,dx,dz;
        if(!c->solid||actor->feet.y>=c->bounds.max.y ||
           actor->feet.y+actor->height<=c->bounds.min.y) continue;
        if(x<c->bounds.min.x)x=c->bounds.min.x;
        if(x>c->bounds.max.x)x=c->bounds.max.x;
        if(z<c->bounds.min.z)z=c->bounds.min.z;
        if(z>c->bounds.max.z)z=c->bounds.max.z;
        dx=actor->feet.x-x;dz=actor->feet.z-z;
        if(dx*dx+dz*dz < actor->radius*actor->radius-.001f)return 0;
    }
    return 1;
}
int main(void) {
    BonesFoundation f;
    BonesModHost *h;
    BonesModule house;
    BonesInput in={0};
    BonesVec3 at={1.35f,0,0};
    uint64_t id=0;
    BonesVec3 first,last;
    BrainModDiagnostics brain={0};
    EarAudioStatus audio={0};
    int k;
    assert(bones_foundation_init(&f,0x1440001u,0x01442026u,1));
    assert(bones_house_register_colliders(&f.engine.world));
    assert(bones_foundation_spawn(&f,BONES_KIND_NPC,0,at,&id));
    first=bones_foundation_find(&f,id)->body.feet;
    h=bones_mod_host_create(&f);assert(h);
    house=bones_test_house_module();assert(bones_register_module(&f.engine,&house));
    assert(bones_mod_register_builtins(h)==BONES_MOD_OK);
    assert(bones_mod_host_start(h)==BONES_MOD_OK);
    audio.size=sizeof(audio);
    assert(ear_audio_get_status(&audio));
    assert(audio.module_running);
    for(k=0;k<240;k++) {
        const BonesEntity *npc;
        in.jump=(uint8_t)(k==0);
        assert(bones_mod_host_step(h,&in)==BONES_MOD_OK);
        npc=bones_foundation_find(&f,id);assert(npc);
        assert(npc->owner==0 && npc->kind==BONES_KIND_NPC);
        assert(collision_free(&f.engine.world,&npc->body));
        ear_audio_pump();
    }
    last=bones_foundation_find(&f,id)->body.feet;
    brain.size=sizeof(brain);
    assert(brain_mod_diagnostics_snapshot(&brain));
    assert(brain.commands_accepted>100);
    assert(brain.commands_rejected==0);
    assert(fabsf(last.x-first.x)+fabsf(last.z-first.z)>.02f);
    audio.size=sizeof(audio);
    assert(ear_audio_get_status(&audio));
    /* A jump and landing were emitted by Bones to the real Ear module. */
    assert(audio.events_played>=2);
    printf("PASS Brain NPC id=%llu accepted=%llu rejected=%llu x=%.3f z=%.3f\n",
           (unsigned long long)id,(unsigned long long)brain.commands_accepted,
           (unsigned long long)brain.commands_rejected,last.x,last.z);
    printf("PASS Ear events=%u device=%u error=%u\n",
           (unsigned)audio.events_played,(unsigned)audio.device_ready,(unsigned)audio.error);
    bones_mod_host_destroy(h);
    return 0;
}
