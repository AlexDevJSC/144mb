/* Playtest regression: input basis, jump with zero horizontal drift, demo
   architecture and door passability. Pure native C; no Windows device needed. */
#include "../src/bones/bones_internal.h"
#include "../src/demo/test_house.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static float length2(float a,float b) {return sqrtf(a*a+b*b);}
static float near(float a,float b) {return fabsf(a-b);}
static BonesWorld empty_world;
static BonesPlayer start_actor(float yaw) {
    BonesPlayer p={0};p.yaw=yaw;p.radius=.32f;p.height=1.8f;
    p.eye_height=1.65f;p.grounded=true;
    return p;
}
static void simulate(const BonesWorld *world,BonesPlayer *actor,BonesInput input,int ticks) {
    int i;
    for(i=0;i<ticks;i++)bones_physics_step_actor(world,actor,&input,BONES_TICK_DT);
}
static void basis(float yaw,float fx,float fz,float rx,float rz) {
    BonesPlayer p=start_actor(yaw);
    BonesInput input={0};input.forward=1;
    simulate(&empty_world,&p,input,24);
    assert(near(p.velocity.x/3.0f,fx)<.01f && near(p.velocity.z/3.0f,fz)<.01f);
    assert(near(p.yaw,yaw)<.0001f || near(remainderf(p.yaw-yaw,6.2831853f),0)<.0001f);
    p=start_actor(yaw);input.forward=0;input.backward=1;
    simulate(&empty_world,&p,input,24);
    assert(near(p.velocity.x/3.0f,-fx)<.01f && near(p.velocity.z/3.0f,-fz)<.01f);
    p=start_actor(yaw);input.backward=0;input.right=1;
    simulate(&empty_world,&p,input,24);
    assert(near(p.velocity.x/3.0f,rx)<.01f && near(p.velocity.z/3.0f,rz)<.01f);
    p=start_actor(yaw);input.right=0;input.left=1;
    simulate(&empty_world,&p,input,24);
    assert(near(p.velocity.x/3.0f,-rx)<.01f && near(p.velocity.z/3.0f,-rz)<.01f);
    p=start_actor(yaw);input.left=0;input.forward=1;input.right=1;
    simulate(&empty_world,&p,input,24);
    assert(near(length2(p.velocity.x,p.velocity.z),3.0f)<.01f);
}
static void test_axes_and_jump(void) {
    BonesPlayer a;BonesInput in={0};
    basis(0.0f,0,1,1,0);
    basis(1.57079632679f,1,0,0,-1);
    basis(3.14159265359f,0,-1,-1,0);
    basis(4.71238898038f,-1,0,0,1);
    a=start_actor(1.2f);
    in.jump=1;
    simulate(&empty_world,&a,in,15);
    assert(a.feet.y>0 && near(a.feet.x,0)<.000001f && near(a.feet.z,0)<.000001f);
    assert(near(a.yaw,1.2f)<.0001f && near(a.pitch,0)<.000001f);
    assert(near(a.velocity.x,0)<.000001f && near(a.velocity.z,0)<.000001f);
    /* Air movement is deliberately retained when WASD is held. */
    a=start_actor(0);memset(&in,0,sizeof(in));in.forward=1;in.jump=1;
    simulate(&empty_world,&a,in,10);
    assert(a.feet.z>.05f && a.feet.y>0);
    /* Keyboard and mouse must both influence yaw in the same physical path. */
    a=start_actor(0);memset(&in,0,sizeof(in));in.yaw_right=1;in.mouse_dx=5;
    bones_physics_step_actor(&empty_world,&a,&in,BONES_TICK_DT);
    assert(a.yaw>0.03f);assert(a.pitch==0);
    printf("PASS BUG-002/003/004: WASD all quadrants, diagonal, jump, yaw\n");
}
static void test_wall_portal(void) {
    BonesWorld w;BonesEngine game;BonesInput in={0};BonesScene scene;
    BonesPlayer p;uint32_t closed_count;
    bones_init(&game,1337);
    w=game.world;
    assert(w.collider_count==7);
    assert(bones_house_register_colliders(&w));
    assert(w.collider_count==7+BONES_HOUSE_COLLIDERS);
    assert(!bones_house_register_colliders(&w)); /* can't insert duplicate layout */
    p=start_actor(0);in.forward=1;
    simulate(&w,&p,in,120);assert(p.feet.z < 1.64f && p.feet.z>1.0f);
    /* Lintel stops a player when positioned above door height, even if he
       starts at this test height; the full collider count remains coherent. */
    p=start_actor(0);p.feet=(BonesVec3){1.5f,0,0};
    simulate(&w,&p,in,120);assert(p.feet.z<1.64f);
    p=start_actor(0);p.feet=(BonesVec3){0,0,4.0f};
    simulate(&w,&p,in,120);assert(p.feet.z<5.64f && p.feet.z>4.0f);
    p=start_actor(1.57079632679f);p.feet=(BonesVec3){0,0,4};
    simulate(&w,&p,in,120);assert(p.feet.x<1.64f);
    p=start_actor(-1.57079632679f);p.feet=(BonesVec3){0,0,4};
    simulate(&w,&p,in,120);assert(p.feet.x> -1.64f);
    /* Demo door is structurally removable without damaging walls. */
    assert(bones_house_open_door(&w));
    assert(!bones_house_open_door(&w));
    p=start_actor(0);memset(&in,0,sizeof(in));in.forward=1;
    simulate(&w,&p,in,120);assert(p.feet.z>2.6f);
    memset(&scene,0,sizeof(scene));
    assert(bones_house_append_world_scene(&scene,&w));
    assert(scene.quad_count==BONES_HOUSE_QUADS-1);
    closed_count=scene.quad_count;
    assert(closed_count==11);
    /* The normal demo scene with door closed has 12 quads. */
    memset(&scene,0,sizeof(scene));
    assert(bones_house_append_scene(&scene));assert(scene.quad_count==12);
    puts("PASS BUG-001: house walls, closed and open passage, render follows collider");
}
int main(void) {test_axes_and_jump();test_wall_portal();puts("PASS playtest phase 1");return 0;}
