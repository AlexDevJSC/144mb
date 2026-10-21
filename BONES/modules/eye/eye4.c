#include "eye4.h"
#include <string.h>

static uint32_t mix4(uint32_t n) {
    n ^= n >> 16; n *= UINT32_C(0x7feb352d);
    n ^= n >> 15; n *= UINT32_C(0x846ca68b); return n ^ (n >> 16);
}
static int clamp4(int n,int lo,int hi) {return n<lo?lo:(n>hi?hi:n);}
static uint32_t color4(int r,int g,int b) {
    return UINT32_C(0xff000000)|((uint32_t)clamp4(r,0,255)<<16)|
           ((uint32_t)clamp4(g,0,255)<<8)|(uint32_t)clamp4(b,0,255);
}
/* Restrained mid-90s timber/plaster palette, deliberately no pure primaries. */
static const uint8_t colors4[][3] = {
    {133,126,108}, /* 0 unpainted plaster */
    {57,43,33},    /* 1 framing dark */
    {88,66,47},    /* 2 framing brown */
    {116,90,61},   /* 3 sunlit timber */
    {38,51,56},    /* 4 window black glass */
    {66,102,111},  /* 5 muted glass blue */
    {132,143,139}, /* 6 glimmer leaded glass */
    {30,32,32},    /* 7 mortar */
    {100,89,71},   /* 8 thatch */
    {74,64,52},    /* 9 dark thatch */
    {123,114,92},  /* 10 straw highlight */
    {85,86,81},    /* 11 gray stone */
    {134,129,110}, /* 12 stone edge */
    {54,54,53},    /* 13 iron */
    {163,144,99},  /* 14 dull brass */
    {108,103,86}   /* 15 dusty street */
};

/* Every command is six bytes in source rodata: kind, x0,y0,x1,y1,color.
   This is a small vector instruction language, not a stored bitmap. */
typedef struct {uint8_t op,x0,y0,x1,y1,c;} Op4;
enum {RECT4=1,LINE4=2,BOX4=3};
#define R(a,b,c,d,e) {RECT4,a,b,c,d,e}
#define L(a,b,c,d,e) {LINE4,a,b,c,d,e}
#define B(a,b,c,d,e) {BOX4,a,b,c,d,e}
/* Commands are shared by all variants of a motif, not repeated per recipe. */
static const Op4 wall4[]={
 R(0,0,63,63,0),R(0,0,5,63,1),R(58,0,63,63,1),
 R(0,0,63,5,1),R(0,57,63,63,1),R(0,29,63,34,2),
 R(6,7,7,56,3),R(56,7,57,56,3),L(1,29,62,29,3)
};
static const Op4 brace4[]={
 R(0,0,63,63,0),R(0,0,5,63,1),R(58,0,63,63,1),
 R(0,0,63,5,1),R(0,58,63,63,1),
 L(6,58,57,7,1),L(7,58,58,7,1),L(9,58,59,9,2),
 L(6,7,56,57,2), L(5,9,54,58,1)
};
static const Op4 window4[]={
 R(0,0,63,63,0),R(0,0,5,63,1),R(58,0,63,63,1),
 R(0,0,63,5,1),R(0,58,63,63,1),
 R(10,9,54,54,1),R(13,11,51,50,3),R(17,14,47,46,4),
 R(19,16,45,44,5),R(13,49,51,53,2),R(10,52,54,55,3),
 R(29,14,33,48,1),R(16,28,49,30,1),
 L(20,16,44,43,6),L(44,16,20,43,6),L(19,28,29,17,6),
 L(34,17,45,28,6),L(19,31,30,43,6),L(44,31,33,43,6),
 B(10,9,54,54,3)
};
static const Op4 door4[]={
 R(0,0,63,63,0),R(0,0,5,63,1),R(58,0,63,63,1),
 R(0,0,63,5,1),R(11,9,53,63,1),R(14,12,50,63,2),
 R(16,13,22,63,3),R(25,13,30,63,3),R(36,13,41,63,3),
 R(48,13,49,63,1),R(14,22,50,24,1),R(14,54,50,56,1),
 R(42,39,47,44,13),R(44,40,46,42,14),
 R(15,12,50,14,3)
};
static const Op4 roof4[]={
 R(0,0,63,63,8), R(0,15,63,16,9),R(0,31,63,32,9),
 R(0,47,63,48,9),R(0,0,63,1,10)
};
static const Op4 foundation4[]={
 R(0,0,63,63,11),R(0,15,63,17,7),R(0,31,63,33,7),
 R(0,47,63,49,7), R(0,1,63,2,12),
 R(31,0,33,15,7),R(15,17,17,31,7),R(45,17,47,31,7),
 R(30,33,32,47,7),R(15,49,17,63,7),R(45,49,47,63,7)
};
static const Op4 road4[]={
 R(0,0,63,63,15),R(0,15,63,17,7),R(0,31,63,33,7),
 R(0,47,63,49,7),R(31,0,33,15,7),R(15,17,17,31,7),
 R(45,17,47,31,7),R(30,33,32,47,7),R(15,49,17,63,7),
 R(45,49,47,63,7)
};
static const Op4 upper4[]={
 R(0,0,63,63,0),R(0,0,6,63,1),R(57,0,63,63,1),
 R(0,0,63,6,1),R(0,56,63,63,1),
 L(6,56,32,8,1),L(7,56,33,8,2),
 L(32,8,57,56,1),L(33,9,58,56,2),R(0,35,63,40,2)
};
static const struct {const Op4 *ops;unsigned count;} scripts4[8]={
 {wall4,sizeof wall4/sizeof *wall4},
 {brace4,sizeof brace4/sizeof *brace4},
 {window4,sizeof window4/sizeof *window4},
 {door4,sizeof door4/sizeof *door4},
 {roof4,sizeof roof4/sizeof *roof4},
 {foundation4,sizeof foundation4/sizeof *foundation4},
 {road4,sizeof road4/sizeof *road4},
 {upper4,sizeof upper4/sizeof *upper4}
};

static int linehit4(int x,int y,int x0,int y0,int x1,int y1) {
    int dx=x1-x0,dy=y1-y0,px=x-x0,py=y-y0;
    int l2=dx*dx+dy*dy,dot=px*dx+py*dy;
    if(!l2) return px*px+py*py<=1;
    if(dot<0||dot>l2)return 0;
    /* Compare pixel-center distance squared with a ~1.5 pixel-wide line. */
    int64_t cross=(int64_t)px*dy-(int64_t)py*dx;
    return cross*cross <= (int64_t)l2*2;
}
static int op_hit4(const Op4 *o,int x,int y) {
    if(o->op==RECT4)return x>=o->x0&&y>=o->y0&&x<=o->x1&&y<=o->y1;
    if(o->op==LINE4)return linehit4(x,y,o->x0,o->y0,o->x1,o->y1);
    return (x>=o->x0&&x<=o->x1&&(y==o->y0||y==o->y1))||
           (y>=o->y0&&y<=o->y1&&(x==o->x0||x==o->x1));
}
static uint32_t layered4(const Eye4Recipe *r,int x,int y,uint32_t base) {
    const Op4 *ops=scripts4[r->motif].ops;
    unsigned n=scripts4[r->motif].count,i;
    int code=-1, tone=0;
    uint32_t h=mix4(r->base.seed^(uint32_t)(x*7319+y*19013)^(uint32_t)(r->motif*137));
    for(i=0;i<n;++i) if(op_hit4(&ops[i],x,y)) code=(int)ops[i].c;
    if(code<0)return base;
    /* Static: fixed outlines and flat colors, hybrid: wear and shading. */
    if(r->method==EYE4_HYBRID) {
        int small=(int)((h>>23)&31u)-15;
        int broad=(int)((mix4(r->base.seed^((uint32_t)(x/6)<<7)^(uint32_t)(y/5))>>25)&31u)-15;
        tone=small/3+broad/2;
        if(code==0 && ((h>>17)&127u)<r->detail)tone-=20;
        if(code==1||code==2||code==3) {
            if((h&127u)<r->detail*2u)tone-=15;
            if((y&15)==7)tone+=9;
        }
        if(code==5) {tone=((int)(h&15u)-7)/2;}
        if(code==8||code==9||code==10) {
            if((y&3)==1)tone-=11;
            tone+=((int)(mix4(((uint32_t)(x/3)+(uint32_t)(y*13))^r->base.seed)>>27)-15);
            if((h&255u)<r->detail*2u)tone-=20;
        }
    }
    /* Slight architectural variations remain deterministic and cheap. */
    if(code==0&&r->style==1)tone+=9;
    if(code==2&&r->style==2)tone-=9;
    if(code==8&&r->style==3)tone-=11;
    if(r->method==EYE4_HYBRID && code==0) {
        int source=(int)(base&255u)-90;
        tone+=source/7; /* let the EYE3 base surface inform the plaster */
    }
    return color4((int)colors4[code][0]+tone,
                  (int)colors4[code][1]+tone,
                  (int)colors4[code][2]+tone);
}

int eye4_valid(const Eye4Recipe *r) {
    return r&&eye3_valid(&r->base)&&r->motif<=EYE4_PANEL_UPPER&&
           r->style<4u&&r->detail<=15u&&r->method<=EYE4_HYBRID;
}
int eye4_generate(const Eye4Recipe *r,uint32_t *out,size_t count) {
    uint32_t backdrop[64u*64u];
    unsigned x,y;
    if(!eye4_valid(r)||!out||count<4096u)return 0;
    if(r->method==EYE4_STATIC) {
        for(y=0;y<64u;++y) for(x=0;x<64u;++x)
            out[y*64u+x]=layered4(r,(int)x,(int)y,0u);
        return 1;
    }
    if(!eye3_generate(&r->base,backdrop,4096u))return 0;
    for(y=0;y<64u;++y)for(x=0;x<64u;++x) {
        /* procedural baseline: pure EYE3 pixel materials, no architectural ink */
        out[y*64u+x] = r->method==EYE4_PROCEDURAL ? backdrop[y*64u+x] :
            layered4(r,(int)x,(int)y,backdrop[y*64u+x]);
    }
    return 1;
}
static void le32(uint8_t *o,uint32_t x) {
    o[0]=(uint8_t)x;o[1]=(uint8_t)(x>>8);o[2]=(uint8_t)(x>>16);o[3]=(uint8_t)(x>>24);
}
static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static uint32_t checksum4(const uint8_t *data,size_t size) {
    uint32_t h=UINT32_C(2166136261);size_t i;
    for(i=0;i<size;++i) {
        if(i>=8u&&i<12u)continue;
        h^=data[i];h*=UINT32_C(16777619);
    }
    return h;
}
size_t eye4_write(const Eye4Recipe *recipes,unsigned n,uint8_t *o,size_t cap) {
    unsigned i;size_t size=EYE4_HEADER_BYTES+(size_t)n*EYE4_RECORD_BYTES;
    if(!recipes||!o||!n||n>EYE4_MAX_RECIPES||cap<size)return 0;
    for(i=0;i<n;++i)if(!eye4_valid(&recipes[i]))return 0;
    o[0]='E';o[1]='Y';o[2]='E';o[3]='4';o[4]=16;o[5]=(uint8_t)n;o[6]=o[7]=0;
    for(i=0;i<n;++i) {
        const Eye4Recipe *r=&recipes[i];uint8_t *p=o+EYE4_HEADER_BYTES+i*16u;
        p[0]=r->base.family;p[1]=r->base.palette;p[2]=r->base.scale;
        p[3]=r->base.pattern;p[4]=r->base.wear;p[5]=r->base.growth;
        p[6]=r->base.contrast;p[7]=r->base.flags;le32(p+8,r->base.seed);
        p[12]=r->motif;p[13]=r->style;p[14]=r->detail;p[15]=r->method;
    }
    le32(o+8,checksum4(o,size));return size;
}
int eye4_read(const uint8_t *p,size_t size,Eye4Bank *out) {
    unsigned n,i; Eye4Bank tmp;
    if(!p||!out||size<EYE4_HEADER_BYTES||memcmp(p,"EYE4",4)||p[4]!=16||
       p[6]||p[7]||!p[5])return 0;
    n=p[5];
    if(n>EYE4_MAX_RECIPES||size!=EYE4_HEADER_BYTES+(size_t)n*16u||
       checksum4(p,size)!=read32(p+8))return 0;
    memset(&tmp,0,sizeof(tmp));tmp.count=n;
    for(i=0;i<n;++i) {
        const uint8_t *s=p+EYE4_HEADER_BYTES+i*16u;
        Eye4Recipe *r=&tmp.recipe[i];
        r->base=(EyeRecipe){s[0],s[1],s[2],s[3],s[4],s[5],s[6],s[7],read32(s+8)};
        r->motif=s[12];r->style=s[13];r->detail=s[14];r->method=s[15];
        if(!eye4_valid(r))return 0;
    }
    *out=tmp;return 1;
}
int eye4_cache_init(Eye4Cache *c,const Eye4Bank *b,uint32_t *arena,
                    size_t capacity,unsigned slots) {
    if(!c||!b||!arena||!slots||slots>EYE4_CACHE_SLOTS||
       !b->count||b->count>EYE4_MAX_RECIPES||capacity<4096u*(size_t)slots)return 0;
    memset(c,0,sizeof(*c));c->bank=b;c->storage=arena;c->slots=slots;return 1;
}
const uint32_t *eye4_cache_get(Eye4Cache *c,unsigned id) {
    unsigned i,j;
    if(!c||!c->bank||!c->storage||id>=c->bank->count)return NULL;
    for(i=0;i<c->count;++i)if(c->keys[i]==id) {
        c->ages[i]=++c->serial;++c->hits;return c->storage+i*4096u;
    }
    if(c->count<c->slots)j=c->count;
    else {j=0;for(i=1;i<c->slots;++i)if(c->ages[i]<c->ages[j])j=i;}
    if(!eye4_generate(&c->bank->recipe[id],c->storage+j*4096u,4096u))return NULL;
    if(c->count<c->slots)++c->count;
    c->keys[j]=(uint16_t)id;c->ages[j]=++c->serial;++c->misses;
    return c->storage+j*4096u;
}
void eye4_demo_recipes(Eye4Recipe r[8],Eye4Method mode) {
    static const uint8_t families[8]={7,7,7,2,8,0,9,7};
    unsigned i;
    if(!r)return;
    for(i=0;i<8u;++i) {
        r[i].base=(EyeRecipe){families[i],0,1,(uint8_t)(i%4u),9,3,11,0,
                              UINT32_C(0x14440000)+(uint32_t)i*1777u};
        r[i].motif=(uint8_t)i;r[i].style=(uint8_t)(i%4u);
        r[i].detail=9;r[i].method=(uint8_t)mode;
    }
    /* The plain procedural baseline uses closest available EYE3 surfaces. */
    r[EYE4_DOOR].base.family=2;r[EYE4_WINDOW].base.family=2;
    r[EYE4_ROOF].base.family=8;r[EYE4_FOUNDATION].base.family=0;
    r[EYE4_ROAD].base.family=9;
}
