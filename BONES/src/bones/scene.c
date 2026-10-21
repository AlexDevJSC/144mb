#include "bones_internal.h"
#include <string.h>

bool bones_scene_add_quad(BonesScene *s, const BonesQuad *q) {
    if (!s || !q || s->quad_count >= BONES_MAX_QUADS || q->material > BONES_MAT_WOOD) return false;
    s->quads[s->quad_count++] = *q;
    return true;
}
bool bones_scene_add_sprite(BonesScene *s, const BonesSprite *b) {
    if (!s || !b || s->sprite_count >= BONES_MAX_SPRITES) return false;
    s->sprites[s->sprite_count++] = *b;
    return true;
}
static BonesVertex vertex(float x,float y,float z,float u,float v) {
    BonesVertex t = {{x,y,z},u,v}; return t;
}
static void quad(BonesScene *s, uint8_t m, BonesVertex a, BonesVertex b, BonesVertex c, BonesVertex d) {
    BonesQuad q = {{a,b,c,d},m};
    (void)bones_scene_add_quad(s,&q);
}
static void box(BonesScene *s, const BonesCollider *c) {
    float x0=c->bounds.min.x, x1=c->bounds.max.x;
    float y0=c->bounds.min.y, y1=c->bounds.max.y;
    float z0=c->bounds.min.z, z1=c->bounds.max.z;
    uint8_t m=c->material;
    float u=x1-x0, v=z1-z0, h=y1-y0;
    quad(s,m,vertex(x0,y0,z0,0,h),vertex(x1,y0,z0,u,h),vertex(x1,y1,z0,u,0),vertex(x0,y1,z0,0,0));
    quad(s,m,vertex(x1,y0,z1,0,h),vertex(x0,y0,z1,u,h),vertex(x0,y1,z1,u,0),vertex(x1,y1,z1,0,0));
    quad(s,m,vertex(x0,y0,z1,0,h),vertex(x0,y0,z0,v,h),vertex(x0,y1,z0,v,0),vertex(x0,y1,z1,0,0));
    quad(s,m,vertex(x1,y0,z0,0,h),vertex(x1,y0,z1,v,h),vertex(x1,y1,z1,v,0),vertex(x1,y1,z0,0,0));
    quad(s,m,vertex(x0,y1,z0,0,0),vertex(x1,y1,z0,u,0),vertex(x1,y1,z1,u,v),vertex(x0,y1,z1,0,v));
}
void bones_make_scene(const BonesEngine *e, BonesScene *s) {
    uint32_t i;
    if (!e || !s) return;
    memset(s,0,sizeof(*s));
    s->camera.position = e->player.feet;
    s->camera.position.y += e->player.eye_height;
    s->camera.yaw = e->player.yaw;
    s->camera.pitch = e->player.pitch;
    /* Floor and ceiling are non-colliding render planes at the physics bounds. */
    quad(s,BONES_MAT_FLOOR,vertex(-8,0,-12,0,0),vertex(8,0,-12,8,0),
         vertex(8,0,12,8,12),vertex(-8,0,12,0,12));
    quad(s,BONES_MAT_WOOD,vertex(-8,5,-12,0,0),vertex(8,5,-12,8,0),
         vertex(8,5,12,8,12),vertex(-8,5,12,0,12));
    for (i=0;i<e->world.collider_count;++i)
        if (e->world.colliders[i].render_box) box(s,&e->world.colliders[i]);
    {
        BonesSprite npc = {{0.0f,0.0f,7.0f}, 1.8f,2.65f,(uint32_t)e->tick};
        (void)bones_scene_add_sprite(s,&npc);
    }
    for (i=0;i<e->module_count;++i)
        if (e->modules[i].on_scene) e->modules[i].on_scene(e->modules[i].user_data,e,s);
    if (e->subsystem_started) {
        for (i=0;i<e->active_subsystems;++i) {
            const BonesSubsystem *m=&e->subsystems[e->subsystem_order[i]];
            if (m->build_scene) m->build_scene(m->context,e,s);
        }
    }
}
bool bones_draw_scene(const BonesScene *scene, const BonesRenderBackend *r) {
    uint32_t i;
    if (!scene || !r || r->version != BONES_API_VERSION || !r->begin ||
        !r->draw_quad || !r->draw_sprite || !r->end) return false;
    r->begin(r->user_data,&scene->camera);
    for (i=0;i<scene->quad_count;i++) r->draw_quad(r->user_data,&scene->camera,&scene->quads[i]);
    for (i=0;i<scene->sprite_count;i++) r->draw_sprite(r->user_data,&scene->camera,&scene->sprites[i]);
    r->end(r->user_data);
    return true;
}

bool bones_render(const BonesEngine *e, const BonesRenderBackend *r) {
    BonesScene scene;
    if (!e) return false;
    bones_make_scene(e, &scene);
    return bones_draw_scene(&scene, r);
}
