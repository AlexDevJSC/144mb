#include "eye_adapter.h"
#include <string.h>
static P144_Camera camera_eye(const BonesCamera *c) {
    P144_Camera cam = {{c->position.x,c->position.y,c->position.z}, c->yaw,c->pitch};
    return cam;
}
static void eye_begin(void *user, const BonesCamera *cam) {
    BonesEye *eye = (BonesEye *)user;
    (void)cam;
    p144_renderer_begin(&eye->renderer,0x15202du);
}
static void eye_quad(void *user,const BonesCamera *c,const BonesQuad *b) {
    P144_Camera cam=camera_eye(c);
    P144_Quad q;
    BonesEye *eye=(BonesEye *)user;
    unsigned i;
    for(i=0;i<4;i++) {
        q.v[i].pos=(P144_Vec3){b->vertices[i].position.x,b->vertices[i].position.y,b->vertices[i].position.z};
        q.v[i].u=b->vertices[i].u; q.v[i].v=b->vertices[i].v;
    }
    /* Maps Bones test material identifiers onto Eye's current material set. */
    p144_renderer_quad(&eye->renderer,&cam,&q,(P144_Material)b->material);
}
static void eye_sprite(void *user,const BonesCamera *c,const BonesSprite *s) {
    P144_Camera cam=camera_eye(c);
    P144_Vec3 foot={s->feet.x,s->feet.y,s->feet.z};
    BonesEye *eye=(BonesEye *)user;
    p144_renderer_billboard(&eye->renderer,&cam,foot,s->width,s->height,s->animation_tick);
}
static void eye_end(void *user) {
    BonesEye *eye=(BonesEye *)user;
    p144_renderer_crosshair(&eye->renderer);
}
void bones_eye_init(BonesEye *eye,uint32_t seed) {
    if(!eye) return;
    memset(eye,0,sizeof(*eye));
    p144_renderer_init(&eye->renderer,seed);
    eye->backend.version=BONES_API_VERSION;
    eye->backend.user_data=eye;
    eye->backend.begin=eye_begin;
    eye->backend.draw_quad=eye_quad;
    eye->backend.draw_sprite=eye_sprite;
    eye->backend.end=eye_end;
}

unsigned bones_eye_load_textures(BonesEye *eye, BonesEyeTextureGenerator generator,
                                 void *context) {
    /* One temporary shared tile (16 KiB), never one buffer per surface.
       Failed generators cannot corrupt the existing cache. */
    uint32_t tile[P144_TEX_SIZE * P144_TEX_SIZE];
    unsigned id,loaded=0;
    if (!eye || !generator) return 0;
    for (id=0;id<P144_MAT_COUNT;++id) {
        if (generator(context,id,eye->renderer.seed,tile,P144_TEX_SIZE,P144_TEX_SIZE)) {
            memcpy(eye->renderer.tex[id],tile,sizeof(tile));
            ++loaded;
        }
    }
    return loaded;
}

static void eye_present_scene(void *ctx, const BonesScene *scene) {
    BonesEye *eye=(BonesEye *)ctx;
    (void)bones_draw_scene(scene,&eye->backend);
}
BonesSubsystem bones_eye_subsystem(BonesEye *eye) {
    BonesSubsystem module = {0};
    module.version=BONES_SUBSYSTEM_API_VERSION;
    module.struct_size=(uint32_t)sizeof(module);
    module.slot=BONES_SLOT_EYE;
    module.context=eye;
    module.present=eye_present_scene;
    return module;
}
