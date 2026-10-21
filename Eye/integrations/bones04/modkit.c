/* Eye 0.5.1 -> Bones 0.4 ModKit ABI 1: SOURCE-LINKED edition.
 * Build script automatically compiles only modules/eye/modkit.c.
 * Unity inclusion below incorporates the actual Eye implementations;
 * DO NOT add eye*.c separately to the same link (duplicate symbols).
 * Does not include bones.h or inspect BonesEngine. No core changes.
 */
#include "../../include/bones/modkit.h"
#include "../../third_party/eye/sw_renderer.h"
#include "eye4.h"
#include "eye5.h"
#include "eye_display.h"
#include <string.h>

typedef struct {
    P144_Renderer renderer;
    const BonesModServices *services;
    Eye5RoofMesh roof;
    float roof_x, roof_y, roof_z;
    unsigned roof_material;
    unsigned roof_enabled;
    unsigned started;
    EyeFrameStats frame_stats;
    uint64_t frames_rendered;
} EyeModkit;
static EyeModkit eye_state;

/* Legacy identity must remain stable: 0=stone/wall, 1=floor, 2=wood.
 * A roof tile ID 3 is NOT legal under Bones 0.4's scene validator.
 * Roof preview swaps ALL wooden surfaces; it is opt-in, NOT gameplay. */
static int prepare_textures(EyeModkit *s, unsigned roof_preview) {
    Eye4Recipe art[8];
    Eye5Recipe roofs[4];
    eye4_demo_recipes(art,EYE4_HYBRID);
    eye5_demo_recipes(roofs);
    if (!eye4_generate(&art[EYE4_PANEL],s->renderer.tex[0],EYE5_TILE_PIXELS) ||
        !eye4_generate(&art[EYE4_ROAD],s->renderer.tex[1],EYE5_TILE_PIXELS)) return 0;
    if (roof_preview) {
        if (!eye5_generate(&roofs[EYE5_WOOD],s->renderer.tex[2],EYE5_TILE_PIXELS))return 0;
    } else {
        if (!eye4_generate(&art[EYE4_DOOR],s->renderer.tex[2],EYE5_TILE_PIXELS))return 0;
    }
    return 1;
}
static int32_t eye_start(void *context, const BonesModServices *svc) {
    EyeModkit *s=(EyeModkit *)context;
    if(!s||!svc||svc->abi!=BONES_MOD_ABI||svc->size!=sizeof(*svc))return BONES_MOD_EINVAL;
    if(!(svc->permissions & BONES_PERMISSION_WRITE_SCENE) || !svc->scene_add_quad)return BONES_MOD_DENIED;
    p144_renderer_init(&s->renderer,UINT32_C(0x01442026));
    if(!prepare_textures(s,0u))return BONES_MOD_BAD_STATE;
    s->services=svc;
    s->frames_rendered=0u;
    memset(&s->frame_stats,0,sizeof(s->frame_stats));
    s->started=1u;
    return BONES_MOD_OK;
}
static void eye_stop(void *context) {
    EyeModkit *s=(EyeModkit *)context;
    if(s){s->started=0u;s->services=NULL;s->roof_enabled=0u;}
}

/* Optional source-linked demo request, from the host application or a test.
 * The game must authorize any roof placement. Eye does not create collision.
 * A null spec disables the overlay. The geometry is cached, not rebuilt
 * every frame. IDs >2 are rejected until Bones+Soul approve an extension. */
int bones_eye_mod_roof_preview(const Eye5RoofSpec *spec,
                               float x,float y,float z,unsigned material_id) {
    Eye5RoofMesh next;
    if(!spec){eye_state.roof_enabled=0u;return 1;}
    if(material_id>2u||!eye5_build_roof(spec,&next))return 0;
    eye_state.roof=next;
    eye_state.roof_x=x;eye_state.roof_y=y;eye_state.roof_z=z;
    eye_state.roof_material=material_id;eye_state.roof_enabled=1u;
    return 1;
}
/* Optional THREE-SLOT material preview; roof shingles use ID2 globally.
 * Call only between frames. Restore 0 for legacy semantics. */
int bones_eye_mod_roof_texture_preview(int enabled) {
    return eye_state.started && prepare_textures(&eye_state,enabled?1u:0u);
}
static void eye_build_scene(void *context) {
    EyeModkit *s=(EyeModkit*)context;
    if(!s||!s->started||!s->roof_enabled||!s->services)return;
    for(unsigned i=0;i<s->roof.count;++i) {
        const Eye5Surface *source=&s->roof.surface[i];
        BonesModQuad quad;
        memset(&quad,0,sizeof(quad));quad.size=sizeof(quad);
        quad.material_id=source->role==EYE5_SURFACE_TRIM?2u:s->roof_material;
        for(unsigned k=0;k<4u;++k) {
            const Eye5Vertex *p=&source->vertex[k<source->vertex_count?k:2u];
            quad.vertices[k]=(BonesModVertex){p->x+s->roof_x,p->y+s->roof_y,p->z+s->roof_z,p->u,p->v};
        }
        if(s->services->scene_add_quad(s->services->host_context,&quad)!=BONES_MOD_OK)break;
    }
}
static void eye_present(void *context,const BonesModFrame *frame) {
    EyeModkit *s=(EyeModkit*)context;
    P144_Camera camera;
    if(!s||!s->started||!frame||frame->size!=sizeof(*frame)||
       frame->quad_count>BONES_MOD_QUAD_LIMIT||frame->sprite_count>BONES_MOD_SPRITE_LIMIT||
       (frame->quad_count&&!frame->quads)||(frame->sprite_count&&!frame->sprites))return;
    camera.pos=(P144_Vec3){frame->camera_x,frame->camera_y,frame->camera_z};
    camera.yaw=frame->camera_yaw;camera.pitch=frame->camera_pitch;
    p144_renderer_begin(&s->renderer,UINT32_C(0x15202d));
    for(uint32_t i=0;i<frame->quad_count;++i) {
        const BonesModQuad *src=&frame->quads[i];
        P144_Quad quad;
        if(src->size!=sizeof(*src)||src->material_id>2u||src->flags!=0u)continue;
        for(unsigned j=0;j<4u;++j)
            quad.v[j]=(P144_Vertex){{src->vertices[j].x,src->vertices[j].y,src->vertices[j].z},
                                     src->vertices[j].u,src->vertices[j].v};
        p144_renderer_quad(&s->renderer,&camera,&quad,(P144_Material)src->material_id);
    }
    for(uint32_t i=0;i<frame->sprite_count;++i) {
        const BonesModSprite *sp=&frame->sprites[i];
        if(sp->size!=sizeof(*sp)||sp->width<=0.f||sp->height<=0.f)continue;
        p144_renderer_billboard(&s->renderer,&camera,
           (P144_Vec3){sp->x,sp->y,sp->z},sp->width,sp->height,sp->animation_tick);
    }
    p144_renderer_crosshair(&s->renderer);
    (void)eye_display_scan(s->renderer.pixels,EYE_DISPLAY_PIXELS,&s->frame_stats);
    ++s->frames_rendered;
}
int32_t bones_mod_eye_query(uint32_t abi,BonesModDescriptor *out) {
    if(abi!=BONES_MOD_ABI||!out||out->size!=sizeof(*out))return BONES_MOD_EINVAL;
    memset(out,0,sizeof(*out));out->size=sizeof(*out);out->abi=abi;
    memcpy(out->id,"eye",4);out->implementation_version=502u;
    out->state_schema_version=0u;out->role=BONES_ROLE_EYE;
    out->requested_permissions=BONES_PERMISSION_WRITE_SCENE;
    out->user_context=&eye_state;out->start=eye_start;out->stop=eye_stop;
    out->build_scene=eye_build_scene;out->present=eye_present;
    return BONES_MOD_OK;
}
const uint32_t *bones_eye_mod_pixels(void) {
    return eye_state.started?eye_state.renderer.pixels:NULL;
}
uint32_t bones_eye_mod_triangles(void) {
    return eye_state.started?eye_state.renderer.triangles:0u;
}

/* The host owns its output buffer; Skin can then call bones_skin_ui_compose()
 * on that buffer. Eye never draws Skin UI or updates game/menu state. */
int bones_eye_mod_prepare_base(uint32_t *dst,size_t capacity,
                               EyeDisplayMode mode,unsigned dim) {
    const uint32_t *scene=(eye_state.started && eye_state.frames_rendered)?
                           eye_state.renderer.pixels:NULL;
    if(!eye_state.started)return 0;
    return eye_display_prepare(dst,capacity,scene,scene?EYE_DISPLAY_PIXELS:0u,
                               mode,dim);
}
int bones_eye_mod_frame_stats(EyeFrameStats *out,uint64_t *serial) {
    if(!eye_state.started||!out||!eye_state.frames_rendered)return 0;
    *out=eye_state.frame_stats;
    if(serial)*serial=eye_state.frames_rendered;
    return 1;
}

/* Unity build: Bones 0.4's built-in module collector includes only modkit.c.
 * Renaming private file-local helpers resolves name collisions between
 * historical Eye generators without changing their source or test hashes. */
#include "eye.c"
#include "eye_tone.c"
#define get32 eye2_get32
#define put32 eye2_put32
#include "eye2.c"
#undef get32
#undef put32
#define read32 eye3_read32
#include "eye3.c"
#undef read32
#include "eye4.c"
#define get32 eye5_get32
#define put32 eye5_put32
#include "eye5.c"
#undef get32
#undef put32

#include "eye_display.c"
