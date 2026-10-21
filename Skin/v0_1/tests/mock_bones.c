/* TEST FIXTURE ONLY: exposes the Bones v0.1 public contract.
   Do not ship this implementation as the real Bones engine! */
#include "bones/bones.h"
#include <string.h>
void bones_init(BonesEngine *e,uint64_t seed) {
    memset(e,0,sizeof(*e));e->world.seed=seed;e->world.generator_version=1;
    e->player.feet.z=-7.0f;e->player.radius=.32f;
    e->player.height=1.80f;e->player.eye_height=1.65f;e->player.grounded=true;
}
bool bones_register_module(BonesEngine *e,const BonesModule *m) {
    if(!e||!m||m->version!=BONES_API_VERSION||e->module_count>=BONES_MAX_MODULES||e->tick)return false;
    e->modules[e->module_count++]=*m;return true;
}
bool bones_step(BonesEngine *e,const BonesInput *in,float dt) {
    uint32_t i;
    if(!e||!in||dt!=BONES_TICK_DT)return false;
    if(in->forward)e->player.feet.z+=3.0f*dt;
    if(in->backward)e->player.feet.z-=3.0f*dt;
    if(in->left)e->player.feet.x-=3.0f*dt;
    if(in->right)e->player.feet.x+=3.0f*dt;
    if(in->jump&&!e->player.jump_held&&e->player.grounded){e->player.velocity.y=5.0f;e->player.grounded=false;}
    e->player.jump_held=in->jump!=0;
    e->player.velocity.y-=14.0f*dt;
    e->player.feet.y+=e->player.velocity.y*dt;
    if(e->player.feet.y<=0){e->player.feet.y=0;e->player.grounded=true;e->player.velocity.y=0;}
    e->tick++;
    for(i=0;i<e->module_count;i++)if(e->modules[i].on_tick)e->modules[i].on_tick(e->modules[i].user_data,e);
    return true;
}
bool bones_world_add_collider(BonesWorld *w,const BonesCollider *c) {
    uint32_t i;
    if(!w||!c||!c->id||w->collider_count>=BONES_MAX_COLLIDERS)return false;
    for(i=0;i<w->collider_count;i++)if(w->colliders[i].id==c->id)return false;
    w->colliders[w->collider_count++]=*c;return true;
}
bool bones_world_remove_collider(BonesWorld *w,uint64_t id) {
    uint32_t i,j;
    if(!w||!id)return false;
    for(i=0;i<w->collider_count;i++)if(w->colliders[i].id==id){
      for(j=i+1;j<w->collider_count;j++)w->colliders[j-1]=w->colliders[j];
      w->collider_count--;
      return true;
    }
    return false;
}
