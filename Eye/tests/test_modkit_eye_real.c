/* Compile against the ACTUAL Bones 0.4.0 ABI1 and sw_renderer.c:
 * does not start Bones core/Win32; this tests Eye's module callbacks and
 * renderer with stable camera/near-plane samples (without changing physics). */
#include "../../BONES/include/bones/modkit.h"
#include "../../BONES/modules/eye/eye_modkit_ext.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int32_t bones_mod_eye_query(uint32_t,BonesModDescriptor *);
static int32_t add_quad_stub(void *ctx,const BonesModQuad *q) {
    (void)ctx;return (q&&q->size==sizeof(*q))?BONES_MOD_OK:BONES_MOD_EINVAL;
}
static int measure(const uint32_t *pixels,float *cx,float *cy) {
    double sx=0,sy=0;int n=0; int left=320,right=-1,top=200,bottom=-1;
    for(unsigned y=0;y<200u;++y)for(unsigned x=0;x<320u;++x) {
        uint32_t c=pixels[y*320u+x]&0xffffffu;
        if(c!=0x15202du && c!=0xe5dabeu){ sx+=x;sy+=y;++n; if((int)x<left)left=(int)x; if((int)x>right)right=(int)x; if((int)y<top)top=(int)y; if((int)y>bottom)bottom=(int)y;}
    }
    fprintf(stderr,"MEASURE n=%d bbox=[%d,%d,%d,%d] centroid=%.2f,%.2f\
",n,left,top,right,bottom,n?sx/n:0.0,n?sy/n:0.0);
    if(n){*cx=(float)(sx/n);*cy=(float)(sy/n);}
    return n;
}
int main(void) {
    BonesModDescriptor d={0};BonesModServices svc={0};
    BonesModFrame f={0};BonesModQuad q={0};EyeFrameStats stats;
    uint32_t *out=malloc(EYE_DISPLAY_PIXELS*sizeof(uint32_t));
    uint64_t serial;float x0,y0,x1,y1,x2,y2,x3,y3;
    assert(out);d.size=sizeof(d);
    assert(bones_mod_eye_query(BONES_MOD_ABI,&d)==BONES_MOD_OK);
    assert(d.role==BONES_ROLE_EYE && d.present && d.start && d.stop);
    svc.size=sizeof(svc);svc.abi=BONES_MOD_ABI;
    svc.permissions=BONES_PERMISSION_WRITE_SCENE;svc.scene_add_quad=add_quad_stub;
    assert(d.start(d.user_context,&svc)==BONES_MOD_OK);
    assert(bones_eye_mod_prepare_base(out,EYE_DISPLAY_PIXELS,EYE_DISPLAY_MENU_GENERATED,0));
    assert(!bones_eye_mod_prepare_base(out,EYE_DISPLAY_PIXELS,EYE_DISPLAY_GAME,0));
    f.size=sizeof(f);f.camera_z=0;
    d.present(d.user_context,&f);
    assert(bones_eye_mod_frame_stats(&stats,&serial));
    assert(serial==1 && !stats.all_black && stats.other_rgb==16);
    q.size=sizeof(q);q.material_id=0;
    q.vertices[0]=(BonesModVertex){.x=-0.5f,.y=-0.5f,.z=6,.u=0,.v=0};
    q.vertices[1]=(BonesModVertex){.x= 0.5f,.y=-0.5f,.z=6,.u=1,.v=0};
    q.vertices[2]=(BonesModVertex){.x= 0.5f,.y= 0.5f,.z=6,.u=1,.v=1};
    q.vertices[3]=(BonesModVertex){.x=-0.5f,.y= 0.5f,.z=6,.u=0,.v=1};
    f.quads=&q;f.quad_count=1;
    d.present(d.user_context,&f);
    assert(bones_eye_mod_frame_stats(&stats,&serial));
    assert(stats.other_rgb>16);
    assert(measure(bones_eye_mod_pixels(),&x0,&y0)>100);
    f.camera_yaw=.12f;
    d.present(d.user_context,&f);
    assert(measure(bones_eye_mod_pixels(),&x1,&y1)>100);
    fprintf(stderr,"Yaw probe: x0=%.3f y0=%.3f x1=%.3f y1=%.3f (delta=%.3f)\
", x0,y0,x1,y1,x1-x0);
    assert(x1 < x0-5.f); /* View moves right, fixed wall shifts left. */
    f.camera_yaw=0;f.camera_pitch=.12f;
    d.present(d.user_context,&f);
    assert(measure(bones_eye_mod_pixels(),&x2,&y2)>100);
    assert(y2 > y0+5.f); /* Looking up, fixed wall moves down. */
    f.camera_pitch=0;f.camera_y=.25f;
    d.present(d.user_context,&f);
    assert(measure(bones_eye_mod_pixels(),&x3,&y3)>100);
    assert(fabsf(x3-x0)<1.f); /* Vertical jump from rest, level camera: no lateral drift. */
    assert(y3 > y0+4.f);
    f.camera_y=0;f.camera_z=10.f; /* Quad now behind near plane. */
    d.present(d.user_context,&f);
    assert(bones_eye_mod_frame_stats(&stats,&serial));
    assert(stats.other_rgb==16 && !stats.all_black);
    assert(bones_eye_mod_prepare_base(out,EYE_DISPLAY_PIXELS,EYE_DISPLAY_MENU_DIM_SCENE,110));
    d.stop(d.user_context);
    assert(!bones_eye_mod_prepare_base(out,EYE_DISPLAY_PIXELS,EYE_DISPLAY_MENU_GENERATED,0));
    free(out);puts("Eye+REAL Bones renderer ModKit callback test: PASS");return 0;
}
