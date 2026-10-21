/* Eye demo provider: bridges ModKit frame to legacy Eye renderer.
   Eye's team can replace implementation without editing Bones. */
#include "../../include/bones/modkit.h"
#include "../../src/adapters/eye_adapter.h"
#include <string.h>
typedef struct {BonesEye eye;int initialized;} EyePort;
static EyePort state; /* static provider: only one host until Eye supplies own context */
static int32_t start(void *ctx,const BonesModServices *services) {
    EyePort *e=(EyePort*)ctx;
    if(services->abi!=BONES_MOD_ABI)return BONES_MOD_EINVAL;
    bones_eye_init(&e->eye,UINT32_C(0x01442026));e->initialized=1;return BONES_MOD_OK;
}
static void stop(void *ctx){((EyePort*)ctx)->initialized=0;}
static void present(void *ctx,const BonesModFrame *frame) {
    BonesScene scene;uint32_t i,j;EyePort *e=(EyePort*)ctx;
    if(!e->initialized||!frame||frame->size!=sizeof(*frame)||
       frame->quad_count>BONES_MAX_QUADS||frame->sprite_count>BONES_MAX_SPRITES)return;
    memset(&scene,0,sizeof(scene));scene.quad_count=frame->quad_count;scene.sprite_count=frame->sprite_count;
    scene.camera.position=(BonesVec3){frame->camera_x,frame->camera_y,frame->camera_z};
    scene.camera.yaw=frame->camera_yaw;scene.camera.pitch=frame->camera_pitch;
    for(i=0;i<frame->quad_count;i++) {
        const BonesModQuad *q=&frame->quads[i];BonesQuad *b=&scene.quads[i];
        if(q->material_id>2)continue;
        b->material=(uint8_t)q->material_id;
        for(j=0;j<4;j++) {
            b->vertices[j].position=(BonesVec3){q->vertices[j].x,q->vertices[j].y,q->vertices[j].z};
            b->vertices[j].u=q->vertices[j].u;b->vertices[j].v=q->vertices[j].v;
        }
    }
    for(i=0;i<frame->sprite_count;i++) {
        scene.sprites[i].feet=(BonesVec3){frame->sprites[i].x,frame->sprites[i].y,frame->sprites[i].z};
        scene.sprites[i].width=frame->sprites[i].width;
        scene.sprites[i].height=frame->sprites[i].height;
        scene.sprites[i].animation_tick=frame->sprites[i].animation_tick;
    }
    (void)bones_draw_scene(&scene,&e->eye.backend);
}
int32_t bones_mod_eye_query(uint32_t abi,BonesModDescriptor *out) {
    if(abi!=BONES_MOD_ABI||!out||out->size!=sizeof(*out))return BONES_MOD_EINVAL;
    memset(out,0,sizeof(*out));out->size=sizeof(*out);out->abi=abi;
    memcpy(out->id,"eye",4);out->implementation_version=1;out->role=BONES_ROLE_EYE;
    out->requested_permissions=BONES_PERMISSION_READ_TIME|BONES_PERMISSION_READ_ACTORS|BONES_PERMISSION_WRITE_SCENE;
    out->user_context=&state;out->start=start;out->stop=stop;out->present=present;
    return BONES_MOD_OK;
}
/* Win32 sample can access pixels via Eye-owned adapter, not engine internals. */
const uint32_t *bones_eye_mod_pixels(void){return state.initialized?state.eye.renderer.pixels:NULL;}
uint32_t bones_eye_mod_triangles(void){return state.initialized?state.eye.renderer.triangles:0;}
