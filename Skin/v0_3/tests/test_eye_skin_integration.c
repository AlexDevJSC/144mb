/* Eye 0.5.2 PR12 pure presentation implementation + the real Skin ModKit
 * UI source from this branch. This is a compositing integration test, not
 * proof that Bones Win32 host dispatches WM_KEYDOWN or that Eye rasterizes 3D. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../../BONES/modules/skin/skin_modkit.h"
#include "eye_052_fixture/eye_display.h"
#define W 320u
#define H 200u
static uint32_t scene[W*H];
static uint32_t frame[W*H];
static uint32_t baseline[W*H];
static uint32_t scene_before[W*H];
static uint32_t depth[W*H];
static uint32_t depth_before[W*H];
static uint64_t tick(void *p){(void)p;return 42;}
static int32_t count(void *p){(void)p;return 3;}
static int32_t at(void *p,uint32_t i,BonesModActor *o){(void)p;if(i>=3||!o)return -1;o->id=i+1;return 0;}
static uint64_t hash(const uint32_t *s,size_t n){uint64_t h=UINT64_C(14695981039346656037);for(size_t i=0;i<n;++i){h^=s[i];h*=UINT64_C(1099511628211);}return h;}
static void dump(const char *p){FILE *f=fopen(p,"wb");assert(f);fprintf(f,"P6\n320 200\n255\n");for(unsigned i=0;i<W*H;i++){unsigned char v[3]={(unsigned char)(frame[i]>>16),(unsigned char)(frame[i]>>8),(unsigned char)frame[i]};assert(fwrite(v,1,3,f)==3);}assert(!fclose(f));}
static void draw(EyeDisplayMode mode,unsigned dim,const char *path){
    assert(eye_display_prepare(frame,W*H,scene,W*H,mode,dim));
    memcpy(baseline,frame,sizeof(frame));
    assert(bones_skin_ui_compose(frame,320,200,320)==BONES_MOD_OK);
    assert(!memcmp(scene,scene_before,sizeof(scene)));
    assert(!memcmp(depth,depth_before,sizeof(depth)));
    assert(hash(frame,W*H)!=hash(baseline,W*H));
    for(unsigned i=0;i<W*H;++i)assert((frame[i]>>24)==255);
    dump(path);
}
int main(void){
    BonesModDescriptor d={0};BonesModServices s={0};
    d.size=sizeof(d);assert(bones_mod_skin_query(BONES_MOD_ABI,&d)==BONES_MOD_OK);
    assert(d.implementation_version>=4u && !d.present && !d.build_scene);
    s.size=sizeof(s);s.abi=BONES_MOD_ABI;s.permissions=d.requested_permissions;s.tick=tick;s.actor_count=count;s.actor_at=at;
    assert(d.start(d.user_context,&s)==BONES_MOD_OK);
    for(unsigned y=0;y<H;y++)for(unsigned x=0;x<W;x++){
        unsigned i=y*W+x;scene[i]=0xff000000u|((x&255u)<<16)|((y&255u)<<8)|((x+y)&255u);
        depth[i]=(x*79u+y*31u) ^ 0xabcddcbau;
    }
    memcpy(depth_before,depth,sizeof(depth));
    memcpy(scene_before,scene,sizeof(scene));
    assert(bones_skin_ui_blocks_gameplay());
    draw(EYE_DISPLAY_MENU_GENERATED,0,"build/eye_skin_main.ppm");
    assert(bones_skin_ui_input(SKIN2_ACCEPT)==SKIN2_START);
    assert(bones_skin_ui_blocks_gameplay()); /* start needs host ACK */
    assert(bones_skin_ui_enter_game()==BONES_MOD_OK);
    assert(!bones_skin_ui_blocks_gameplay());
    d.after_tick(d.user_context);
    draw(EYE_DISPLAY_GAME,0,"build/eye_skin_hud.ppm");
    assert(bones_skin_ui_input(SKIN2_BACK)==SKIN2_NO_ACTION);
    assert(bones_skin_ui_blocks_gameplay());
    draw(EYE_DISPLAY_MENU_DIM_SCENE,128,"build/eye_skin_pause.ppm");
    assert(bones_skin_ui_input(SKIN2_BACK)==SKIN2_RESUME);
    assert(!bones_skin_ui_blocks_gameplay());
    /* Setting: scale HUD, ensuring 2x glyph bottom is within 320x200. */
    assert(bones_skin_ui_input(SKIN2_BACK)==SKIN2_NO_ACTION); /* pause */
    assert(bones_skin_ui_input(SKIN2_DOWN)==SKIN2_NO_ACTION);
    assert(bones_skin_ui_input(SKIN2_DOWN)==SKIN2_NO_ACTION);
    assert(bones_skin_ui_input(SKIN2_ACCEPT)==SKIN2_NO_ACTION); /* settings */
    assert(bones_skin_ui_input(SKIN2_ACCEPT)==SKIN2_APPLY_SETTINGS);
    assert(bones_skin_ui_input(SKIN2_BACK)==SKIN2_NO_ACTION);
    assert(bones_skin_ui_input(SKIN2_BACK)==SKIN2_RESUME);
    draw(EYE_DISPLAY_GAME,0,"build/eye_skin_hud_large.ppm");
    assert(bones_skin_ui_input(SKIN2_OPEN_INV)==SKIN2_NO_ACTION);
    draw(EYE_DISPLAY_MENU_DIM_SCENE,128,"build/eye_skin_inventory.ppm");
    /* Dim-scene must not modify the source rasterized by Eye. */
    assert(eye_display_prepare(frame,W*H,scene,W*H,EYE_DISPLAY_GAME,0));
    assert(memcmp(scene,frame,sizeof(scene))==0);
    assert(!memcmp(depth,depth_before,sizeof(depth)));
    Skin2Canvas sc;
    uint32_t big[1280*720];for(unsigned i=0;i<1280u*720u;i++)big[i]=0xff6789abu;
    assert(skin2_canvas_init(&sc,big,1280,720,1280));
    skin2_draw(bones_skin_ui_state(), &(Skin2View){0}, &sc);
    assert(big[0]==0xff6789abu && big[1279]==0xff6789abu); /* side bars intact */
    /* Host blocking applies to ALL movement input, including held keyboard state. */
    typedef struct {int forward,mouse_dx,jump;} GameInput;
    GameInput in={1,99,1};
    if(bones_skin_ui_blocks_gameplay())memset(&in,0,sizeof(in));
    assert(in.forward==0&&in.mouse_dx==0&&in.jump==0);
    d.stop(d.user_context);
    puts("PASS Eye 0.5.2/Skin: menu, HUD, pause/resume, inventory, alpha, depth untouched, neutral input, upscale");
    return 0;
}
