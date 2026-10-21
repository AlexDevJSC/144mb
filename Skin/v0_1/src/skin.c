#include "skin/skin.h"
#include <string.h>
#include <math.h>

static bool player_finite(const BonesPlayer *p) {
    const float f[]={p->feet.x,p->feet.y,p->feet.z,p->velocity.x,p->velocity.y,p->velocity.z,
      p->yaw,p->pitch,p->radius,p->height,p->eye_height};
    unsigned i;
    for(i=0;i<sizeof(f)/sizeof(f[0]);i++) if(!isfinite(f[i])) return false;
    return p->radius>0.0f && p->radius<10.0f && p->height>0.0f && p->height<10.0f &&
           p->eye_height>0.0f && p->eye_height<=p->height;
}
static bool has_id(const uint64_t *ids,uint32_t count,uint64_t id) {
    uint32_t i;
    for(i=0;i<count;i++) if(ids[i]==id) return true;
    return false;
}
static bool append(Skin *s,const BonesEngine *e,SkinEventKind kind,uint64_t id,uint64_t argument) {
    SkinEvent *event;
    if(s->event_count==SKIN_MAX_EVENTS || s->next_event_sequence==UINT64_MAX) {
        s->history_truncated=true; s->dirty=true; return false;
    }
    event=&s->events[s->event_count++];
    event->sequence=s->next_event_sequence++;
    event->tick=e->tick;
    event->kind=(uint32_t)kind;
    event->entity_id=id;
    event->argument=argument;
    s->dirty=true;
    return true;
}
static void refresh_tracking(Skin *s,const BonesEngine *e) {
    uint32_t i;
    s->last_player=e->player;
    s->known_count=e->world.collider_count;
    for(i=0;i<s->known_count;i++) {
        s->known_colliders[i]=e->world.colliders[i].id;
        s->observed_colliders[i]=e->world.colliders[i];
    }
    s->observed=true;
}
bool skin_init(Skin *s,const BonesEngine *e,SkinWorldId id,uint64_t branch_id) {
    if(!s||!e||(!id.hi&&!id.lo)||!branch_id||e->world.collider_count>BONES_MAX_COLLIDERS||
       !player_finite(&e->player)) return false;
    memset(s,0,sizeof(*s));
    s->world_id=id; s->branch_id=branch_id; s->next_event_sequence=1;
    s->expected_seed=e->world.seed; s->expected_generator_version=e->world.generator_version;
    refresh_tracking(s,e);
    return true;
}
void skin_observe(Skin *s,const BonesEngine *e) {
    uint32_t i;
    if(!s||!e||!s->observed||e->world.collider_count>BONES_MAX_COLLIDERS||!player_finite(&e->player)) return;
    if(e->world.seed!=s->expected_seed||e->world.generator_version!=s->expected_generator_version) {
        s->history_truncated=true;return;
    }
    /* Confirmed world lifecycle changes are logged independently of draw calls. */
    for(i=0;i<s->known_count;i++) {
        if(!has_id(s->known_colliders,e->world.collider_count,s->known_colliders[i]))
            (void)append(s,e,SKIN_EVT_COLLIDER_REMOVED,s->known_colliders[i],0);
    }
    for(i=0;i<e->world.collider_count;i++) {
        const BonesCollider *cur=&e->world.colliders[i];
        uint32_t k;
        if(!has_id(s->known_colliders,s->known_count,cur->id)) {
            (void)append(s,e,SKIN_EVT_COLLIDER_ADDED,cur->id,0);
            continue;
        }
        for(k=0;k<s->known_count;k++) if(s->known_colliders[k]==cur->id) {
            const BonesCollider *old=&s->observed_colliders[k];
            /* Compare semantic fields, not C struct padding bytes. */
            if(old->bounds.min.x!=cur->bounds.min.x || old->bounds.min.y!=cur->bounds.min.y ||
               old->bounds.min.z!=cur->bounds.min.z || old->bounds.max.x!=cur->bounds.max.x ||
               old->bounds.max.y!=cur->bounds.max.y || old->bounds.max.z!=cur->bounds.max.z ||
               old->solid!=cur->solid || old->render_box!=cur->render_box || old->material!=cur->material)
               (void)append(s,e,SKIN_EVT_COLLIDER_UPDATED,cur->id,0);
            break;
        }
    }
    if(e->player.grounded!=s->last_player.grounded)
        (void)append(s,e,SKIN_EVT_GROUND_CHANGED,0,(uint64_t)e->player.grounded);
    /* Player movements are sampled every 30 ticks. NOT semantic merge events. */
    if(e->tick%30u==0u &&
      (fabsf(e->player.feet.x-s->last_player.feet.x)>.005f ||
       fabsf(e->player.feet.y-s->last_player.feet.y)>.005f ||
       fabsf(e->player.feet.z-s->last_player.feet.z)>.005f ||
       fabsf(e->player.yaw-s->last_player.yaw)>.001f))
        (void)append(s,e,SKIN_EVT_PLAYER_SAMPLE,0,0);
    /* Update last sample ONLY on sampled ticks, to prevent per-tick drift. */
    if(e->tick%30u==0u) s->last_player=e->player;
    else s->last_player.grounded=e->player.grounded;
    s->known_count=e->world.collider_count;
    for(i=0;i<s->known_count;i++) {
        s->known_colliders[i]=e->world.colliders[i].id;
        s->observed_colliders[i]=e->world.colliders[i];
    }
}
bool skin_record_confirmed(Skin *s,const BonesEngine *e,uint64_t id,uint64_t action) {
    if(!s||!e||!id||!action||!s->observed||e->world.seed!=s->expected_seed||
        e->world.generator_version!=s->expected_generator_version) return false;
    return append(s,e,SKIN_EVT_CONFIRMED_ACTION,id,action);
}
static void tick_callback(void *p,BonesEngine *engine) {skin_observe((Skin*)p,engine);}
bool skin_attach(Skin *s,BonesEngine *e) {
    BonesModule module={0};
    if(!s||!e||!s->observed) return false;
    module.version=BONES_API_VERSION;
    module.user_data=s;
    module.on_tick=tick_callback;
    return bones_register_module(e,&module);
}
