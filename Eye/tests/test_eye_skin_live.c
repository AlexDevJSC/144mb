/* CI: actual Bones 0.4 host + real Eye and Skin modules, no fake ModKit services.
 * Produces three PPM captures for visual review. No Win32/GDI in this test.
 */
#include "../../BONES/include/bones/modkit_host.h"
#include "../../BONES/src/demo/test_house.h"
#include "../../BONES/modules/eye/eye_modkit_ext.h"
#include "../../BONES/modules/skin/skin_modkit.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

extern int32_t bones_mod_eye_query(uint32_t,BonesModDescriptor *);
extern int32_t bones_mod_skin_query(uint32_t,BonesModDescriptor *);
static BonesFoundation foundation;
static uint32_t composite[EYE_DISPLAY_PIXELS];
static uint32_t base_copy[EYE_DISPLAY_PIXELS];

static void snapshot(const char *path) {
    FILE *f=fopen(path,"wb");
    unsigned i;
    assert(f);
    assert(fprintf(f,"P6\n320 200\n255\n")>0);
    for(i=0;i<EYE_DISPLAY_PIXELS;i++) {
        uint32_t p=composite[i];
        unsigned char rgb[3]={(unsigned char)(p>>16),(unsigned char)(p>>8),(unsigned char)p};
        assert(fwrite(rgb,1,3,f)==3u);
    }
    assert(fclose(f)==0);
}
static void compose(EyeDisplayMode mode,unsigned dim,const char *path) {
    const uint32_t *eye=bones_eye_mod_pixels();
    uint64_t hash_before=UINT64_C(14695981039346656037),hash_after=hash_before;
    EyeFrameStats stats;uint64_t serial;
    unsigned i,diff=0;
    assert(eye!=NULL);
    for(i=0;i<EYE_DISPLAY_PIXELS;i++) {
        for(unsigned b=0;b<4;b++) {
            hash_before^=(uint8_t)(eye[i]>>(b*8));
            hash_before*=UINT64_C(1099511628211);
        }
    }
    assert(bones_eye_mod_prepare_base(composite,EYE_DISPLAY_PIXELS,mode,dim));
    memcpy(base_copy,composite,sizeof(composite));
    assert(bones_skin_ui_compose(composite,320,200,320)==BONES_MOD_OK);
    for(i=0;i<EYE_DISPLAY_PIXELS;i++) {
        if(composite[i]!=base_copy[i])++diff;
        assert((composite[i]>>24)==255u);
        for(unsigned b=0;b<4;b++) {
            hash_after^=(uint8_t)(eye[i]>>(b*8));
            hash_after*=UINT64_C(1099511628211);
        }
    }
    assert(diff>100u); /* Real Skin alpha composition has a visible effect. */
    assert(hash_before==hash_after); /* No destructive write to Eye depth/pixels. */
    assert(bones_eye_mod_frame_stats(&stats,&serial));
    assert(serial>=1u && !stats.all_black && stats.nonopaque==0u);
    snapshot(path);
    fprintf(stderr,"%s: serial=%llu black=%u clear=%u art=%u overlays=%u hash=%016llx\n",
        path,(unsigned long long)serial,stats.black_rgb,stats.clear_rgb,
        stats.other_rgb,diff,(unsigned long long)stats.hash);
}
int main(int argc,char **argv) {
    BonesModule house;
    BonesModHost *host;
    BonesInput no_input={0};
    const char *dir=argc>1?argv[1]:".";
    char path[1024];
    float initial_x,initial_z;
    uint32_t ticks=0;
    assert(bones_foundation_init(&foundation,UINT64_C(0x1440001),UINT64_C(0x01442026),1));
    house=bones_test_house_module();
    assert(bones_register_module(&foundation.engine,&house));
    host=bones_mod_host_create(&foundation);
    assert(host!=NULL);
    assert(bones_mod_host_add(host,bones_mod_skin_query)==BONES_MOD_OK);
    assert(bones_mod_host_add(host,bones_mod_eye_query)==BONES_MOD_OK);
    assert(bones_mod_host_start(host)==BONES_MOD_OK);
    assert(bones_mod_host_frame(host)==BONES_MOD_OK);
    snprintf(path,sizeof(path),"%s/eye_mode_generated_skin.ppm",dir);
    compose(EYE_DISPLAY_MENU_GENERATED,0,path);
    assert(bones_skin_ui_input(SKIN2_ACCEPT)==SKIN2_START);
    snprintf(path,sizeof(path),"%s/eye_mode_game_skin.ppm",dir);
    compose(EYE_DISPLAY_GAME,0,path);
    initial_x=foundation.engine.player.feet.x;initial_z=foundation.engine.player.feet.z;
    no_input.jump=1;
    for(unsigned i=0;i<3u;i++){
        assert(bones_mod_host_step(host,&no_input)==BONES_MOD_OK);
        no_input.jump=0;ticks++;
    }
    assert(foundation.engine.player.feet.x==initial_x);
    assert(foundation.engine.player.feet.z==initial_z);
    assert(ticks==3u);
    assert(bones_mod_host_frame(host)==BONES_MOD_OK);
    assert(bones_skin_ui_input(SKIN2_BACK)==SKIN2_NO_ACTION);
    snprintf(path,sizeof(path),"%s/eye_mode_dim_skin.ppm",dir);
    compose(EYE_DISPLAY_MENU_DIM_SCENE,128,path);
    bones_mod_host_destroy(host);
    fprintf(stderr,"real Bones+Eye+Skin three-mode integration: PASS\n");
    return 0;
}
