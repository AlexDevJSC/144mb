#include "../../include/bones/modkit_host.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#define MAX_TRANSIENT_EVENTS 64u
#define PHASE_IDLE 0
#define PHASE_START 1
#define PHASE_BEFORE 2
#define PHASE_STEP 3
#define PHASE_AFTER 4
#define PHASE_BUILD 5
#define PHASE_PRESENT 6
#define PHASE_EVENTS 7
#define PHASE_STOP 8

typedef struct ModuleEntry ModuleEntry;
struct BonesModHost;
struct ModuleEntry {
    BonesModDescriptor descriptor;
    BonesModServices services;
    struct BonesModHost *host;
    uint32_t started;
    void *library;
};
struct BonesModHost {
    BonesFoundation *foundation;
    ModuleEntry modules[BONES_MOD_LIMIT];
    uint32_t count;
    uint8_t order[BONES_MOD_LIMIT];
    uint32_t started, running, phase;
    int32_t error;
    BonesScene legacy_scene;
    BonesModQuad quads[BONES_MOD_QUAD_LIMIT];
    BonesModSprite sprites[BONES_MOD_SPRITE_LIMIT];
    BonesModEvent events[MAX_TRANSIENT_EVENTS];
    uint32_t event_count;
};
static uint32_t role_allow(uint32_t role) {
    const uint32_t basic = BONES_PERMISSION_READ_ACTORS | BONES_PERMISSION_READ_TIME;
    switch (role) {
        case BONES_ROLE_EYE: return basic | BONES_PERMISSION_WRITE_SCENE;
        case BONES_ROLE_EAR: return basic;
        case BONES_ROLE_BRAIN: return basic | BONES_PERMISSION_SEND_COMMANDS | BONES_PERMISSION_EMIT_EVENTS;
        case BONES_ROLE_SKIN: return basic | BONES_PERMISSION_READ_SNAPSHOT | BONES_PERMISSION_READ_FACTS | BONES_PERMISSION_RESTORE_SNAPSHOT | BONES_PERMISSION_LIST_MODULES;
        case BONES_ROLE_MOUTH: return basic | BONES_PERMISSION_SEND_COMMANDS | BONES_PERMISSION_READ_SNAPSHOT | BONES_PERMISSION_READ_FACTS | BONES_PERMISSION_LIST_MODULES;
        case BONES_ROLE_EXTENSION: return basic;
        default: return 0;
    }
}
static int valid_id(const char *id) {
    uint32_t n;
    if (!id || id[0]<'a' || id[0]>'z') return 0;
    for(n=0;n<BONES_MOD_ID_LEN;n++) {
        unsigned char c=(unsigned char)id[n];
        if(c==0) return n>0;
        if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-')) return 0;
    }
    return 0;
}
static ModuleEntry *entry_from(void *context) { return (ModuleEntry *)context; }
static int has(ModuleEntry *e, uint32_t perm) { return e && e->host && (e->services.permissions & perm)!=0; }
static uint64_t api_tick(void *ctx) {
    ModuleEntry *e=entry_from(ctx);
    return has(e,BONES_PERMISSION_READ_TIME)? e->host->foundation->engine.tick : 0;
}
static int32_t api_actor_count(void *ctx) {
    ModuleEntry *e=entry_from(ctx);
    if(!has(e,BONES_PERMISSION_READ_ACTORS)) return BONES_MOD_DENIED;
    return (int32_t)e->host->foundation->entity_count;
}
static void copy_actor(BonesModActor *out,const BonesEntity *in) {
    memset(out,0,sizeof(*out));out->size=(uint32_t)sizeof(*out);
    out->id=in->id;out->owner=in->owner;out->kind=in->kind;
    out->x=in->body.feet.x;out->y=in->body.feet.y;out->z=in->body.feet.z;
    out->vx=in->body.velocity.x;out->vy=in->body.velocity.y;out->vz=in->body.velocity.z;
    out->yaw=in->body.yaw;out->pitch=in->body.pitch;out->grounded=in->body.grounded?1u:0u;
}
static int32_t api_actor_at(void *ctx,uint32_t index,BonesModActor *out) {
    ModuleEntry *e=entry_from(ctx);
    if(!has(e,BONES_PERMISSION_READ_ACTORS)) return BONES_MOD_DENIED;
    if(!out || out->size!=sizeof(*out)) return BONES_MOD_EINVAL;
    if(index>=e->host->foundation->entity_count) return BONES_MOD_MISSING;
    copy_actor(out,&e->host->foundation->entities[index]);return BONES_MOD_OK;
}
static int32_t api_actor_get(void *ctx,uint64_t id,BonesModActor *out) {
    ModuleEntry *e=entry_from(ctx);const BonesEntity *a;
    if(!has(e,BONES_PERMISSION_READ_ACTORS)) return BONES_MOD_DENIED;
    if(!out || out->size!=sizeof(*out)) return BONES_MOD_EINVAL;
    a=bones_foundation_find(e->host->foundation,id);
    if(!a) return BONES_MOD_MISSING;
    copy_actor(out,a);return BONES_MOD_OK;
}
static int32_t api_command(void *ctx,const BonesModCommand *c) {
    ModuleEntry *e=entry_from(ctx);BonesCommand b;
    if(!has(e,BONES_PERMISSION_SEND_COMMANDS)) return BONES_MOD_DENIED;
    if(!c || c->size!=sizeof(*c)) return BONES_MOD_EINVAL;
    if(e->host->phase!=PHASE_BEFORE && e->host->phase!=PHASE_AFTER) return BONES_MOD_BAD_STATE;
    /* Brain has no authority to impersonate players; Mouth cannot drive NPCs. */
    if((e->descriptor.role==BONES_ROLE_BRAIN && c->issuer!=0) ||
       (e->descriptor.role==BONES_ROLE_MOUTH && c->issuer==0)) return BONES_MOD_DENIED;
    if(c->forward < -1 || c->forward>1 || c->strafe < -1 || c->strafe >1 ||
       c->yaw_milliradians < -300 || c->yaw_milliradians>300 || c->sprint>1 || c->jump>1) return BONES_MOD_EINVAL;
    memset(&b,0,sizeof(b));
    b.actor_id=c->actor_id;b.tick=c->tick;b.sequence=c->sequence;b.issuer=c->issuer;
    b.forward=(int16_t)c->forward;b.strafe=(int16_t)c->strafe;
    b.yaw_milliradians=(int16_t)c->yaw_milliradians;
    b.sprint=(uint8_t)c->sprint;b.jump=(uint8_t)c->jump;
    return bones_foundation_submit(e->host->foundation,&b)?BONES_MOD_OK:BONES_MOD_EINVAL;
}
static int32_t api_snapshot(void *ctx,uint8_t *dst,uint32_t cap,uint32_t *written) {
    ModuleEntry *e=entry_from(ctx);size_t size;
    if(!has(e,BONES_PERMISSION_READ_SNAPSHOT)) return BONES_MOD_DENIED;
    if(!dst||!written||cap>BONES_FOUNDATION_SNAPSHOT_MAX) return BONES_MOD_EINVAL;
    if(e->host->phase!=PHASE_AFTER && e->host->phase!=PHASE_IDLE) return BONES_MOD_BAD_STATE;
    /* An already-submitted command is NOT a committed world change.
       Export a committed-state view without volatile next-tick commands. */
    if(e->host->foundation->command_count) {
        BonesFoundation view=*e->host->foundation;
        view.command_count=0;
        if(!bones_foundation_export(&view,dst,cap,&size)) return BONES_MOD_BAD_STATE;
    } else if(!bones_foundation_export(e->host->foundation,dst,cap,&size)) return BONES_MOD_BAD_STATE;
    *written=(uint32_t)size;return BONES_MOD_OK;
}
/* Skin-only cold start restore. No live rollback, file I/O or merge.
   Done BEFORE other specialist providers start; world_id/branch/version validated by foundation. */
static int32_t api_restore(void *ctx,const uint8_t *src,uint32_t length) {
    ModuleEntry *e=entry_from(ctx);
    if(!has(e,BONES_PERMISSION_RESTORE_SNAPSHOT))return BONES_MOD_DENIED;
    if(e->host->phase!=PHASE_START || e->host->foundation->engine.tick!=0 ||
       e->host->foundation->engine.subsystem_started || !src || length==0 ||
       length>BONES_FOUNDATION_SNAPSHOT_MAX)return BONES_MOD_BAD_STATE;
    return bones_foundation_restore(e->host->foundation,src,length)?BONES_MOD_OK:BONES_MOD_EINVAL;
}
static int32_t api_facts(void *ctx,uint64_t after,BonesModFact *dst,uint32_t capacity) {
    ModuleEntry *e=entry_from(ctx);uint32_t i,n=0;const BonesFoundation *f;
    if(!has(e,BONES_PERMISSION_READ_FACTS)) return BONES_MOD_DENIED;
    if(capacity && !dst) return BONES_MOD_EINVAL;
    f=e->host->foundation;
    for(i=0;i<f->fact_count && n<capacity;i++) if(f->facts[i].sequence>after) {
        if(dst[n].size!=sizeof(*dst)) return BONES_MOD_EINVAL;
        dst[n].sequence=f->facts[i].sequence;dst[n].tick=f->facts[i].tick;
        dst[n].subject=f->facts[i].subject;dst[n].kind=f->facts[i].kind;n++;
    }
    return (int32_t)n;
}
static int32_t api_module_count(void *ctx) {
    ModuleEntry *e=entry_from(ctx);
    if(!has(e,BONES_PERMISSION_LIST_MODULES))return BONES_MOD_DENIED;
    return (int32_t)e->host->count;
}
static int32_t api_module_at(void *ctx,uint32_t index,BonesModInfo *out) {
    ModuleEntry *e=entry_from(ctx);const BonesModDescriptor *d;
    if(!has(e,BONES_PERMISSION_LIST_MODULES))return BONES_MOD_DENIED;
    if(!out||out->size!=sizeof(*out))return BONES_MOD_EINVAL;
    if(index>=e->host->count)return BONES_MOD_MISSING;
    d=&e->host->modules[index].descriptor;
    memset(out,0,sizeof(*out));out->size=sizeof(*out);
    memcpy(out->id,d->id,BONES_MOD_ID_LEN);
    out->role=d->role;out->implementation_version=d->implementation_version;
    out->state_schema_version=d->state_schema_version;
    return BONES_MOD_OK;
}
static int32_t api_event(void *ctx,const BonesModEvent *ev) {
    ModuleEntry *e=entry_from(ctx);BonesModHost *h;
    if(!has(e,BONES_PERMISSION_EMIT_EVENTS)) return BONES_MOD_DENIED;
    if(!ev||ev->size!=sizeof(*ev)||ev->type<BONES_EVT_CUSTOM) return BONES_MOD_EINVAL;
    h=e->host;
    if(h->phase!=PHASE_AFTER && h->phase!=PHASE_BEFORE) return BONES_MOD_BAD_STATE;
    if(h->event_count>=MAX_TRANSIENT_EVENTS) {h->error=BONES_MOD_FULL;return BONES_MOD_FULL;}
    h->events[h->event_count]=*ev;
    h->events[h->event_count].tick=h->foundation->engine.tick;
    h->event_count++;
    return BONES_MOD_OK;
}
static int32_t api_add_quad(void *ctx,const BonesModQuad *quad) {
    ModuleEntry *e=entry_from(ctx);BonesQuad b;uint32_t i;BonesScene *scene;
    if(!has(e,BONES_PERMISSION_WRITE_SCENE)) return BONES_MOD_DENIED;
    if(e->host->phase!=PHASE_BUILD) return BONES_MOD_BAD_STATE;
    if(!quad||quad->size!=sizeof(*quad)||quad->flags||quad->material_id>2) return BONES_MOD_EINVAL;
    for(i=0;i<4;i++) {
        const BonesModVertex *v=&quad->vertices[i];
        if(!isfinite(v->x)||!isfinite(v->y)||!isfinite(v->z)||!isfinite(v->u)||!isfinite(v->v))return BONES_MOD_EINVAL;
        b.vertices[i].position=(BonesVec3){v->x,v->y,v->z};b.vertices[i].u=v->u;b.vertices[i].v=v->v;
    }
    b.material=(uint8_t)quad->material_id;
    scene=&e->host->legacy_scene;
    return bones_scene_add_quad(scene,&b)?BONES_MOD_OK:BONES_MOD_FULL;
}
static int32_t api_add_sprite(void *ctx,const BonesModSprite *s) {
    ModuleEntry *e=entry_from(ctx);BonesSprite b;
    if(!has(e,BONES_PERMISSION_WRITE_SCENE)) return BONES_MOD_DENIED;
    if(e->host->phase!=PHASE_BUILD) return BONES_MOD_BAD_STATE;
    if(!s||s->size!=sizeof(*s)||!isfinite(s->x)||!isfinite(s->y)||!isfinite(s->z)||
       !isfinite(s->width)||!isfinite(s->height)||s->width<=0||s->height<=0) return BONES_MOD_EINVAL;
    b.feet=(BonesVec3){s->x,s->y,s->z};b.width=s->width;b.height=s->height;b.animation_tick=s->animation_tick;
    return bones_scene_add_sprite(&e->host->legacy_scene,&b)?BONES_MOD_OK:BONES_MOD_FULL;
}
static void build_services(ModuleEntry *e) {
    BonesModServices *s=&e->services;
    memset(s,0,sizeof(*s));s->size=(uint32_t)sizeof(*s);s->abi=BONES_MOD_ABI;
    s->permissions=e->descriptor.requested_permissions;
    s->host_context=e;
    s->tick=api_tick;s->actor_count=api_actor_count;s->actor_at=api_actor_at;s->actor_get=api_actor_get;
    s->submit_move=api_command;s->snapshot_export=api_snapshot;s->snapshot_restore_at_start=api_restore;s->facts_since=api_facts;
    s->module_count=api_module_count;s->module_at=api_module_at;
    s->emit_event=api_event;s->scene_add_quad=api_add_quad;s->scene_add_sprite=api_add_sprite;
}
BonesModHost *bones_mod_host_create(BonesFoundation *f) {
    BonesModHost *h;
    if(!f || !f->world_id) return NULL;
    h=(BonesModHost*)calloc(1,sizeof(*h));
    if(h)h->foundation=f;
    return h;
}
uint32_t bones_mod_host_count(const BonesModHost *h){return h?h->count:0;}
int32_t bones_mod_host_error(const BonesModHost *h){return h?h->error:BONES_MOD_EINVAL;}
static int32_t add_descriptor(BonesModHost *h,const BonesModDescriptor *input) {
    BonesModDescriptor d=*input;uint32_t i;
    if(!h||h->running||h->phase||h->count==BONES_MOD_LIMIT)return BONES_MOD_BAD_STATE;
    if(d.size!=sizeof(d) || d.abi!=BONES_MOD_ABI ||
       !valid_id(d.id) || d.role>BONES_ROLE_EXTENSION || !d.implementation_version ||
       (d.required_roles & ~BONES_ROLE_PROVIDER_MASK)!=0 ||
       (d.role!=BONES_ROLE_EXTENSION && (d.required_roles & BONES_ROLE_BIT(d.role))) ||
       (d.requested_permissions & ~role_allow(d.role))!=0 ||
       (d.present && d.role!=BONES_ROLE_EYE) ||
       (d.build_scene && !(d.requested_permissions & BONES_PERMISSION_WRITE_SCENE))) return BONES_MOD_EINVAL;
    for(i=0;i<h->count;i++) {
        if(strcmp(h->modules[i].descriptor.id,d.id)==0)return BONES_MOD_EINVAL;
        if(d.role!=BONES_ROLE_EXTENSION && h->modules[i].descriptor.role==d.role)return BONES_MOD_EINVAL;
    }
    h->modules[h->count].descriptor=d;
    h->modules[h->count].host=h;
    build_services(&h->modules[h->count]);
    h->count++;
    return BONES_MOD_OK;
}
int32_t bones_mod_host_add(BonesModHost *h,BonesModQuery query) {
    BonesModDescriptor d;
    if(!h||!query)return BONES_MOD_EINVAL;
    if(h->running||h->phase||h->count==BONES_MOD_LIMIT)return BONES_MOD_BAD_STATE;
    memset(&d,0,sizeof(d));d.size=(uint32_t)sizeof(d);
    if(query(BONES_MOD_ABI,&d)!=BONES_MOD_OK)return BONES_MOD_EINVAL;
    return add_descriptor(h,&d);
}
static int role_rank(uint32_t r){
    switch(r){case BONES_ROLE_SKIN:return 0;case BONES_ROLE_BRAIN:return 1;
        case BONES_ROLE_EAR:return 2;case BONES_ROLE_MOUTH:return 3;
        case BONES_ROLE_EYE:return 4;default:return 5;}
}
int32_t bones_mod_host_start(BonesModHost *h) {
    uint32_t n,mask=0,scheduled=0,i,selected;int32_t err;
    if(!h)return BONES_MOD_EINVAL;
    if(h->running)return BONES_MOD_OK;
    if(h->phase)return BONES_MOD_BAD_STATE;
    for(i=0;i<h->count;i++) if(h->modules[i].descriptor.role!=BONES_ROLE_EXTENSION)
        mask|=BONES_ROLE_BIT(h->modules[i].descriptor.role);
    for(i=0;i<h->count;i++) if((h->modules[i].descriptor.required_roles & mask)!=h->modules[i].descriptor.required_roles)
        return BONES_MOD_MISSING;
    /* Plan all dependencies before running a single callback. */
    for(n=0;n<h->count;n++) {
        int rank=100;selected=UINT32_MAX;
        for(i=0;i<h->count;i++) {
            uint32_t k,j;int used=0;
            for(j=0;j<n;j++)if(h->order[j]==i)used=1;
            if(used)continue;
            if((h->modules[i].descriptor.required_roles & scheduled)!=h->modules[i].descriptor.required_roles)continue;
            k=(uint32_t)role_rank(h->modules[i].descriptor.role);
            if(selected==UINT32_MAX || (int)k<rank || ((int)k==rank &&
                strcmp(h->modules[i].descriptor.id,h->modules[selected].descriptor.id)<0)) {
                selected=i;rank=(int)k;
            }
        }
        if(selected==UINT32_MAX)return BONES_MOD_EINVAL;
        h->order[n]=(uint8_t)selected;
        if(h->modules[selected].descriptor.role!=BONES_ROLE_EXTENSION)
            scheduled |= BONES_ROLE_BIT(h->modules[selected].descriptor.role);
    }
    h->error=BONES_MOD_OK;
    h->phase=PHASE_START;
    for(n=0;n<h->count;n++) {
        ModuleEntry *e=&h->modules[h->order[n]];
        err=e->descriptor.start?e->descriptor.start(e->descriptor.user_context,&e->services):0;
        if(err!=0) {
            while(h->started) {
                ModuleEntry *prev=&h->modules[h->order[--h->started]];
                if(prev->descriptor.stop)prev->descriptor.stop(prev->descriptor.user_context);
                prev->started=0;
            }
            h->phase=PHASE_IDLE;h->error=err;return err;
        }
        e->started=1;h->started++;
    }
    h->running=1;h->phase=PHASE_IDLE;return BONES_MOD_OK;
}
static void send_events(BonesModHost *h) {
    uint32_t i,k;h->phase=PHASE_EVENTS;
    for(i=0;i<h->event_count;i++)for(k=0;k<h->started;k++) {
        ModuleEntry *e=&h->modules[h->order[k]];
        if(e->descriptor.on_event)e->descriptor.on_event(e->descriptor.user_context,&h->events[i]);
    }
    h->event_count=0;
}
static void add_physics_event(BonesModHost *h,uint32_t type,uint64_t id) {
    BonesModEvent *e;
    if(h->event_count==MAX_TRANSIENT_EVENTS) {h->error=BONES_MOD_FULL;return;}
    e=&h->events[h->event_count++];memset(e,0,sizeof(*e));e->size=sizeof(*e);
    e->type=type;e->source_id=id;e->tick=h->foundation->engine.tick;
}
int32_t bones_mod_host_step(BonesModHost *h,const BonesInput *local) {
    uint32_t i,j,count;uint64_t ids[BONES_MOD_MAX_ACTORS];uint8_t grounded[BONES_MOD_MAX_ACTORS];
    if(!h||!h->running||h->phase)return BONES_MOD_BAD_STATE;
    count=h->foundation->entity_count;
    if(count>BONES_MOD_MAX_ACTORS)return BONES_MOD_FULL;
    for(i=0;i<count;i++){ids[i]=h->foundation->entities[i].id;grounded[i]=(uint8_t)h->foundation->entities[i].body.grounded;}
    h->phase=PHASE_BEFORE;
    for(i=0;i<h->started;i++) {
        ModuleEntry *e=&h->modules[h->order[i]];
        if(e->descriptor.before_tick)e->descriptor.before_tick(e->descriptor.user_context);
    }
    h->phase=PHASE_STEP;
    if(!bones_foundation_step(h->foundation,local)){h->phase=PHASE_IDLE;return BONES_MOD_BAD_STATE;}
    for(i=0;i<count;i++) {
        const BonesEntity *e=bones_foundation_find(h->foundation,ids[i]);
        if(e && (uint8_t)e->body.grounded!=grounded[i])add_physics_event(h,e->body.grounded?BONES_EVT_LAND:BONES_EVT_JUMP,e->id);
    }
    h->phase=PHASE_AFTER;
    for(j=0;j<h->started;j++) {
        ModuleEntry *e=&h->modules[h->order[j]];
        if(e->descriptor.after_tick)e->descriptor.after_tick(e->descriptor.user_context);
    }
    send_events(h);
    h->phase=PHASE_IDLE;
    return h->error? h->error:BONES_MOD_OK;
}
int32_t bones_mod_host_frame(BonesModHost *h) {
    BonesModFrame frame;uint32_t i,j;
    if(!h||!h->running||h->phase)return BONES_MOD_BAD_STATE;
    if(!bones_foundation_make_scene(h->foundation,&h->legacy_scene))return BONES_MOD_BAD_STATE;
    h->phase=PHASE_BUILD;
    for(i=0;i<h->started;i++){
        ModuleEntry *e=&h->modules[h->order[i]];
        if(e->descriptor.build_scene)e->descriptor.build_scene(e->descriptor.user_context);
    }
    memset(&frame,0,sizeof(frame));frame.size=sizeof(frame);frame.tick=h->foundation->engine.tick;
    frame.camera_x=h->legacy_scene.camera.position.x;
    frame.camera_y=h->legacy_scene.camera.position.y;
    frame.camera_z=h->legacy_scene.camera.position.z;
    frame.camera_yaw=h->legacy_scene.camera.yaw;frame.camera_pitch=h->legacy_scene.camera.pitch;
    frame.quad_count=h->legacy_scene.quad_count;frame.sprite_count=h->legacy_scene.sprite_count;
    for(i=0;i<frame.quad_count;i++) {
        BonesModQuad *q=&h->quads[i];const BonesQuad *s=&h->legacy_scene.quads[i];
        memset(q,0,sizeof(*q));q->size=sizeof(*q);q->material_id=s->material;
        for(j=0;j<4;j++) {
            q->vertices[j]=(BonesModVertex){s->vertices[j].position.x,s->vertices[j].position.y,s->vertices[j].position.z,
                                             s->vertices[j].u,s->vertices[j].v};
        }
    }
    for(i=0;i<frame.sprite_count;i++) {
        BonesModSprite *s=&h->sprites[i];const BonesSprite *src=&h->legacy_scene.sprites[i];
        *s=(BonesModSprite){sizeof(*s),src->feet.x,src->feet.y,src->feet.z,src->width,src->height,src->animation_tick};
    }
    frame.quads=h->quads;frame.sprites=h->sprites;
    h->phase=PHASE_PRESENT;
    for(i=0;i<h->started;i++) {
        ModuleEntry *e=&h->modules[h->order[i]];
        if(e->descriptor.present)e->descriptor.present(e->descriptor.user_context,&frame);
    }
    h->phase=PHASE_IDLE;
    return h->error?h->error:BONES_MOD_OK;
}
void bones_mod_host_stop(BonesModHost *h) {
    if(!h||h->phase || !h->running)return;
    h->phase=PHASE_STOP;
    while(h->started) {
        ModuleEntry *e=&h->modules[h->order[--h->started]];
        if(e->descriptor.stop)e->descriptor.stop(e->descriptor.user_context);
        e->started=0;
    }
    h->running=0;h->phase=PHASE_IDLE;h->event_count=0;
}
static void close_library(void *lib) {
    if(!lib)return;
#ifndef BONES_MODKIT_NO_DYNLIB
#ifdef _WIN32
    FreeLibrary((HMODULE)lib);
#else
    dlclose(lib);
#endif
#endif
}
int32_t bones_mod_host_load(BonesModHost *h,const char *path) {
#ifndef BONES_MODKIT_NO_DYNLIB
    BonesDynamicQuery query=NULL;void *lib=NULL;int32_t rc;BonesModDescriptor d;uint32_t before;
    if(!h||!path||!path[0])return BONES_MOD_EINVAL;
    if(h->running||h->phase||h->count==BONES_MOD_LIMIT)return BONES_MOD_BAD_STATE;
#ifdef _WIN32
    lib=(void*)LoadLibraryA(path);
    if(lib)query=(BonesDynamicQuery)(void*)GetProcAddress((HMODULE)lib,"bones_mod_query_v1");
#else
    lib=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if(lib) {
        void *symbol=dlsym(lib,"bones_mod_query_v1");
        /* POSIX dlsym: copy object pointer representation to function pointer. */
        memcpy(&query,&symbol,sizeof(query));
    }
#endif
    if(!lib||!query){close_library(lib);return BONES_MOD_MISSING;}
    memset(&d,0,sizeof(d));d.size=sizeof(d);
    rc=query(BONES_MOD_ABI,&d);before=h->count;
    if(rc==BONES_MOD_OK)rc=add_descriptor(h,&d);
    if(rc!=BONES_MOD_OK||h->count==before){close_library(lib);return rc;}
    h->modules[before].library=lib;
    return BONES_MOD_OK;
#else
    (void)h;(void)path;
    return BONES_MOD_MISSING;
#endif
}
void bones_mod_host_destroy(BonesModHost *h) {
    uint32_t i;
    if(!h)return;
    bones_mod_host_stop(h);
    if(h->running)return;
    for(i=0;i<h->count;i++)close_library(h->modules[i].library);
    free(h);
}
