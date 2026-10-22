#include "interaction_core.h"
#include <math.h>
#include <string.h>
#include <float.h>

static int finite3(BonesVec3 a) { return isfinite(a.x)&&isfinite(a.y)&&isfinite(a.z); }
static int validbox(BonesAabb a) {
    return finite3(a.min)&&finite3(a.max)&&a.max.x>a.min.x&&a.max.y>a.min.y&&a.max.z>a.min.z;
}
bool bones_interaction_add(BonesInteractionRegistry *r,const BonesInteractionEntry *e) {
    uint32_t i;
    if(!r||!e||!e->id||!e->kind||!validbox(e->bounds)||
       (e->flags&~BONES_INTERACTION_REQUIRE_COLLIDER)||r->count>=BONES_INTERACTION_CAPACITY)return false;
    for(i=0;i<r->count;i++)if(r->entries[i].id==e->id)return false;
    r->entries[r->count++]=*e;return true;
}
/* Slab ray/AABB intersection; returns nonnegative entry distance or FLT_MAX.
   Camera ray direction must have unit length. */
static float ray_box(BonesVec3 origin,BonesVec3 dir,BonesAabb box,float limit) {
    float mn=0.f,mx=limit;
    const float originv[3]={origin.x,origin.y,origin.z};
    const float dirv[3]={dir.x,dir.y,dir.z};
    const float bmin[3]={box.min.x,box.min.y,box.min.z};
    const float bmax[3]={box.max.x,box.max.y,box.max.z};
    int k;
    for(k=0;k<3;k++) {
        float lo,hi,tmp;
        if(fabsf(dirv[k])<0.000001f) {
            if(originv[k]<bmin[k]||originv[k]>bmax[k])return FLT_MAX;
        } else {
            lo=(bmin[k]-originv[k])/dirv[k];hi=(bmax[k]-originv[k])/dirv[k];
            if(lo>hi) {tmp=lo;lo=hi;hi=tmp;}
            if(lo>mn)mn=lo;
            if(hi<mx)mx=hi;
            if(mx<mn)return FLT_MAX;
        }
    }
    return mn;
}
static int collider_present(const BonesWorld *w,uint64_t id) {
    uint32_t i;
    for(i=0;i<w->collider_count;i++)if(w->colliders[i].id==id&&w->colliders[i].solid)return 1;
    return 0;
}
bool bones_interaction_select(const BonesWorld *world,const BonesInteractionRegistry *r,
                              const BonesCamera *camera,float max_distance,BonesInteractionHit *out) {
    BonesVec3 dir;uint32_t i,j;float best=FLT_MAX;
    uint64_t best_id=0;uint32_t best_kind=0;
    if(!world||!r||!camera||!out||!finite3(camera->position)||
       !isfinite(camera->yaw)||!isfinite(camera->pitch)||
       !isfinite(max_distance)||max_distance<=0||max_distance>100.f)return false;
    /* Invert Eye's world_to_view transform: center ray is +camera Z. */
    dir.x=sinf(camera->yaw)*cosf(camera->pitch);
    dir.y=sinf(camera->pitch);
    dir.z=cosf(camera->yaw)*cosf(camera->pitch);
    if(!finite3(dir))return false;
    for(i=0;i<r->count;i++) {
        const BonesInteractionEntry *entry=&r->entries[i];
        float distance=ray_box(camera->position,dir,entry->bounds,max_distance);
        if(distance==FLT_MAX)continue;
        if((entry->flags&BONES_INTERACTION_REQUIRE_COLLIDER)&&!collider_present(world,entry->id))continue;
        if(distance>best+0.00001f || (fabsf(distance-best)<0.00001f && entry->id>=best_id))continue;
        for(j=0;j<world->collider_count;j++) {
            const BonesCollider *c=&world->colliders[j];
            float block;
            if(!c->solid||c->id==entry->id)continue;
            block=ray_box(camera->position,dir,c->bounds,max_distance);
            if(block<distance-.0001f)break;
        }
        if(j<world->collider_count)continue;
        best=distance;best_id=entry->id;best_kind=entry->kind;
    }
    if(!best_id)return false;
    out->id=best_id;out->kind=best_kind;out->distance=best;
    out->point.x=camera->position.x+dir.x*best;
    out->point.y=camera->position.y+dir.y*best;
    out->point.z=camera->position.z+dir.z*best;
    return true;
}
