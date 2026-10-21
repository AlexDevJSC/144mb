/* Test-only functionally mirrored implementation from Eye PR #12 (source branch
   eye/diagnostico-y-soporte-skin-052), for cross-module testing only. */
#include "eye_display.h"
static uint32_t hash_xy(unsigned x, unsigned y) {
    uint32_t h=(uint32_t)x * UINT32_C(0x9e3779b9) ^
               (uint32_t)y * UINT32_C(0x85ebca6b) ^ UINT32_C(0x14400005);
    h^=h>>16; h*=UINT32_C(0x7feb352d); h^=h>>15;
    return h;
}
int eye_display_scan(const uint32_t *pixels, size_t n, EyeFrameStats *out) {
    EyeFrameStats s={0}; size_t i;
    if (!pixels || !out || n!=EYE_DISPLAY_PIXELS) return 0;
    s.hash=UINT64_C(14695981039346656037);
    s.total=(uint32_t)n;
    for (i=0;i<n;++i) {
        uint32_t v=pixels[i],rgb=v&UINT32_C(0xffffff);
        if(rgb==0)++s.black_rgb;
        if(rgb==EYE_DISPLAY_CLEAR_RGB)++s.clear_rgb;
        else ++s.other_rgb;
        if((v>>24)!=255u)++s.nonopaque;
        for (unsigned b=0;b<4;++b) { s.hash^=(uint8_t)(v>>(b*8u)); s.hash*=UINT64_C(1099511628211); }
    }
    s.all_black=(uint8_t)(s.black_rgb==n);
    s.clear_only=(uint8_t)(s.clear_rgb==n);
    *out=s;return 1;
}
static uint32_t dim_rgb(uint32_t c, unsigned d) {
    unsigned remain=255u-d;
    unsigned r=(((c>>16)&255u)*remain+127u)/255u;
    unsigned g=(((c>>8)&255u)*remain+127u)/255u;
    unsigned b=((c&255u)*remain+127u)/255u;
    return UINT32_C(0xff000000)|(r<<16)|(g<<8)|b;
}
static uint32_t menu_pixel(unsigned x,unsigned y) {
    unsigned grain=(hash_xy(x/2u,y/2u)>>27)&7u;
    unsigned vignette=(x<160u?160u-x:x-160u)/9u;
    vignette+=(y<100u?100u-y:y-100u)/8u;
    unsigned r=43u+grain-vignette/2u;
    unsigned g=38u+grain-vignette/2u;
    unsigned b=35u+grain-vignette/2u;
    if ((y%24u)<2u || ((x+(y/24u%2u)*20u)%64u)<2u) {r=r*4u/5u; g=g*4u/5u; b=b*4u/5u;}
    if ((x>=11u&&x<=16u)||(x>=303u&&x<=308u)) {r+=12u;g+=10u;b+=7u;}
    if (r>255u)r=255u;
    if (g>255u)g=255u;
    if (b>255u)b=255u;
    return UINT32_C(0xff000000)|(r<<16)|(g<<8)|b;
}
int eye_display_prepare(uint32_t *dst,size_t n,const uint32_t *scene,size_t scene_n,EyeDisplayMode mode,unsigned dim) {
    if(!dst||n<EYE_DISPLAY_PIXELS||mode>EYE_DISPLAY_MENU_GENERATED||dim>255u)return 0;
    if(mode!=EYE_DISPLAY_MENU_GENERATED && (!scene||scene_n<EYE_DISPLAY_PIXELS))return 0;
    for(unsigned y=0;y<EYE_DISPLAY_HEIGHT;++y)for(unsigned x=0;x<EYE_DISPLAY_WIDTH;++x) {
        unsigned i=y*EYE_DISPLAY_WIDTH+x;
        if(mode==EYE_DISPLAY_MENU_GENERATED)dst[i]=menu_pixel(x,y);
        else if(mode==EYE_DISPLAY_GAME)dst[i]=UINT32_C(0xff000000)|(scene[i]&UINT32_C(0xffffff));
        else dst[i]=dim_rgb(scene[i],dim);
    }
    return 1;
}
