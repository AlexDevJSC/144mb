#include "../include/bones/foundation.h"
#include "../src/adapters/eye_adapter.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static BonesFoundation f, other;
static BonesEye eye;
static uint8_t bytes[BONES_FOUNDATION_SNAPSHOT_MAX];
static void fresh(void) {
    assert(bones_foundation_init(&f,UINT64_C(0x101234),1234,1));
    assert(f.entity_count==1 && f.world_id!=f.engine.world.seed);
    assert(bones_foundation_find(&f,1)!=NULL);
    assert(bones_foundation_find(&f,1000)==NULL);
}
static void test_entities_and_authority(void) {
    uint64_t id=0;
    BonesCommand c={0};
    BonesVec3 feet={3.0f,0.0f,-7.0f};
    uint64_t first, after;
    fresh();
    assert(bones_foundation_spawn(&f,BONES_KIND_PLAYER,2,feet,&id));
    assert(id==(UINT64_C(1)<<32|2u) && f.entity_count==2);
    c.actor_id=id;c.tick=1;c.sequence=1;c.issuer=2;c.forward=1;
    assert(!bones_foundation_submit(&f,&(BonesCommand){.actor_id=2,.tick=1,.sequence=1,.issuer=3,.forward=1}));
    assert(!bones_foundation_submit(&f,&(BonesCommand){.actor_id=2,.tick=1,.sequence=1,.issuer=2,.forward=7}));
    assert(bones_foundation_submit(&f,&c));
    assert(!bones_foundation_submit(&f,&c));
    assert(bones_foundation_step(&f,NULL));
    assert(f.engine.tick==1 && f.entity_count==2);
    assert(bones_foundation_find(&f,id)->body.feet.z>feet.z);
    assert(!bones_foundation_submit(&f,&c)); /* stale tick */
    c.tick=2;assert(!bones_foundation_submit(&f,&c)); /* replay */
    c.sequence=2;assert(bones_foundation_submit(&f,&c));
    assert(bones_foundation_step(&f,NULL));
    assert(f.entities[0].body.feet.x==f.engine.player.feet.x);
    first=bones_foundation_hash(&f);
    assert(first!=0);
    after=f.entities[1].body.feet.z >feet.z;
    assert(after);
    assert(!bones_foundation_remove(&f,1));
    assert(bones_foundation_remove(&f,id));
    assert(f.entity_count==1);
    puts("PASS entidades, autorizacion, tick, repeticion y fisica compartida");
}
static void test_snapshot_and_recovery(void) {
    uint64_t id,initial,hash;
    size_t n=0;
    BonesFact facts[4];
    fresh();
    assert(bones_foundation_spawn(&f,BONES_KIND_NPC,0,(BonesVec3){1,0,1},&id));
    assert(bones_world_remove_collider(&f.engine.world,f.engine.world.colliders[0].id));
    assert(bones_foundation_step(&f,NULL));
    hash=bones_foundation_hash(&f);
    assert(bones_foundation_export(&f,bytes,sizeof(bytes),&n));
    assert(n>120 && n<sizeof(bytes));
    assert(bones_foundation_init(&other,UINT64_C(0x101234),1234,1));
    assert(bones_foundation_restore(&other,bytes,n));
    assert(other.entity_count==2 && other.fact_count==1 && other.engine.tick==1);
    assert(other.engine.world.collider_count==f.engine.world.collider_count);
    assert(bones_foundation_hash(&other)==hash);
    initial=bones_foundation_hash(&other);
    bytes[100]^=0x11;
    assert(!bones_foundation_restore(&other,bytes,n));
    assert(initial==bones_foundation_hash(&other));
    bytes[100]^=0x11;
    other.world_id=987;
    assert(!bones_foundation_restore(&other,bytes,n));
    other.world_id=f.world_id;
    other.branch_id=2;
    assert(!bones_foundation_restore(&other,bytes,n));
    other.branch_id=f.branch_id;
    assert(bones_foundation_read_facts(&other,0,facts,4)==1);
    assert(facts[0].kind==BONES_FACT_SPAWN && facts[0].subject==id);
    assert(bones_foundation_ack_facts(&other,facts[0].sequence));
    assert(!other.fact_count);
    puts("PASS snapshot little endian/CRC, importacion atomica, world_id y journal");
}
static void test_branch_namespace(void) {
    uint64_t a,b;
    fresh();
    assert(bones_foundation_init(&other,UINT64_C(0x101234),1234,2));
    assert(bones_foundation_spawn(&f,BONES_KIND_NPC,0,(BonesVec3){0,0,1},&a));
    assert(bones_foundation_spawn(&other,BONES_KIND_NPC,0,(BonesVec3){0,0,1},&b));
    assert(a!=b && (a>>32)==1 && (b>>32)==2);
    puts("PASS entidades de ramas divergentes no reutilizan IDs");
}
static void test_independent_replay(void) {
    BonesCommand c={0};
    uint64_t remote;
    unsigned i;
    uint64_t h;
    fresh();
    assert(bones_foundation_spawn(&f,BONES_KIND_PLAYER,2,(BonesVec3){1,0,0},&remote));
    for(i=0;i<35;i++) {
        c.actor_id=remote;c.tick=f.engine.tick+1;c.sequence=i+1;c.issuer=2;
        c.forward=1;c.strafe=(i&1)?1:0;c.jump=(uint8_t)(i==4);
        assert(bones_foundation_submit(&f,&c));
        assert(bones_foundation_step(&f,NULL));
    }
    h=bones_foundation_hash(&f);
    assert(bones_foundation_init(&other,UINT64_C(0x101234),1234,1));
    assert(bones_foundation_spawn(&other,BONES_KIND_PLAYER,2,(BonesVec3){1,0,0},&remote));
    for(i=0;i<35;i++) {
        c.actor_id=remote;c.tick=other.engine.tick+1;c.sequence=i+1;c.issuer=2;
        c.forward=1;c.strafe=(i&1)?1:0;c.jump=(uint8_t)(i==4);
        assert(bones_foundation_submit(&other,&c));
        assert(bones_foundation_step(&other,NULL));
    }
    assert(h==bones_foundation_hash(&other));
    puts("PASS replay reproducible dentro de una misma compilacion");
}
static void test_static_collision(void) {
    uint64_t npc;
    BonesCommand c={0};
    unsigned i;
    fresh();
    assert(bones_foundation_spawn(&f,BONES_KIND_NPC,0,(BonesVec3){7,0,0},&npc));
    for(i=0;i<180;i++) {
        c.actor_id=npc;c.issuer=0;c.tick=f.engine.tick+1;
        c.sequence=i+1;c.strafe=1;
        assert(bones_foundation_submit(&f,&c));
        assert(bones_foundation_step(&f,NULL));
    }
    assert(bones_foundation_find(&f,npc)->body.feet.x < 7.75f);
    puts("PASS entidades secundarias colisionan con geometria central");
}
typedef struct { BonesFoundation *f; uint64_t actor; unsigned sent; unsigned rejected; } TickProbe;
static void probe_before(void *ctx,BonesEngine *engine,const BonesInput *in) {
    TickProbe *p=(TickProbe*)ctx;
    BonesCommand c={0};
    (void)in;
    c.actor_id=p->actor;c.issuer=0;c.tick=engine->tick+1;
    c.sequence=1000;c.forward=1;
    if(!bones_foundation_submit(p->f,&c)) p->rejected++;
}
static void probe_after(void *ctx,BonesEngine *engine) {
    TickProbe *p=(TickProbe*)ctx;
    BonesCommand c={0};
    if(p->sent) return;
    c.actor_id=p->actor;c.issuer=0;c.tick=engine->tick+1;
    c.sequence=1;c.forward=1;
    assert(bones_foundation_submit(p->f,&c));
    p->sent++;
}
static void test_callback_scheduling(void) {
    uint64_t actor;
    TickProbe probe={0};
    BonesSubsystem mod={0};
    fresh();
    assert(bones_foundation_spawn(&f,BONES_KIND_NPC,0,(BonesVec3){0,0,0},&actor));
    probe.f=&f;probe.actor=actor;
    mod.version=BONES_SUBSYSTEM_API_VERSION;mod.struct_size=sizeof(mod);
    mod.slot=BONES_SLOT_BRAIN;mod.context=&probe;
    mod.before_tick=probe_before;mod.after_tick=probe_after;
    assert(bones_attach_subsystem(&f.engine,&mod));
    assert(bones_foundation_step(&f,NULL));
    assert(probe.sent==1 && probe.rejected==1 && f.command_count==1);
    assert(bones_foundation_find(&f,actor)->last_sequence==0);
    assert(bones_foundation_step(&f,NULL));
    assert(f.command_count==0 && probe.rejected==2);
    assert(bones_foundation_find(&f,actor)->last_sequence==1);
    bones_stop(&f.engine);
    puts("PASS callbacks before_tick rechazan orden actual; after_tick agenda proximo tick");
}
static void test_modular_registry(void) {
    fresh();
    bones_eye_init(&eye,123);
    assert(bones_register_project_modules(&f,&eye));
    assert(f.engine.subsystem_count==5);
    assert(bones_start(&f.engine));
    assert(bones_foundation_render_frame(&f));
    {
        uint64_t id;
        BonesScene scene;
        assert(bones_foundation_spawn(&f,BONES_KIND_NPC,0,(BonesVec3){1,0,2},&id));
        assert(bones_foundation_make_scene(&f,&scene));
        assert(scene.sprite_count>=2);
    }
    assert(bones_foundation_step(&f,NULL));
    assert(bones_foundation_render_frame(&f));
    bones_stop(&f.engine);
    puts("PASS registro automatico cinco carpetas, Eye real y fisica");
}
int main(void) {
    test_entities_and_authority();
    test_snapshot_and_recovery();
    test_branch_namespace();
    test_independent_replay();
    test_static_collision();
    test_callback_scheduling();
    test_modular_registry();
    return 0;
}
