#include "eye3.h"
#include "eye_tone.h"

#include <string.h>

static uint32_t hash3(uint32_t s) {
    s ^= s >> 16; s *= UINT32_C(0x7feb352d);
    s ^= s >> 15; s *= UINT32_C(0x846ca68b);
    return s ^ (s >> 16);
}
static unsigned n3(uint32_t seed, unsigned x, unsigned y, uint32_t salt) {
    return hash3(seed ^ hash3(x + salt) ^ hash3(y + salt * UINT32_C(0x9e3779b9))) >> 24;
}
static int clamp3(int a, int lo, int hi) { return a < lo ? lo : (a > hi ? hi : a); }
static int abs3(int a) { return a < 0 ? -a : a; }
static uint32_t rgb3(int r, int g, int b) {
    return UINT32_C(0xff000000) | ((uint32_t)clamp3(r,0,255) << 16) |
        ((uint32_t)clamp3(g,0,255) << 8) | (uint32_t)clamp3(b,0,255);
}
static const uint8_t palette3[3][4][3] = {
    { /* slate, fired tile, shale, weathered green slate */
      {79,82,79},{122,87,68},{70,81,93},{89,99,78}},
    { /* cobbles, dusty limestone, dark wet stone, ochre path */
      {116,110,96},{144,133,112},{83,91,86},{122,108,83}},
    { /* stone carving, warm sandstone, aged marble, limestone */
      {133,130,117},{146,123,93},{128,130,126},{149,143,119}}
};

int eye3_valid(const EyeRecipe *r) {
    return r && r->family < EYE3_FAMILY_COUNT && r->palette < 4u &&
        r->scale >= 1u && r->scale <= 4u && r->pattern < 8u &&
        r->wear <= 15u && r->growth <= 15u && r->contrast <= 15u && r->flags == 0u;
}

uint32_t eye3_pixel(const EyeRecipe *r, int px, int py) {
    unsigned x, y, grain, mottled;
    int lx, ly, nx, ny, shade, mask, stone;
    int red, green, blue;
    const uint8_t *base;
    if (!eye3_valid(r)) return 0;
    /* EYE3 changes chromatic appearance but keeps EYE2 geometry for families 0..7.
       This is a deliberate new version, never retroactively alters EYE2 output. */
    if (r->family < EYE_FAMILY_COUNT)
        return eye_tone_pixel(eye2_pixel(r, px, py), EYE_TONE_AGED);
    x = (unsigned)px & 63u;
    y = (unsigned)py & 63u;
    base = palette3[r->family - EYE3_ROOF][r->palette];
    red = base[0]; green = base[1]; blue = base[2];
    grain = n3(r->seed, x, y, 0x1301u);
    mottled = n3(r->seed, x/4u, y/4u, 0x1302u);
    shade = ((int)grain - 128) / 7 + ((int)mottled - 128) / 5;
    stone = 0;
    switch (r->family) {
    case EYE3_ROOF: {
        /* Interlocking slate shingles: periodic 64x64, staggered courses. */
        int w = 32 >> (r->scale - 1u);
        int h = 16 >> (r->scale - 1u);
        int row = (int)y / h;
        int offset = ((row + (int)r->pattern) & 1) * (w / 2);
        int sx = ((int)x + offset) & 63;
        int col = sx / w;
        unsigned tile = n3(r->seed, (unsigned)col, (unsigned)row, 0x1303u);
        lx = sx % w; ly = (int)y % h;
        shade += ((int)(tile & 31u) - 15) / 2;
        if (ly == 0) shade -= 54;
        if (ly == 1) shade += 18;
        if (ly == h-1) shade -= 28;
        if (lx == 0 && ly > h/2) shade -= 34;
        if (lx == w-1 && ly > h/2) shade -= 19;
        /* Chips and hairline cracks remain within each shingle. */
        if (r->wear > 5u && ly > 1 && ly < h-1 &&
            lx + (int)(tile % 5u) == w/2 &&
            tile > (unsigned)(249 - r->wear * 5u)) shade -= 34;
        if (r->growth > 3u && mottled < (unsigned)(r->growth*6u) && ly > h/2)
            stone = 1;
        break;
    }
    case EYE3_COBBLE: {
        /* Offset cobbles with crooked joints and per-stone shade; no alpha. */
        int w = 16 >> (r->scale - 1u);
        int h = 16 >> (r->scale - 1u);
        int row = (int)y/h;
        int stagger = ((row+(int)r->pattern)&1)*(w/2);
        int sx = ((int)x + stagger) & 63;
        int col = sx/w;
        unsigned tile = n3(r->seed,(unsigned)col,(unsigned)row,0x1403u);
        int skew = ((int)(tile & 3u)-1);
        lx = sx%w; ly = (int)y%h;
        mask = (lx == 0 || ly == 0 || (lx == w-1 && (ly&1)==0));
        shade += ((int)(tile & 47u)-24) / 2;
        if (mask) shade -= 62;
        else if (lx==1 || ly==1) shade += 21;
        else if (lx==w-2 || ly==h-2) shade -= 16;
        if (w>=4 && lx==w/2+skew && ly==h/2 && r->wear>8u) shade -= 30;
        if (r->growth>5u && mask && mottled<(unsigned)(r->growth*9u)) stone = 1;
        break;
    }
    case EYE3_CARVING: {
        /* Small reusable carved molding; can be tiled across architectural trim. */
        int w = 32 >> (r->scale - 1u);
        int h = 32 >> (r->scale - 1u);
        int cx = w/2, cy = h/2;
        lx = (int)x % w;
        ly = (int)y % h;
        nx = abs3(lx-cx); ny=abs3(ly-cy);
        shade += ((int)n3(r->seed,x/(unsigned)w,y/(unsigned)h,0x1503u)-128)/14;
        if (lx==0 || ly==0) shade-=58;
        if (lx==1 || ly==1) shade+=30;
        if (lx==w-1 || ly==h-1) shade-=23;
        if ((r->pattern & 1u) ? (nx+ny == (w/3)) : (nx == w/4 || ny == h/4))
            shade -= 30;
        if ((r->pattern & 2u) && (nx+ny == (w/3)-1)) shade += 26;
        if (r->wear>7u && grain<(unsigned)(r->wear*3u)) shade -= 26;
        if (r->growth>8u && mottled<(unsigned)(r->growth*4u)) stone = 1;
        break;
    }
    default: return 0u;
    }
    shade = clamp3(shade * (9+(int)r->contrast)/17,-90,58);
    shade = (shade/7)*7;
    if (stone) { red = red*3/4; green = green*4/5+9; blue = blue*3/4; }
    return rgb3(red+shade, green+shade, blue+shade);
}

int eye3_generate(const EyeRecipe *r, uint32_t *pixels, size_t capacity) {
    unsigned x, y;
    if (!eye3_valid(r) || !pixels || capacity < EYE_SIZE*EYE_SIZE) return 0;
    for (y=0; y<EYE_SIZE; ++y) for (x=0; x<EYE_SIZE; ++x)
        pixels[y*EYE_SIZE+x] = eye3_pixel(r,(int)x,(int)y);
    return 1;
}

/* EYE3 packet layout intentionally matches EYE2; version is included in hash. */
static uint32_t hashpack(const uint8_t *data,size_t n) {
    uint32_t h=UINT32_C(2166136261);
    size_t i;
    for(i=0;i<n;++i) {
        if(i>=8u && i<12u) continue;
        h^=data[i];h*=UINT32_C(16777619);
    }
    return h;
}
static void write32(uint8_t *p,uint32_t v) {
    p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);
}
static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
size_t eye3_pack_write(const EyeRecipe *recipes,unsigned count,uint8_t *dst,size_t cap) {
    size_t len,i;
    if(!recipes || !dst || !count || count>EYE_MAX_RECIPES) return 0;
    len=EYE_HEADER_BYTES+(size_t)count*EYE_RECIPE_BYTES;
    if(cap<len) return 0;
    for(i=0;i<count;++i) if(!eye3_valid(&recipes[i])) return 0;
    dst[0]='E';dst[1]='Y';dst[2]='E';dst[3]='3';
    dst[4]=(uint8_t)EYE_RECIPE_BYTES;dst[5]=(uint8_t)count;
    dst[6]=0;dst[7]=0;
    for(i=0;i<count;++i) {
        const EyeRecipe *r=&recipes[i];
        uint8_t *p=dst+EYE_HEADER_BYTES+i*EYE_RECIPE_BYTES;
        p[0]=r->family;p[1]=r->palette;p[2]=r->scale;p[3]=r->pattern;
        p[4]=r->wear;p[5]=r->growth;p[6]=r->contrast;p[7]=r->flags;
        write32(p+8,r->seed);
    }
    write32(dst+8,hashpack(dst,len));
    return len;
}
int eye3_pack_read(const uint8_t *data,size_t len,EyeRecipe *out,
                   unsigned capacity,unsigned *count) {
    unsigned n,i;
    if(!data||!out||!count||len<EYE_HEADER_BYTES||memcmp(data,"EYE3",4)||
       data[4]!=EYE_RECIPE_BYTES||data[6]||data[7]) return 0;
    n=data[5];
    if(!n||n>capacity||len!=EYE_HEADER_BYTES+(size_t)n*EYE_RECIPE_BYTES||
       hashpack(data,len)!=read32(data+8)) return 0;
    for(i=0;i<n;++i) {
        const uint8_t *p=data+EYE_HEADER_BYTES+(size_t)i*EYE_RECIPE_BYTES;
        EyeRecipe r={p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],read32(p+8)};
        if(!eye3_valid(&r)) return 0; /* no partial mutation */
    }
    for(i=0;i<n;++i) {
        const uint8_t *p=data+EYE_HEADER_BYTES+(size_t)i*EYE_RECIPE_BYTES;
        EyeRecipe r={p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],read32(p+8)};
        out[i]=r;
    }
    *count=n;
    return 1;
}
const char *eye3_family_name(unsigned f) {
    static const char *names[3]={"roof","cobble","carving"};
    if(f<EYE_FAMILY_COUNT) return eye_family_name(f);
    return f>=EYE3_ROOF&&f<EYE3_FAMILY_COUNT ? names[f-EYE3_ROOF] : "invalid";
}
int eye3_family_from_name(const char *name) {
    static const char *names[3]={"roof","cobble","carving"};
    int i=eye_family_from_name(name);
    unsigned j;
    if(i>=0) return i;
    for(j=0;j<3u;++j) if(name&&!strcmp(name,names[j])) return (int)(EYE3_ROOF+j);
    return -1;
}

/* Convert signed fixed-point UV to a wrapping index, floor for negative UV. */
static unsigned uv_component(int32_t q16) {
    int64_t scaled=(int64_t)q16*64;
    int64_t integer=scaled/65536;
    if(scaled<0 && (scaled%65536)!=0) --integer;
    return (unsigned)((uint64_t)integer & 63u);
}
uint32_t eye_uv_sample_repeat(const uint32_t *texture,int32_t u,int32_t v) {
    if(!texture) return 0;
    return texture[uv_component(v)*64u+uv_component(u)];
}

int eye_cache_init(EyeMaterialCache *c,const EyeTextureBank *bank,
                   uint32_t *storage,size_t capacity,unsigned slots) {
    if(!c||!bank||!storage||slots==0u||slots>EYE_CACHE_LIMIT||
       capacity<(size_t)slots*EYE_SIZE*EYE_SIZE||
       (bank->version!=1u&&bank->version!=2u&&bank->version!=3u)) return 0;
    memset(c,0,sizeof(*c));c->bank=bank;c->texels=storage;c->slots=slots;
    return 1;
}
const uint32_t *eye_cache_get(EyeMaterialCache *c,unsigned id) {
    unsigned i,slot;
    if(!c||!c->bank||!c->texels||id>=c->bank->count) return NULL;
    for(i=0;i<c->used;++i) {
        if(c->material[i]==id) {
            ++c->hits;
            c->age[i]=++c->counter;
            return c->texels+(size_t)i*EYE_SIZE*EYE_SIZE;
        }
    }
    if(c->used<c->slots) slot=c->used;
    else {
        slot=0;
        for(i=1;i<c->slots;++i) if(c->age[i]<c->age[slot]) slot=i;
    }
    if(!eye_bank_render(c->bank,id,c->texels+(size_t)slot*EYE_SIZE*EYE_SIZE,EYE_SIZE*EYE_SIZE)) return NULL;
    if(c->used<c->slots) ++c->used;
    c->material[slot]=id;c->age[slot]=++c->counter;++c->misses;
    return c->texels+(size_t)slot*EYE_SIZE*EYE_SIZE;
}
