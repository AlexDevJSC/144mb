#include "eye5.h"
#include <string.h>

static uint32_t hash5(uint32_t n) {
    n ^= n >> 16; n *= UINT32_C(0x7feb352d);
    n ^= n >> 15; n *= UINT32_C(0x846ca68b);
    return n ^ (n >> 16);
}
static int clamp5(int v) {return v<0?0:(v>255?255:v);}
static uint32_t rgba5(int r,int g,int b) {
    return UINT32_C(0xff000000)|((uint32_t)clamp5(r)<<16)|
           ((uint32_t)clamp5(g)<<8)|(uint32_t)clamp5(b);
}
static const uint8_t palette5[4][4][3]={
    {{76,81,80},{93,91,84},{66,75,79},{96,93,83}},
    {{117,76,59},{106,70,55},{123,91,69},{99,67,53}},
    {{98,77,58},{78,64,52},{117,94,68},{82,73,61}},
    {{123,113,83},{101,101,79},{133,121,89},{109,99,78}}
};
int eye5_valid(const Eye5Recipe *r) {
    return r&&r->family<=EYE5_THATCH&&r->palette<4u&&
        (r->width==8u||r->width==16u)&&
        (r->course==8u||r->course==16u)&&
        r->stagger<=3u&&r->wear<=15u&&r->edge<=15u&&r->flags==0u;
}
int eye5_generate(const Eye5Recipe *r,uint32_t *out,size_t n) {
    unsigned x,y;
    if(!eye5_valid(r)||!out||n<EYE5_TILE_PIXELS)return 0;
    for(y=0;y<64u;++y) for(x=0;x<64u;++x) {
        unsigned row=y/r->course, yy=y%r->course;
        unsigned shift=(r->stagger==0u?0u:((row&1u)?r->width/2u:0u));
        unsigned sx=(x+shift)&63u, col=sx/r->width, xx=sx%r->width;
        uint32_t tile=hash5(r->seed ^ (row*UINT32_C(0x9e3779b9)) ^
                            (col*UINT32_C(0x85ebca6b)) ^ r->family*73u);
        uint32_t grain=hash5(tile ^ (xx*719u + yy*1259u));
        unsigned fine=grain>>24;
        int variation=(int)((tile>>23)&31u)-15;
        int noise=(int)(fine>>4)-8;
        int delta=variation/2 + noise/2;
        int seam=0,high=0;
        switch(r->family) {
          case EYE5_SLATE:
            seam=(yy==r->course-1u||xx==0u);
            high=(yy==0u || xx==r->width-1u);
            delta+=(int)((hash5(tile ^ (xx/3u)*117u ^ (yy/2u)*977u)>>27)&7u)-4;
            if((grain&255u)<(unsigned)r->wear*3u && yy>1u) delta-=22;
            break;
          case EYE5_CLAY:
            seam=(yy==r->course-1u || xx==0u);
            high=(xx==r->width/3u || yy==0u);
            delta+= ((int)(xx<r->width/2u?xx:r->width-1u-xx)-3)*3;
            if((grain&127u)<r->wear)delta-=18;
            break;
          case EYE5_WOOD:
            seam=(yy==r->course-1u||xx==0u);
            high=yy==0u;
            delta+=(int)((hash5(tile^(xx/2u)*991u)>>26)&15u)-7;
            if(xx==r->width-2u && yy>r->course/2u)delta-=12;
            break;
          default:
            seam=(yy==r->course-1u || yy==r->course-2u);
            high=yy==0u || (grain&31u)==0u;
            delta+= ((int)(hash5(r->seed^((x/2u)&31u)*199u^row*701u)>>27)&15)-8;
            if(xx%3u==0u)delta-=8;
            break;
        }
        if(seam)delta-=24+(int)r->edge;
        else if(high)delta+=7+(int)r->edge/2;
        if(yy>r->course-4u && !seam)delta-=4;
        if((grain&255u)<(unsigned)r->wear*2u)delta-=8;
        const uint8_t *base=palette5[r->family][r->palette];
        out[y*64u+x]=rgba5((int)base[0]+delta,(int)base[1]+delta,(int)base[2]+delta);
    }
    return 1;
}
static void put32(uint8_t *p,uint32_t x){p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8);p[2]=(uint8_t)(x>>16);p[3]=(uint8_t)(x>>24);}
static uint32_t get32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint32_t digest5(const uint8_t *p,size_t n) {
    uint32_t h=UINT32_C(2166136261);
    for(size_t i=0;i<n;++i){if(i>=8u&&i<12u)continue;h^=p[i];h*=UINT32_C(16777619);}
    return h;
}
size_t eye5_write(const Eye5Recipe *r,unsigned n,uint8_t *p,size_t cap){
    size_t size=12u+(size_t)n*12u;
    if(!r||!p||!n||n>EYE5_MAX_RECIPES||cap<size)return 0;
    for(unsigned i=0;i<n;++i)if(!eye5_valid(&r[i]))return 0;
    memcpy(p,"EYE5",4);p[4]=12;p[5]=(uint8_t)n;p[6]=p[7]=0;
    for(unsigned i=0;i<n;++i){
        uint8_t *q=p+12u+i*12u;
        q[0]=r[i].family;q[1]=r[i].palette;q[2]=r[i].width;
        q[3]=r[i].course;q[4]=r[i].stagger;q[5]=r[i].wear;
        q[6]=r[i].edge;q[7]=0;put32(q+8,r[i].seed);
    }
    put32(p+8,digest5(p,size));return size;
}
int eye5_read(const uint8_t *p,size_t n,Eye5Bank *out){
    Eye5Bank tmp;unsigned count;
    if(!p||!out||n<12u||memcmp(p,"EYE5",4)||p[4]!=12u||!p[5]||p[6]||p[7])return 0;
    count=p[5];if(count>EYE5_MAX_RECIPES||n!=12u+count*12u||digest5(p,n)!=get32(p+8))return 0;
    memset(&tmp,0,sizeof tmp);tmp.count=count;
    for(unsigned i=0;i<count;++i){
        const uint8_t *q=p+12u+i*12u;Eye5Recipe *r=&tmp.recipe[i];
        *r=(Eye5Recipe){q[0],q[1],q[2],q[3],q[4],q[5],q[6],q[7],get32(q+8)};
        if(!eye5_valid(r))return 0;
    }
    *out=tmp;return 1;
}
int eye5_cache_init(Eye5Cache *c,const Eye5Bank *bank,uint32_t *arena,size_t cap,unsigned slots){
    if(!c||!bank||!arena||!bank->count||bank->count>EYE5_MAX_RECIPES||
       !slots||slots>EYE5_CACHE_MAX||cap<(size_t)slots*4096u)return 0;
    memset(c,0,sizeof(*c));c->bank=bank;c->arena=arena;c->slots=slots;return 1;
}
const uint32_t *eye5_cache_get(Eye5Cache *c,unsigned id){
    unsigned dest=0;
    if(!c||!c->bank||!c->arena||id>=c->bank->count)return NULL;
    for(unsigned i=0;i<c->count;++i)if(c->keys[i]==id){++c->hits;c->age[i]=++c->serial;return c->arena+i*4096u;}
    if(c->count<c->slots)dest=c->count;
    else for(unsigned i=1;i<c->slots;++i)if(c->age[i]<c->age[dest])dest=i;
    if(!eye5_generate(&c->bank->recipe[id],c->arena+dest*4096u,4096u))return NULL;
    if(c->count<c->slots)++c->count;
    c->keys[dest]=(uint8_t)id;c->age[dest]=++c->serial;++c->misses;
    return c->arena+dest*4096u;
}
void eye5_demo_recipes(Eye5Recipe r[4]){
    if(!r)return;
    r[0]=(Eye5Recipe){EYE5_SLATE,0,16,8,1,9,10,0,UINT32_C(0x14440501)};
    r[1]=(Eye5Recipe){EYE5_CLAY,1,8,16,1,8,9,0,UINT32_C(0x14440502)};
    r[2]=(Eye5Recipe){EYE5_WOOD,0,8,16,1,9,8,0,UINT32_C(0x14440503)};
    r[3]=(Eye5Recipe){EYE5_THATCH,1,16,8,0,10,8,0,UINT32_C(0x14440504)};
}

static Eye5Vertex v5(float x,float y,float z,float u,float v) {return (Eye5Vertex){x,y,z,u,v};}
static int append5(Eye5RoofMesh *m,Eye5Vertex a,Eye5Vertex b,Eye5Vertex c,Eye5Vertex d,unsigned n,unsigned role){
    if(m->count>=EYE5_MAX_SURFACES)return 0;
    Eye5Surface *s=&m->surface[m->count++];
    s->vertex[0]=a;s->vertex[1]=b;s->vertex[2]=c;s->vertex[3]=d;
    s->vertex_count=(uint8_t)n;s->role=(uint8_t)role;return 1;
}
static void hip_cap5(Eye5RoofMesh *m, float x0,float y0,float z0,
                     float x1,float y1,float z1){
    float dx=x1-x0,dz=z1-z0;
    float ax=dx<0.f?-dx:dx,az=dz<0.f?-dz:dz;
    float length=ax>az?ax+0.4f*az:az+0.4f*ax;
    float sx=-dz/length*0.050f,sz=dx/length*0.050f;
    float u=length*0.58f;
    append5(m,v5(x0+sx,y0+0.026f,z0+sz,0,0),
              v5(x1+sx,y1+0.033f,z1+sz,u,0),
              v5(x1-sx,y1+0.033f,z1-sz,u,0.3f),
              v5(x0-sx,y0+0.026f,z0-sz,0,0.3f),
              4,EYE5_SURFACE_RIDGE);
}
int eye5_build_roof(const Eye5RoofSpec *s,Eye5RoofMesh *out){
    Eye5RoofMesh m={0};float w,z0,z1,e,r,side,r0,r1;
    float u_long,v_slope;
    if(!s||!out||!(s->half_width>=0.5f&&s->half_width<=1000.f)||
       !(s->half_depth>=0.5f&&s->half_depth<=1000.f)||
       !(s->wall_height>=0.f&&s->wall_height<=1000.f)||
       !(s->roof_rise>=0.25f&&s->roof_rise<=1000.f)||
       !(s->overhang>=0.f&&s->overhang<=2.f)||s->form>EYE5_HIP)return 0;
    w=s->half_width+s->overhang;z0=-s->half_depth-s->overhang;
    z1=s->half_depth+s->overhang;e=s->wall_height;
    r=e+s->roof_rise;side=z1-z0;
    u_long=side*0.59f;
    v_slope=(w>s->roof_rise?w+0.4f*s->roof_rise:
             s->roof_rise+0.4f*w)*0.62f;
    if(s->form==EYE5_GABLE){
        append5(&m,v5(-w,e,z0,0,v_slope),v5(0,r,z0,0,0),
                  v5(0,r,z1,u_long,0),v5(-w,e,z1,u_long,v_slope),
                  4,EYE5_SURFACE_ROOF);
        append5(&m,v5(0,r,z0,0,0),v5(w,e,z0,0,v_slope),
                  v5(w,e,z1,u_long,v_slope),v5(0,r,z1,u_long,0),
                  4,EYE5_SURFACE_ROOF);
        r0=z0;r1=z1;
    }else{
        float inset=side*0.38f;
        r0=z0+inset;r1=z1-inset;
        append5(&m,v5(-w,e,z0,0,v_slope),v5(0,r,r0,inset*0.59f,0),
                  v5(0,r,r1,(r1-z0)*0.59f,0),v5(-w,e,z1,u_long,v_slope),
                  4,EYE5_SURFACE_ROOF);
        append5(&m,v5(0,r,r0,inset*0.59f,0),v5(w,e,z0,0,v_slope),
                  v5(w,e,z1,u_long,v_slope),v5(0,r,r1,(r1-z0)*0.59f,0),
                  4,EYE5_SURFACE_ROOF);
        append5(&m,v5(-w,e,z0,0,v_slope),v5(w,e,z0,w*1.18f,v_slope),
                  v5(0,r,r0,w*0.59f,0),v5(0,r,r0,w*0.59f,0),
                  3,EYE5_SURFACE_ROOF);
        append5(&m,v5(0,r,r1,w*0.59f,0),v5(w,e,z1,w*1.18f,v_slope),
                  v5(-w,e,z1,0,v_slope),v5(-w,e,z1,0,v_slope),
                  3,EYE5_SURFACE_ROOF);
    }
    append5(&m,v5(-0.065f,r+0.025f,r0,0,0),v5(0.065f,r+0.025f,r0,0,0.3f),
              v5(0.065f,r+0.025f,r1,(r1-r0)*0.59f,0.3f),
              v5(-0.065f,r+0.025f,r1,(r1-r0)*0.59f,0),4,EYE5_SURFACE_RIDGE);
    for(unsigned i=0;i<2u;++i){
        float x=i?w:-w;
        append5(&m,v5(x,e+0.035f,z0,0,0),v5(x,e+0.035f,z1,side,0),
                  v5(x,e-0.14f,z1,side,1),v5(x,e-0.14f,z0,0,1),
                  4,EYE5_SURFACE_TRIM);
    }
    if(s->form==EYE5_GABLE){
        for(unsigned j=0;j<2u;++j){
            float z=j?z1:z0;
            for(unsigned i=0;i<2u;++i){
                float x=i?w:-w;
                append5(&m,v5(x,e+0.035f,z,0,0),v5(0,r+0.035f,z,2,0),
                          v5(0,r-0.06f,z,2,1),v5(x,e-0.065f,z,0,1),
                          4,EYE5_SURFACE_TRIM);
            }
        }
    }else{
        for(unsigned j=0;j<2u;++j){
            float z=j?z1:z0;
            append5(&m,v5(-w,e+0.035f,z,0,0),v5(w,e+0.035f,z,w*2.f,0),
                      v5(w,e-0.14f,z,w*2.f,1),v5(-w,e-0.14f,z,0,1),
                      4,EYE5_SURFACE_TRIM);
        }
        hip_cap5(&m,0,r,r0,-w,e,z0);
        hip_cap5(&m,0,r,r0,w,e,z0);
        hip_cap5(&m,0,r,r1,-w,e,z1);
        hip_cap5(&m,0,r,r1,w,e,z1);
    }
    *out=m;return 1;
}
