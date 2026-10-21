#if !defined(_WIN32)
# define _POSIX_C_SOURCE 200809L
#endif
#include "skin/skin.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#if defined(_WIN32)
# define WIN32_LEAN_AND_MEAN
# include <windows.h>
# include <io.h>
#else
# include <unistd.h>
# include <fcntl.h>
#endif

/* Explicit v1 little-endian on-disk format. NOT a memcpy of C structs. */
_Static_assert(sizeof(float)==4, "Save v1 requires IEEE-like 32-bit float storage");
#define HEADER_SIZE 20u
static const uint8_t magic[8]={'S','K','I','N','1','4','4',1};
typedef struct {uint8_t data[SKIN_MAX_SAVE_BYTES];size_t at,length;bool good;} Buffer;
static void u8(Buffer *b,uint8_t x) {if(!b->good||b->at>=b->length){b->good=false;return;}b->data[b->at++]=x;}
static void u16(Buffer *b,uint16_t x) {u8(b,(uint8_t)x);u8(b,(uint8_t)(x>>8));}
static void u32(Buffer *b,uint32_t x) {unsigned i;for(i=0;i<4;i++)u8(b,(uint8_t)(x>>(i*8)));}
static void u64(Buffer *b,uint64_t x) {unsigned i;for(i=0;i<8;i++)u8(b,(uint8_t)(x>>(i*8)));}
static uint8_t r8(Buffer *b) {if(!b->good||b->at>=b->length){b->good=false;return 0;}return b->data[b->at++];}
static uint16_t r16(Buffer *b) {uint16_t v=r8(b);v|=(uint16_t)r8(b)<<8;return v;}
static uint32_t r32(Buffer *b) {uint32_t v=0;unsigned i;for(i=0;i<4;i++)v|=(uint32_t)r8(b)<<(i*8);return v;}
static uint64_t r64(Buffer *b) {uint64_t v=0;unsigned i;for(i=0;i<8;i++)v|=(uint64_t)r8(b)<<(i*8);return v;}
static void f32(Buffer *b,float x) {uint32_t u;memcpy(&u,&x,4);u32(b,u);}
static float rf32(Buffer *b) {uint32_t u=r32(b);float f;memcpy(&f,&u,4);return f;}
static void vec(Buffer *b,BonesVec3 v) {f32(b,v.x);f32(b,v.y);f32(b,v.z);}
static BonesVec3 rvec(Buffer *b) {BonesVec3 v;v.x=rf32(b);v.y=rf32(b);v.z=rf32(b);return v;}
static uint32_t checksum(const uint8_t *p,size_t n) {
    uint32_t crc=~(uint32_t)0;size_t i;unsigned k;
    for(i=0;i<n;i++) {crc^=p[i];for(k=0;k<8;k++)crc=(crc>>1) ^ (UINT32_C(0xedb88320)&(uint32_t)-(int)(crc&1u));}
    return ~crc;
}
static bool valid_world(const BonesWorld *w) {
    uint32_t i,j;
    if(!w||w->collider_count>BONES_MAX_COLLIDERS||!w->generator_version)return false;
    for(i=0;i<w->collider_count;i++) {
        const BonesCollider *c=&w->colliders[i];
        const BonesVec3 a=c->bounds.min,b=c->bounds.max;
        if(!c->id||!(a.x<b.x&&a.y<b.y&&a.z<b.z)||c->material>2||
          !isfinite(a.x)||!isfinite(a.y)||!isfinite(a.z)||
          !isfinite(b.x)||!isfinite(b.y)||!isfinite(b.z))return false;
        for(j=0;j<i;j++) if(w->colliders[j].id==c->id)return false;
    }
    return true;
}
static bool valid_player(const BonesPlayer *p) {
    const float a[]={p->feet.x,p->feet.y,p->feet.z,p->velocity.x,p->velocity.y,p->velocity.z,
                     p->yaw,p->pitch,p->radius,p->height,p->eye_height};
    unsigned i;for(i=0;i<sizeof(a)/sizeof(a[0]);i++) if(!isfinite(a[i])) return false;
    return p->radius>0&&p->radius<10&&p->height>0&&p->height<10&&p->eye_height>0&&p->eye_height<=p->height;
}
static bool encode(Buffer *b,const Skin *s,const BonesEngine *e,uint64_t generation) {
    uint32_t i;size_t payload_offset;
    if(!s||!e||!s->observed||e->world.seed!=s->expected_seed||
       e->world.generator_version!=s->expected_generator_version||
       !valid_world(&e->world)||!valid_player(&e->player)||
       s->event_count>SKIN_MAX_EVENTS||!s->branch_id||(!s->world_id.hi&&!s->world_id.lo)) return false;
    memset(b,0,sizeof(*b));b->good=true;b->length=sizeof(b->data);
    for(i=0;i<8;i++)u8(b,magic[i]);
    u16(b,SKIN_VERSION); u16(b,s->history_truncated?1u:0u);
    u32(b,0u); u32(b,0u); /* total length, crc32 patched after serialization */
    payload_offset=b->at;
    u64(b,generation); u64(b,s->world_id.hi);u64(b,s->world_id.lo);u64(b,s->branch_id);
    u64(b,e->world.seed);u32(b,e->world.generator_version);u64(b,e->tick);
    u64(b,s->next_event_sequence);u16(b,(uint16_t)e->world.collider_count);
    u16(b,(uint16_t)s->event_count);
    vec(b,e->player.feet);vec(b,e->player.velocity);
    f32(b,e->player.yaw);f32(b,e->player.pitch);f32(b,e->player.radius);
    f32(b,e->player.height);f32(b,e->player.eye_height);
    u8(b,e->player.grounded?1:0);u8(b,e->player.jump_held?1:0);
    for(i=0;i<e->world.collider_count;i++) {
        const BonesCollider *c=&e->world.colliders[i];
        u64(b,c->id);vec(b,c->bounds.min);vec(b,c->bounds.max);
        u8(b,c->solid);u8(b,c->render_box);u8(b,c->material);
    }
    for(i=0;i<s->event_count;i++) {
        const SkinEvent *ev=&s->events[i];
        u64(b,ev->sequence);u64(b,ev->tick);u32(b,ev->kind);
        u64(b,ev->entity_id);u64(b,ev->argument);
    }
    if(!b->good||b->at>UINT32_MAX)return false;
    {uint32_t len=(uint32_t)b->at,crc=checksum(b->data+payload_offset,b->at-payload_offset);
     for(i=0;i<4;i++){b->data[12+i]=(uint8_t)(len>>(8*i));b->data[16+i]=(uint8_t)(crc>>(8*i));}}
    return true;
}
typedef struct {Skin s;BonesWorld world;BonesPlayer player;uint64_t tick,generation;} Parsed;
static bool decode(Buffer *b,Parsed *p) {
    unsigned i;uint32_t n,crc,gen;uint16_t ver,flags,cols,events;
    memset(p,0,sizeof(*p));b->good=true;b->at=0;
    if(b->data[0]!='S'||b->data[1]!='K'||memcmp(b->data,magic,8)!=0)return false;
    b->at=8;ver=r16(b);flags=r16(b);n=r32(b);crc=r32(b);
    if(!b->good||ver!=SKIN_VERSION||(flags &~1u)!=0||n<HEADER_SIZE||n>SKIN_MAX_SAVE_BYTES||n!=b->length||
       checksum(b->data+HEADER_SIZE,n-HEADER_SIZE)!=crc) return false;
    p->generation=r64(b);p->s.save_generation=p->generation;
    p->s.world_id.hi=r64(b);p->s.world_id.lo=r64(b);
    p->s.branch_id=r64(b);p->world.seed=r64(b);gen=r32(b);
    p->world.generator_version=gen;p->tick=r64(b);
    p->s.next_event_sequence=r64(b);cols=r16(b);events=r16(b);
    if(!b->good||cols>BONES_MAX_COLLIDERS||events>SKIN_MAX_EVENTS||p->generation==0||
       p->s.branch_id==0||(!p->s.world_id.hi&&!p->s.world_id.lo)||!gen||
       p->s.next_event_sequence==0) return false;
    p->player.feet=rvec(b);p->player.velocity=rvec(b);
    p->player.yaw=rf32(b);p->player.pitch=rf32(b);
    p->player.radius=rf32(b);p->player.height=rf32(b);p->player.eye_height=rf32(b);
    p->player.grounded=r8(b)!=0;p->player.jump_held=r8(b)!=0;
    p->world.collider_count=cols;
    for(i=0;i<cols;i++) {
        BonesCollider *c=&p->world.colliders[i];
        c->id=r64(b);c->bounds.min=rvec(b);c->bounds.max=rvec(b);
        c->solid=r8(b);c->render_box=r8(b);c->material=r8(b);
    }
    p->s.event_count=events;
    {uint64_t prev_seq=0;for(i=0;i<events;i++) {
        SkinEvent *ev=&p->s.events[i];
        ev->sequence=r64(b);ev->tick=r64(b);ev->kind=r32(b);
        ev->entity_id=r64(b);ev->argument=r64(b);
        if(ev->sequence<=prev_seq||ev->sequence>=p->s.next_event_sequence||
           ev->tick>p->tick||ev->kind<SKIN_EVT_PLAYER_SAMPLE||ev->kind>SKIN_EVT_COLLIDER_UPDATED)return false;
        prev_seq=ev->sequence;
    }}
    if(!b->good||b->at!=n||!valid_world(&p->world)||!valid_player(&p->player))return false;
    p->s.history_truncated=(flags&1u)!=0;
    return true;
}
static bool path_for(char *target,size_t max,const char *base,const char *extension) {
    int n;
    if(!base||!base[0]||strlen(base)>220)return false;
    n=snprintf(target,max,"%s%s",base,extension);
    return n>0&&(size_t)n<max;
}
static bool read_slot(const char *base,int slot,Parsed *out) {
    char path[256];FILE *f;Buffer b;size_t n;int extra;
    if(!path_for(path,sizeof(path),base,slot?".b":".a"))return false;
    f=fopen(path,"rb");if(!f)return false;
    n=fread(b.data,1,sizeof(b.data),f);
    extra=fgetc(f); fclose(f);
    if(extra!=EOF||n<HEADER_SIZE)return false;
    b.at=0;b.length=n;b.good=true;
    if(!decode(&b,out))return false;
    /* decoder consumed exactly encoded length; no trailing bytes allowed */
    return b.at==n;
}
static bool matching(const Parsed *p,const Skin *s,const BonesEngine *e) {
    return p->s.world_id.hi==s->world_id.hi&&p->s.world_id.lo==s->world_id.lo&&
      p->s.branch_id==s->branch_id&&p->world.seed==e->world.seed&&
      p->world.seed==s->expected_seed&&p->world.generator_version==s->expected_generator_version&&
      p->world.generator_version==e->world.generator_version;
}
static bool best_slot(const char *path,const Skin *s,const BonesEngine *e,Parsed *out,int *which) {
    Parsed a,b;bool va=read_slot(path,0,&a),vb=read_slot(path,1,&b);
    if(va&&!matching(&a,s,e))va=false;
    if(vb&&!matching(&b,s,e))vb=false;
    if(!va&&!vb)return false;
    *which=(vb&&(!va||b.generation>a.generation))?1:0;
    *out=*which?b:a;
    return true;
}
static bool flush_file(FILE *f) {
    if(fflush(f)!=0)return false;
#if defined(_WIN32)
    return _commit(_fileno(f))==0;
#else
    return fsync(fileno(f))==0;
#endif
}
static bool replace_file(const char *tmp,const char *dest) {
#if defined(_WIN32)
    return MoveFileExA(tmp,dest,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
    if(rename(tmp,dest)!=0)return false;
    /* Ensure the directory entry survives a sudden power loss, where supported. */
    {
        char parent[256],*slash;int fd;
        size_t n=strlen(dest);
        if(n>=sizeof(parent))return false;
        memcpy(parent,dest,n+1);
        slash=strrchr(parent,'/');
        if(slash){if(slash==parent)slash[1]='\0';else *slash='\0';}
        else {parent[0]='.';parent[1]='\0';}
        fd=open(parent,O_RDONLY);
        if(fd<0)return false;
        {int rc=fsync(fd);close(fd);return rc==0;}
    }
#endif
}
bool skin_save(Skin *s,const BonesEngine *e,const char *path) {
    Parsed previous;int newest=0,target=0;uint64_t gen;Buffer b;
    char dst[256],tmp[256];FILE *f;bool ok;
    if(!s||!e||!path||!s->observed||
       e->world.seed!=s->expected_seed||e->world.generator_version!=s->expected_generator_version||
       !valid_world(&e->world)||
       !valid_player(&e->player) ) return false;
    {
      Parsed other;int k;
      for(k=0;k<2;k++)if(read_slot(path,k,&other)&&!matching(&other,s,e))return false;
    }
    if(best_slot(path,s,e,&previous,&newest)) {
        if(previous.generation==UINT64_MAX)return false;
        gen=previous.generation+1;target=1-newest;
    } else {gen=s->save_generation+1;target=0;}
    if(!gen||!encode(&b,s,e,gen)||!path_for(dst,sizeof(dst),path,target?".b":".a")||
       !path_for(tmp,sizeof(tmp),path,".tmp"))return false;
    f=fopen(tmp,"wb"); if(!f)return false;
    ok=fwrite(b.data,1,b.at,f)==b.at && flush_file(f);
    if(fclose(f)!=0)ok=false;
    if(!ok){remove(tmp);return false;}
    if(!replace_file(tmp,dst)){remove(tmp);return false;}
    s->save_generation=gen;s->dirty=false;
    return true;
}
bool skin_load(Skin *s,BonesEngine *e,const char *path) {
    Parsed chosen;int which;
    uint32_t i;
    if(!s||!e||!path||!best_slot(path,s,e,&chosen,&which))return false;
    /* Transactional in-memory commit: parse+validate EVERYTHING before mutation. */
    e->world=chosen.world;
    e->player=chosen.player;
    e->tick=chosen.tick;
    *s=chosen.s;
    s->expected_seed=e->world.seed;
    s->expected_generator_version=e->world.generator_version;
    s->last_player=e->player;
    s->known_count=e->world.collider_count;
    for(i=0;i<s->known_count;i++) {
        s->known_colliders[i]=e->world.colliders[i].id;
        s->observed_colliders[i]=e->world.colliders[i];
    }
    s->observed=true;s->dirty=false;
    /* NB: Bones module callback pointers remain untouched in *e. */
    return true;
}
bool skin_has_valid_save(const Skin *s,const BonesEngine *e,const char *path) {
    Parsed p;int which;
    if(!s||!e||!path)return false;
    return best_slot(path,s,e,&p,&which);
}
