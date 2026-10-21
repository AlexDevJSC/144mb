#include "../include/bones/bones.h"
#include "../src/adapters/eye_adapter.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static BonesEngine a, b;
static BonesEye eye;
static int callbacks;
static void module_tick(void *ctx, BonesEngine *e) {
    int *counter=(int*)ctx;
    assert(e->tick>0);
    (*counter)++;
}
static void module_scene(void *ctx, const BonesEngine *e, BonesScene *scene) {
    BonesSprite s={{1.0f,0.0f,0.0f},1.0f,1.8f,0};
    (void)ctx; (void)e;
    (void)bones_scene_add_sprite(scene,&s); /* 8th plugin hits explicit capacity limit */
}
static void steps(BonesEngine *e, BonesInput *in, int count) {
    int i; for(i=0;i<count;i++) assert(bones_step(e,in,BONES_TICK_DT));
}
static void test_physics(void) {
    BonesInput in={0};
    float d_single, d_diag;
    int i;
    bones_init(&a,1234); in.forward=1; steps(&a,&in,60);
    d_single=sqrtf(a.player.feet.x*a.player.feet.x+(a.player.feet.z+7)*(a.player.feet.z+7));
    bones_init(&b,1234); in.right=1; steps(&b,&in,60);
    d_diag=sqrtf(b.player.feet.x*b.player.feet.x+(b.player.feet.z+7)*(b.player.feet.z+7));
    assert(fabsf(d_single-d_diag)<0.02f);
    assert(d_single<3.01f && d_single>2.7f);
    bones_init(&a,1234); in.right=0; in.forward=1;
    steps(&a,&in,2000);
    /* The NPC collider at z=6.7 blocks us before the north wall. */
    assert(a.player.feet.z<6.39f && a.player.feet.z>6.2f);
    assert(a.player.grounded && a.player.feet.y==0.0f);
    bones_init(&a,1234);
    a.player.feet.x=-3.4f; a.player.feet.z=0.0f;
    steps(&a,&in,500);
    assert(a.player.feet.z>=2.0f && a.player.feet.z<2.25f);
    /* Collision sliding: movement on X while Z is blocked. */
    in.right=1; steps(&a,&in,120);
    assert(a.player.feet.x>-3.0f);
    /* Jump height, gravity, landing, and no autorepeat while key is held. */
    bones_init(&a,1234); memset(&in,0,sizeof(in)); in.jump=1;
    steps(&a,&in,25);
    assert(a.player.feet.y>.5f && a.player.feet.y<1.1f);
    steps(&a,&in,180);
    assert(a.player.feet.y==0.0f && a.player.grounded);
    assert(a.player.velocity.y==0.0f);
    in.jump=0; steps(&a,&in,1); in.jump=1;
    steps(&a,&in,1);
    assert(a.player.feet.y>0.0f);
    in.jump=0;
    steps(&a,&in,100);
    assert(a.player.feet.y==0.0f);
    for(i=0;i<1000;i++) {in.yaw_right=1; steps(&a,&in,1);}
    assert(fabsf(a.player.yaw)<=3.1416f);
}
static void test_determinism(void) {
    BonesInput in={0};
    BonesSectorCoord c0={12,-99}, c1={12,-98};
    uint64_t key=bones_sector_key(123,1,c0,7);
    int j;
    assert(key==bones_sector_key(123,1,c0,7));
    assert(key!=bones_sector_key(123,1,c1,7));
    assert(key!=bones_sector_key(123,2,c0,7));
    assert(key!=bones_sector_key(123,1,c0,8));
    assert(key!=bones_sector_key(124,1,c0,7));
    bones_init(&a,987); bones_init(&b,987);
    assert(a.world.collider_count==7);
    for(j=0;j<7;j++) {
        assert(a.world.colliders[j].id==b.world.colliders[j].id);
        assert(a.world.colliders[j].id!=bones_sector_key(988,1,(BonesSectorCoord){0,0},UINT64_C(0x424f4e45535f524d)));
    }
    for(j=0;j<420;j++) {
        memset(&in,0,sizeof(in));
        in.forward=(j%67)<30; in.right=(j%41)<16;
        in.jump=(j%120)==1; in.sprint=(j%57)<20;
        in.yaw_right=j%3==0;
        in.mouse_dx=(float)((j%5)-2);
        assert(bones_step(&a,&in,BONES_TICK_DT));
        assert(bones_step(&b,&in,BONES_TICK_DT));
        assert(memcmp(&a.player,&b.player,sizeof(a.player))==0);
    }
    assert(a.tick==420 && b.tick==420);
    assert(!bones_step(&a,&in,.02f));
    assert(!bones_step(&a,&in,NAN));
    assert(a.tick==420);
}
static void test_world_extension(void) {
    BonesCollider obj={0};
    BonesInput in={0};
    uint64_t old;
    bones_init(&a,51);
    old=a.world.collider_count;
    obj.id=UINT64_C(0x4348414952);
    obj.bounds.min=(BonesVec3){-.5f,0,-4.0f};
    obj.bounds.max=(BonesVec3){ .5f,1,-3.0f};
    obj.solid=1; obj.render_box=1; obj.material=BONES_MAT_WOOD;
    assert(bones_world_add_collider(&a.world,&obj));
    assert(a.world.collider_count==old+1);
    assert(!bones_world_add_collider(&a.world,&obj));
    in.forward=1; steps(&a,&in,200);
    assert(a.player.feet.z < -4.30f); /* added crate blocks movement */
    assert(bones_world_remove_collider(&a.world,obj.id));
    assert(!bones_world_remove_collider(&a.world,obj.id));
    assert(a.world.collider_count==old);
    steps(&a,&in,120);
    assert(a.player.feet.z > -4.0f);
    obj.bounds.min.x=obj.bounds.max.x;
    assert(!bones_world_add_collider(&a.world,&obj));
}
static void test_extensions(void) {
    BonesInput in={0};
    BonesScene sc;
    BonesModule good={BONES_API_VERSION,&callbacks,module_tick,module_scene};
    BonesModule bad=good;
    int i;
    bones_init(&a,7); callbacks=0;
    bad.version=999;
    assert(!bones_register_module(&a,&bad));
    for(i=0;i<(int)BONES_MAX_MODULES;i++) assert(bones_register_module(&a,&good));
    assert(!bones_register_module(&a,&good));
    assert(bones_step(&a,&in,BONES_TICK_DT));
    assert(callbacks==(int)BONES_MAX_MODULES);
    assert(!bones_register_module(&a,&good));
    bones_make_scene(&a,&sc);
    assert(sc.quad_count==32); /* 2 room planes + 6 visible boxes * 5 */
    assert(sc.sprite_count==BONES_MAX_SPRITES);
    assert(!bones_scene_add_sprite(&sc,&sc.sprites[0]));
    bones_eye_init(&eye,0x01442026u);
    assert(bones_render(&a,&eye.backend));
    assert(eye.renderer.triangles>12);
    {
        BonesRenderBackend bad_backend=eye.backend;
        bad_backend.version=0;
        assert(!bones_render(&a,&bad_backend));
    }
}
static void test_graphics_separation(void) {
    uint64_t h1,h2;
    BonesScene scene;
    bones_init(&a,0x1442026);
    bones_make_scene(&a,&scene);
    assert(scene.camera.position.y==1.65f);
    assert(scene.sprite_count==1);
    bones_eye_init(&eye,0x1442026);
    assert(bones_render(&a,&eye.backend));
    h1=p144_renderer_image_hash(&eye.renderer);
    assert(bones_render(&a,&eye.backend));
    h2=p144_renderer_image_hash(&eye.renderer);
    assert(h1==h2);
    printf("PASS: frame determinista=%016llx, triangulos=%u\n",
        (unsigned long long)h1,(unsigned)eye.renderer.triangles);
}
int main(void) {
    test_physics(); test_determinism(); test_world_extension(); test_extensions(); test_graphics_separation();
    puts("PASS: movimiento, colisiones, salto, gravedad, control de tick, mundo, modulos, adaptador Eye");
    return 0;
}
