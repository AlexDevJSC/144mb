/* Compile against the ACTUAL Bones 0.4.0 ABI1 and sw_renderer.c:
 * does not start Bones core/Win32; this tests Eye's module callbacks and
 * renderer with stable camera/near-plane samples (without changing physics). */
#include "../../BONES/include/bones/modkit.h"
#include "../../BONES/modules/eye/eye_modkit_ext.h"
#include "../../BONES/third_party/eye/sw_renderer.h"
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
    double sx=0,sy=0;int n=0;
    for(unsigned y=0;y<200u;++y)for(unsigned x=0;x<320u;++x) {
        uint32_t c=pixels[y*320u+x]&0xffffffu;
        if(c!=0x15202du && c!=0xe5dabeu){sx+=x;sy+=y;++n;}
    }
    if(n){*cx=(float)(sx/n);*cy=(float)(sy/n);}
    return n;
}
int main(void) {
    BonesModDescriptor d={0};BonesModServices svc={0};
    BonesModFrame f={0};BonesModQuad q={0};EyeFrameStats stats;
    uint32_t *out=malloc(EYE_DISPLAY_PIXELS*sizeof(uint32_t));
    uint64_t serial, hash0, hashYaw;float x0,y0,x1,y1,x2,y2,x3,y3;
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
    assert(stats.other_rgb>16); hash0=stats.hash;
    assert(measure(bones_eye_mod_pixels(),&x0,&y0)>100);
    f.camera_yaw=.12f;
    d.present(d.user_context,&f);
    assert(bones_eye_mod_frame_stats(&stats,&serial)); hashYaw=stats.hash;
    fprintf(stderr,"FRAME yaw=%f serial=%llu hashBefore=%llx hashAfter=%llx\n",
        f.camera_yaw,(unsigned long long)serial,(unsigned long long)hash0,(unsigned long long)hashYaw);
    assert(measure(bones_eye_mod_pixels(),&x1,&y1)>100);
    assert(hash0!=hashYaw); /* Camera change must affect image pixels. */
    /* Direct rasterizer baseline prevents false positives caused by the adapter. */
    {
        P144_Renderer *direct=(P144_Renderer*)malloc(sizeof(P144_Renderer));
        P144_Camera cam={0};
        P144_Quad probe={0};
        float dx0,dy0,dx1,dy1;
        assert(direct);
        for(unsigned i=0;i<4u;i++) {
            probe.v[i].pos=(P144_Vec3){q.vertices[i].x,q.vertices[i].y,q.vertices[i].z};
            probe.v[i].u=q.vertices[i].u;probe.v[i].v=q.vertices[i].v;
        }
        p144_renderer_init(direct,0x01442026u);
        p144_renderer_begin(direct,0x15202du);
        p144_renderer_quad(direct,&cam,&probe,P144_MAT_STONE);
        p144_renderer_crosshair(direct);
        assert(measure(direct->pixels,&dx0,&dy0)>100);
        cam.yaw=.12f;
        p144_renderer_begin(direct,0x15202du);
        p144_renderer_quad(direct,&cam,&probe,P144_MAT_STONE);
        p144_renderer_crosshair(direct);
        assert(measure(direct->pixels,&dx1,&dy1)>100);
        assert(dx1 < dx0-5.f);
        free(direct);
    }
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
