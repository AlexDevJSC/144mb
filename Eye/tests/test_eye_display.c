#include "eye_display.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

static void test_black_detector(void) {
    uint32_t *f=(uint32_t*)malloc(EYE_DISPLAY_PIXELS*sizeof(*f));
    EyeFrameStats s;
    assert(f);
    for(unsigned i=0;i<EYE_DISPLAY_PIXELS;++i)f[i]=UINT32_C(0xff000000);
    assert(eye_display_scan(f,EYE_DISPLAY_PIXELS,&s));
    assert(s.all_black && s.black_rgb==EYE_DISPLAY_PIXELS && !s.clear_only);
    uint64_t first=s.hash;
    for(unsigned i=0;i<EYE_DISPLAY_PIXELS;++i)f[i]=UINT32_C(0xff15202d);
    assert(eye_display_scan(f,EYE_DISPLAY_PIXELS,&s));
    assert(s.clear_only && !s.all_black && s.other_rgb==0 && s.hash!=first);
    /* Crosshair can hide the fact that no polygon survived clipping. */
    for(int d=-4;d<=4;++d)if(d) {
        f[100u*320u+160u+d]=UINT32_C(0xffe5dabe);
        f[(100u+d)*320u+160u]=UINT32_C(0xffe5dabe);
    }
    assert(eye_display_scan(f,EYE_DISPLAY_PIXELS,&s));
    assert(!s.clear_only && !s.all_black && s.other_rgb==16u);
    assert(s.clear_rgb==EYE_DISPLAY_PIXELS-16u);
    assert(!eye_display_scan(f,EYE_DISPLAY_PIXELS-1u,&s));
    assert(!eye_display_scan(f,EYE_DISPLAY_PIXELS,NULL));
    free(f);
}
static void test_menu(void) {
    uint32_t *src=malloc(EYE_DISPLAY_PIXELS*sizeof(uint32_t));
    uint32_t *dst=malloc((EYE_DISPLAY_PIXELS+1u)*sizeof(uint32_t));
    EyeFrameStats s;uint64_t hash1,hash2;
    assert(src&&dst);
    for(unsigned i=0;i<EYE_DISPLAY_PIXELS;++i)src[i]=UINT32_C(0xff804020);
    dst[EYE_DISPLAY_PIXELS]=UINT32_C(0x98765432);
    assert(eye_display_prepare(dst,EYE_DISPLAY_PIXELS,src,EYE_DISPLAY_PIXELS,EYE_DISPLAY_GAME,0));
    assert(dst[0]==src[0] && dst[EYE_DISPLAY_PIXELS-1u]==src[0]);
    assert(eye_display_prepare(dst,EYE_DISPLAY_PIXELS,src,EYE_DISPLAY_PIXELS,EYE_DISPLAY_MENU_DIM_SCENE,128));
    assert(dst[0]==UINT32_C(0xff402010));
    assert(src[0]==UINT32_C(0xff804020)); /* Eye scene is immutable */
    assert(eye_display_prepare(dst,EYE_DISPLAY_PIXELS,src,EYE_DISPLAY_PIXELS,EYE_DISPLAY_MENU_DIM_SCENE,255));
    assert(eye_display_scan(dst,EYE_DISPLAY_PIXELS,&s)&&s.all_black);
    assert(eye_display_prepare(dst,EYE_DISPLAY_PIXELS,NULL,0,EYE_DISPLAY_MENU_GENERATED,0));
    assert(eye_display_scan(dst,EYE_DISPLAY_PIXELS,&s)&&!s.all_black&&!s.clear_only);
    hash1=s.hash;
    assert(eye_display_prepare(dst,EYE_DISPLAY_PIXELS,NULL,0,EYE_DISPLAY_MENU_GENERATED,255));
    assert(eye_display_scan(dst,EYE_DISPLAY_PIXELS,&s));hash2=s.hash;
    assert(hash1==hash2); /* Stable menu bg, no reinitialization or rerolls. */
    assert(!eye_display_prepare(dst,EYE_DISPLAY_PIXELS,NULL,0,EYE_DISPLAY_GAME,0));
    assert(!eye_display_prepare(dst,EYE_DISPLAY_PIXELS,NULL,0,EYE_DISPLAY_MENU_DIM_SCENE,64));
    assert(!eye_display_prepare(dst,EYE_DISPLAY_PIXELS,src,EYE_DISPLAY_PIXELS,EYE_DISPLAY_GAME,256));
    assert(!eye_display_prepare(dst,EYE_DISPLAY_PIXELS-1,src,EYE_DISPLAY_PIXELS,EYE_DISPLAY_GAME,0));
    assert(dst[EYE_DISPLAY_PIXELS]==UINT32_C(0x98765432));
    free(src);free(dst);
}
/* Analytical regression of the actual P144 world_to_view formula: this
 * checks camera sign convention, not an assertion that Bones input is right.
 * EYE does not alter yaw/pitch or the physics player's X/Z on jumping. */
static void view(float yaw,float pitch,float dx,float dy,float dz,
                 float *outx,float *outy,float *outz){
    float sy=sinf(yaw),cy=cosf(yaw),sp=sinf(pitch),cp=cosf(pitch);
    float x=cy*dx-sy*dz,z=sy*dx+cy*dz;
    *outx=x;*outy=cp*dy-sp*z;*outz=sp*dy+cp*z;
}
static void test_camera_axes(void) {
    float x,y,z,px0,py0,pz0,px1,py1,pz1;
    view(0.f,0.f,1.f,0.f,5.f,&x,&y,&z);
    assert(fabsf(x-1.f)<1e-5f && fabsf(y)<1e-5f && fabsf(z-5.f)<1e-5f);
    view(0.1f,0.f,1.f,0.f,5.f,&x,&y,&z);
    assert(x<1.f && z>5.f); /* Yaw right rotates a fixed object left. */
    view(0.f,0.1f,0.f,0.f,5.f,&x,&y,&z);
    assert(y<0.f && z>0.f); /* Looking up shifts world centre down. */
    view(0.f,0.f,1.f,0.f,5.f,&px0,&py0,&pz0);
    view(0.f,0.f,1.f,-1.f,5.f,&px1,&py1,&pz1);
    assert(fabsf((px0/pz0)-(px1/pz1))<1e-6f); /* Jump: no horizontal drift at zero pitch. */
    assert(py1<py0);
}
int main(void){test_black_detector();test_menu();test_camera_axes();puts("EYE display diagnostics/menu tests: PASS");return 0;}
