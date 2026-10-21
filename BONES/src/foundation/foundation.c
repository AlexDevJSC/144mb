#include "../../include/bones/foundation.h"
#include "../bones/bones_internal.h"
#include <string.h>
#include <math.h>

static bool finite_vec(BonesVec3 p) { return isfinite(p.x) && isfinite(p.y) && isfinite(p.z); }
static bool nonoverlap(const BonesFoundation *f, uint64_t id, uint32_t *index) {
    uint32_t i;
    for(i=0;i<f->entity_count;i++) if(f->entities[i].id==id) {
        if(index) *index=i;
        return false;
    }
    return true;
}
const BonesEntity *bones_foundation_find(const BonesFoundation *f, uint64_t id) {
    uint32_t index;
    if(!f || !id || nonoverlap(f,id,&index)) return NULL;
    return &f->entities[index];
}
static BonesEntity *mutable_entity(BonesFoundation *f, uint64_t id) {
    return (BonesEntity *)bones_foundation_find(f,id);
}
static bool push_fact(BonesFoundation *f, uint32_t kind, uint64_t id) {
    BonesFact *p;
    if(f->fact_count>=BONES_FOUNDATION_MAX_FACTS || f->next_fact_sequence==UINT64_MAX)
        return false;
    p=&f->facts[f->fact_count++];
    p->sequence=++f->next_fact_sequence;
    p->tick=f->engine.tick;
    p->kind=kind;
    p->subject=id;
    return true;
}
bool bones_foundation_init(BonesFoundation *f,uint64_t world_id,uint64_t seed,uint32_t branch_id) {
    BonesEntity *local;
    if(!f || !world_id || !branch_id) return false;
    memset(f,0,sizeof(*f));
    bones_init(&f->engine,seed);
    f->world_id=world_id;
    f->branch_id=branch_id;
    f->next_entity_id=((uint64_t)branch_id<<32)|UINT64_C(2);
    f->entity_count=1;
    local=&f->entities[0];
    local->id=1; local->owner=1; local->kind=BONES_KIND_PLAYER;
    local->enabled=1; local->body=f->engine.player;
    return true;
}
bool bones_foundation_spawn(BonesFoundation *f,BonesEntityKind kind,
                             uint32_t owner,BonesVec3 feet,uint64_t *out_id) {
    BonesEntity *e;
    uint64_t id;
    if(!f || !out_id || (kind!=BONES_KIND_PLAYER && kind!=BONES_KIND_NPC && kind!=BONES_KIND_OBJECT)
       || !finite_vec(feet) || feet.y<0.0f || f->entity_count==BONES_FOUNDATION_MAX_ENTITIES
       || (uint32_t)f->next_entity_id==UINT32_MAX || f->fact_count>=BONES_FOUNDATION_MAX_FACTS
       || (kind==BONES_KIND_PLAYER && owner==0) || (kind!=BONES_KIND_PLAYER && owner!=0)) return false;
    id=f->next_entity_id;
    if(!push_fact(f,BONES_FACT_SPAWN,id)) return false;
    e=&f->entities[f->entity_count++];
    memset(e,0,sizeof(*e));
    e->id=id; e->kind=(uint8_t)kind; e->owner=owner; e->enabled=1;
    e->body=f->engine.player;
    e->body.feet=feet;
    e->body.velocity=(BonesVec3){0,0,0};
    e->body.grounded=feet.y==0.0f;
    e->body.jump_held=false;
    f->next_entity_id++;
    *out_id=id;
    return true;
}
bool bones_foundation_remove(BonesFoundation *f,uint64_t id) {
    uint32_t i, j;
    uint32_t pending;
    if(!f || id==1 || nonoverlap(f,id,&i) || f->fact_count>=BONES_FOUNDATION_MAX_FACTS) return false;
    for(pending=0;pending<f->command_count;pending++)
        if(f->commands[pending].actor_id==id) return false;
    if(!push_fact(f,BONES_FACT_REMOVE,id)) return false;
    for(j=i+1;j<f->entity_count;j++) f->entities[j-1]=f->entities[j];
    f->entity_count--;
    return true;
}
bool bones_foundation_submit(BonesFoundation *f,const BonesCommand *c) {
    const BonesEntity *e;
    uint32_t i;
    if(!f || !c || f->command_count==BONES_FOUNDATION_MAX_COMMANDS ||
       f->engine.tick==UINT64_MAX || c->tick!=f->engine.tick+1 ||
       (f->in_step && c->tick<=f->executing_tick) ||
       !c->sequence || c->forward < -1 || c->forward>1 ||
       c->strafe < -1 || c->strafe>1 || c->yaw_milliradians < -300 ||
       c->yaw_milliradians > 300 || c->sprint>1 || c->jump>1) return false;
    e=bones_foundation_find(f,c->actor_id);
    if(!e || !e->enabled || e->kind==BONES_KIND_OBJECT || c->issuer!=e->owner ||
       c->sequence<=e->last_sequence) return false;
    for(i=0;i<f->command_count;i++)
        if(f->commands[i].actor_id==c->actor_id) return false;
    f->commands[f->command_count++]=*c;
    return true;
}
static BonesInput to_input(const BonesCommand *c) {
    BonesInput i={0};
    i.forward=(uint8_t)(c->forward>0); i.backward=(uint8_t)(c->forward<0);
    i.right=(uint8_t)(c->strafe>0); i.left=(uint8_t)(c->strafe<0);
    i.sprint=c->sprint; i.jump=c->jump;
    i.mouse_dx=(float)c->yaw_milliradians/(2.3f);
    return i;
}
bool bones_foundation_step(BonesFoundation *f,const BonesInput *local_input) {
    BonesInput idle={0}, for_player;
    BonesCommand consumed[BONES_FOUNDATION_MAX_COMMANDS];
    uint32_t i,j,consumed_count;
    if(!f || f->engine.tick==UINT64_MAX || f->in_step) return false;
    for_player=local_input?*local_input:idle;
    if(!isfinite(for_player.mouse_dx)||!isfinite(for_player.mouse_dy)) return false;
    consumed_count=f->command_count;
    memcpy(consumed,f->commands,consumed_count*sizeof(BonesCommand));
    for(i=0;i<consumed_count;i++) {
        if(consumed[i].actor_id==1) for_player=to_input(&consumed[i]);
    }
    /* Detach current tick inputs BEFORE callback dispatch. after_tick is free
       to schedule commands for the NEXT tick without losing them. */
    f->command_count=0;
    f->in_step=1;
    f->executing_tick=f->engine.tick+1;
    if(!bones_step(&f->engine,&for_player,BONES_TICK_DT)) {
        f->in_step=0;
        if(!f->command_count) {
            memcpy(f->commands,consumed,consumed_count*sizeof(BonesCommand));
            f->command_count=consumed_count;
        }
        return false;
    }
    for(i=0;i<f->entity_count;i++) {
        BonesEntity *e=&f->entities[i];
        BonesInput input=idle;
        if(e->id==1) {
            e->body=f->engine.player;
        } else if(e->enabled && e->kind!=BONES_KIND_OBJECT) {
            bool was_grounded=e->body.grounded;
            for(j=0;j<consumed_count;j++) if(consumed[j].actor_id==e->id) {
                input=to_input(&consumed[j]);
                break;
            }
            bones_physics_step_actor(&f->engine.world,&e->body,&input,BONES_TICK_DT);
            if(was_grounded!=e->body.grounded) {
                BonesEvent ev={e->body.grounded?BONES_EVENT_LAND:BONES_EVENT_JUMP,e->id,0};
                (void)bones_emit_event(&f->engine,&ev);
            }
        }
    }
    for(i=0;i<consumed_count;i++) {
        BonesEntity *e=mutable_entity(f,consumed[i].actor_id);
        if(e) e->last_sequence=consumed[i].sequence;
    }
    bones_dispatch_events(&f->engine);
    f->in_step=0;
    f->executing_tick=0;
    return true;
}
uint32_t bones_foundation_read_facts(const BonesFoundation *f,uint64_t after,
                                    BonesFact *out,uint32_t cap) {
    uint32_t i,n=0;
    if(!f || (cap>0 && !out)) return 0;
    for(i=0;i<f->fact_count && n<cap;i++)
        if(f->facts[i].sequence>after) out[n++]=f->facts[i];
    return n;
}
bool bones_foundation_ack_facts(BonesFoundation *f,uint64_t through) {
    uint32_t n=0;
    if(!f || through>f->next_fact_sequence) return false;
    while(n<f->fact_count && f->facts[n].sequence<=through) n++;
    if(n) {
        memmove(f->facts,f->facts+n,(f->fact_count-n)*sizeof(BonesFact));
        f->fact_count-=n;
    }
    return true;
}
/* Canonical serializer. Header: magic[4], version/u32, byte_length/u32,
   checksum/u32 (over bytes from offset 16), world_id/u64, seed/u64, branch_id/u32,
   generator/u32, tick/u64, next_entity_id/u64, fact_sequence/u64,
   entity_count/u32, fact_count/u32, collider_count/u32, then entities,
   pending facts and physical colliders.
   No raw structs/padding/pointers. */
static void put32(uint8_t **p,uint32_t x) {
    unsigned i; for(i=0;i<4;i++){*(*p)++=(uint8_t)(x&255u);x>>=8;}
}
static void put64(uint8_t **p,uint64_t x) {
    unsigned i; for(i=0;i<8;i++){*(*p)++=(uint8_t)(x&255u);x>>=8;}
}
static uint32_t get32(const uint8_t **p) {
    uint32_t x=0; int i; for(i=0;i<4;i++) x|=(uint32_t)*(*p)++ <<(i*8);return x;
}
static uint64_t get64(const uint8_t **p) {
    uint64_t x=0; int i; for(i=0;i<8;i++) x|=(uint64_t)*(*p)++ <<(i*8);return x;
}
static void putfloat(uint8_t **p,float f) {uint32_t x;memcpy(&x,&f,4);put32(p,x);}
static float getfloat(const uint8_t **p) {uint32_t x=get32(p);float f;memcpy(&f,&x,4);return f;}
static uint32_t crc32(const uint8_t *p,size_t n) {
    uint32_t crc=UINT32_MAX; size_t i; unsigned bit;
    for(i=0;i<n;i++) {
        crc^=p[i];
        for(bit=0;bit<8;bit++) crc=(crc>>1)^((crc&1u)?UINT32_C(0xedb88320):0u);
    }
    return ~crc;
}
#define FOUNDATION_HEADER_SIZE 76u
#define FOUNDATION_COLLIDER_SIZE 36u
#define FOUNDATION_FACT_SIZE 28u
#define FOUNDATION_ENTITY_SIZE 81u
bool bones_foundation_export(const BonesFoundation *f,uint8_t *dst,size_t cap,size_t *written) {
    uint8_t *p; size_t required; uint32_t i;
    if(!f || !dst || !written || sizeof(float)!=4 || f->entity_count>BONES_FOUNDATION_MAX_ENTITIES) return false;
    if(f->command_count || f->in_step || f->fact_count>BONES_FOUNDATION_MAX_FACTS ||
       f->engine.world.collider_count>BONES_MAX_COLLIDERS) return false;
    required=FOUNDATION_HEADER_SIZE+f->entity_count*FOUNDATION_ENTITY_SIZE+f->fact_count*FOUNDATION_FACT_SIZE+f->engine.world.collider_count*FOUNDATION_COLLIDER_SIZE;
    if(cap<required) return false;
    p=dst; *p++='B'; *p++='S'; *p++='F'; *p++='1';
    put32(&p,BONES_FOUNDATION_VERSION); put32(&p,(uint32_t)required); put32(&p,0);
    put64(&p,f->world_id);put64(&p,f->engine.world.seed);put32(&p,f->branch_id);
    put32(&p,f->engine.world.generator_version);
    put64(&p,f->engine.tick);put64(&p,f->next_entity_id);
    put64(&p,f->next_fact_sequence);put32(&p,f->entity_count);put32(&p,f->fact_count);put32(&p,f->engine.world.collider_count);
    for(i=0;i<f->entity_count;i++) {
        const BonesEntity *e=&f->entities[i];
        put64(&p,e->id); put32(&p,e->owner);
        *p++=e->kind;*p++=e->enabled;
        *p++=(uint8_t)(e->body.grounded?1:0);*p++=(uint8_t)(e->body.jump_held?1:0);
        put64(&p,e->last_sequence);
        putfloat(&p,e->body.feet.x);putfloat(&p,e->body.feet.y);putfloat(&p,e->body.feet.z);
        putfloat(&p,e->body.velocity.x);putfloat(&p,e->body.velocity.y);putfloat(&p,e->body.velocity.z);
        putfloat(&p,e->body.yaw);putfloat(&p,e->body.pitch);
        putfloat(&p,e->body.radius);putfloat(&p,e->body.height);putfloat(&p,e->body.eye_height);
        /* 13 reserved bytes; must remain zero and verified during decoding. */
        memset(p,0,FOUNDATION_ENTITY_SIZE-68u);p+=FOUNDATION_ENTITY_SIZE-68u;
    }
    for(i=0;i<f->fact_count;i++) {
        const BonesFact *a=&f->facts[i];
        put64(&p,a->sequence);put64(&p,a->tick);put64(&p,a->subject);put32(&p,a->kind);
    }
    for(i=0;i<f->engine.world.collider_count;i++) {
        const BonesCollider *c=&f->engine.world.colliders[i];
        put64(&p,c->id);*p++=c->solid;*p++=c->render_box;*p++=c->material;*p++=0;
        putfloat(&p,c->bounds.min.x);putfloat(&p,c->bounds.min.y);putfloat(&p,c->bounds.min.z);
        putfloat(&p,c->bounds.max.x);putfloat(&p,c->bounds.max.y);putfloat(&p,c->bounds.max.z);
    }
    if((size_t)(p-dst)!=required) return false;
    p=dst+12; put32(&p,crc32(dst+16,required-16));
    *written=required;return true;
}
bool bones_foundation_restore(BonesFoundation *f,const uint8_t *src,size_t n) {
    const uint8_t *p; uint32_t count,fact_count,collider_count,i,j,version,length,stored_crc,gen,branch;
    BonesFact parsed_facts[BONES_FOUNDATION_MAX_FACTS];
    BonesCollider parsed_colliders[BONES_MAX_COLLIDERS];
    uint64_t world,seed,tick,next_id,fact_seq;
    BonesEntity parsed[BONES_FOUNDATION_MAX_ENTITIES];
    if(!f||!src||n<FOUNDATION_HEADER_SIZE||n>BONES_FOUNDATION_SNAPSHOT_MAX||
       f->engine.subsystem_started||f->engine.subsystem_count||f->engine.module_count||
       f->command_count||f->fact_count||f->in_step||sizeof(float)!=4||
       memcmp(src,"BSF1",4)!=0) return false;
    p=src+4;version=get32(&p);length=get32(&p);stored_crc=get32(&p);
    if(version!=BONES_FOUNDATION_VERSION||length!=n||crc32(src+16,n-16)!=stored_crc) return false;
    world=get64(&p);seed=get64(&p);branch=get32(&p);gen=get32(&p);tick=get64(&p);
    next_id=get64(&p);fact_seq=get64(&p);count=get32(&p);fact_count=get32(&p);collider_count=get32(&p);
    if(world!=f->world_id||seed!=f->engine.world.seed||branch!=f->branch_id||
       gen!=f->engine.world.generator_version||count<1||count>BONES_FOUNDATION_MAX_ENTITIES||
       fact_count>BONES_FOUNDATION_MAX_FACTS||collider_count>BONES_MAX_COLLIDERS||
       n!=FOUNDATION_HEADER_SIZE+count*FOUNDATION_ENTITY_SIZE+fact_count*FOUNDATION_FACT_SIZE+collider_count*FOUNDATION_COLLIDER_SIZE || (next_id>>32)!=branch || (uint32_t)next_id<2) return false;
    memset(parsed,0,sizeof(parsed));
    for(i=0;i<count;i++) {
        BonesEntity *e=&parsed[i];
        uint8_t grounded, jump_held;
        e->id=get64(&p);e->owner=get32(&p);
        e->kind=*p++;e->enabled=*p++;
        grounded=*p++;jump_held=*p++;
        if(grounded>1 || jump_held>1) return false;
        e->body.grounded=(grounded!=0);e->body.jump_held=(jump_held!=0);
        e->last_sequence=get64(&p);
        e->body.feet.x=getfloat(&p);e->body.feet.y=getfloat(&p);e->body.feet.z=getfloat(&p);
        e->body.velocity.x=getfloat(&p);e->body.velocity.y=getfloat(&p);e->body.velocity.z=getfloat(&p);
        e->body.yaw=getfloat(&p);e->body.pitch=getfloat(&p);
        e->body.radius=getfloat(&p);e->body.height=getfloat(&p);e->body.eye_height=getfloat(&p);
        for(j=0;j<FOUNDATION_ENTITY_SIZE-68u;j++) if(*p++!=0) return false;
        if(!e->id || e->id>=next_id || !finite_vec(e->body.feet) ||
           !finite_vec(e->body.velocity) || !isfinite(e->body.yaw)||!isfinite(e->body.pitch)||
           !isfinite(e->body.radius)||!isfinite(e->body.height)||!isfinite(e->body.eye_height)||
           e->body.radius<=0||e->body.height<=0 || e->body.eye_height<=0 ||
           e->kind<BONES_KIND_PLAYER || e->kind>BONES_KIND_OBJECT || e->enabled!=1 ||
           (e->kind==BONES_KIND_PLAYER && e->owner==0) ||
           (e->kind!=BONES_KIND_PLAYER && e->owner!=0)) return false;
        for(j=0;j<i;j++) if(parsed[j].id==e->id) return false;
    }
    for(i=0;i<fact_count;i++) {
        BonesFact *a=&parsed_facts[i];
        a->sequence=get64(&p);a->tick=get64(&p);
        a->subject=get64(&p);a->kind=get32(&p);
        if(!a->sequence || a->sequence>fact_seq || a->tick>tick ||
           !a->subject || (a->kind!=BONES_FACT_SPAWN && a->kind!=BONES_FACT_REMOVE) ||
           (i && a->sequence<=parsed_facts[i-1].sequence)) return false;
    }
    for(i=0;i<collider_count;i++) {
        BonesCollider *c=&parsed_colliders[i];
        c->id=get64(&p);c->solid=*p++;c->render_box=*p++;c->material=*p++;
        if(*p++!=0) return false;
        c->bounds.min.x=getfloat(&p);c->bounds.min.y=getfloat(&p);c->bounds.min.z=getfloat(&p);
        c->bounds.max.x=getfloat(&p);c->bounds.max.y=getfloat(&p);c->bounds.max.z=getfloat(&p);
        if(!c->id || c->solid>1 || c->render_box>1 || c->material>BONES_MAT_WOOD ||
           !finite_vec(c->bounds.min) || !finite_vec(c->bounds.max) ||
           c->bounds.min.x>=c->bounds.max.x || c->bounds.min.y>=c->bounds.max.y ||
           c->bounds.min.z>=c->bounds.max.z) return false;
        for(j=0;j<i;j++) if(parsed_colliders[j].id==c->id) return false;
    }
    if(parsed[0].id!=1 || parsed[0].kind!=BONES_KIND_PLAYER || parsed[0].owner!=1) return false;
    /* Commit only after full validation: corrupt snapshots never touch runtime. */
    memcpy(f->entities,parsed,count*sizeof(BonesEntity));
    f->entity_count=count;f->next_entity_id=next_id;f->next_fact_sequence=fact_seq;
    memcpy(f->facts,parsed_facts,fact_count*sizeof(BonesFact));f->fact_count=fact_count;
    f->engine.tick=tick;f->engine.player=parsed[0].body;
    memcpy(f->engine.world.colliders,parsed_colliders,collider_count*sizeof(BonesCollider));
    f->engine.world.collider_count=collider_count;
    return true;
}
uint64_t bones_foundation_hash(const BonesFoundation *f) {
    uint8_t bytes[BONES_FOUNDATION_SNAPSHOT_MAX];size_t len=0,i;
    uint64_t h=UINT64_C(14695981039346656037);
    if(!bones_foundation_export(f,bytes,sizeof(bytes),&len)) return 0;
    for(i=0;i<len;i++){h^=bytes[i];h*=UINT64_C(1099511628211);}
    return h;
}

/* The authoritative entity table is never owned by Eye. The temporary
   visual representation is a generic sprite until Eye provides richer art. */
bool bones_foundation_make_scene(const BonesFoundation *f, BonesScene *scene) {
    uint32_t i;
    if(!f || !scene) return false;
    bones_make_scene(&f->engine,scene);
    for(i=0;i<f->entity_count;i++) {
        const BonesEntity *e=&f->entities[i];
        BonesSprite sprite;
        if(!e->enabled || e->id==1 || e->kind==BONES_KIND_OBJECT) continue;
        sprite.feet=e->body.feet;
        sprite.width=1.0f;sprite.height=e->body.height;
        sprite.animation_tick=(uint32_t)f->engine.tick;
        if(!bones_scene_add_sprite(scene,&sprite)) return false;
    }
    return true;
}
bool bones_foundation_render_frame(BonesFoundation *f) {
    BonesScene scene;
    BonesSubsystem *eye;
    if(!f || !bones_subsystem_attached(&f->engine,BONES_SLOT_EYE) ||
       !bones_start(&f->engine)) return false;
    eye=&f->engine.subsystems[BONES_SLOT_EYE];
    if(!eye->present || !bones_foundation_make_scene(f,&scene)) return false;
    eye->present(eye->context,&scene);
    return true;
}
