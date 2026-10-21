/* Compile against REAL Bones ModKit header + Eye's vendored code/renderer.
 * This is a provider integration test, not a mock API emulation. */
#include "../../BONES/include/bones/modkit.h"
#include "../../BONES/modules/eye/eye_modkit_ext.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
extern int32_t bones_mod_eye_query(uint32_t,BonesModDescriptor*);
static unsigned count;
static int32_t accept(void *ctx,const BonesModQuad *q) {
    (void)ctx;
    assert(q && q->size==sizeof(*q) && q->material_id<=2u && !q->flags);
    ++count;
    return BONES_MOD_OK;
}
int main(void) {
    BonesModDescriptor d={0};d.size=sizeof(d);
    assert(bones_mod_eye_query(BONES_MOD_ABI,&d)==BONES_MOD_OK);
    assert(strcmp(d.id,"eye")==0 && d.implementation_version==501u);
    assert(d.role==BONES_ROLE_EYE && d.state_schema_version==0u);
    assert(d.present && d.build_scene && d.start && d.stop);
    BonesModServices svc={0};svc.size=sizeof(svc);svc.abi=BONES_MOD_ABI;
    svc.permissions=BONES_PERMISSION_WRITE_SCENE;svc.scene_add_quad=accept;
    assert(d.start(d.user_context,&svc)==BONES_MOD_OK);
    assert(bones_eye_mod_pixels()!=NULL);
    assert(bones_eye_mod_roof_texture_preview(1));
    assert(bones_eye_mod_roof_texture_preview(0));
    Eye5RoofSpec spec={2.f,2.f,2.5f,1.4f,.25f,EYE5_GABLE};
    assert(!bones_eye_mod_roof_preview(&spec,0,0,4,3));
    assert(bones_eye_mod_roof_preview(&spec,0,0,4,2));
    d.build_scene(d.user_context);assert(count==9u);
    spec.form=EYE5_HIP;
    assert(bones_eye_mod_roof_preview(&spec,0,0,4,2));
    d.build_scene(d.user_context);assert(count==22u);
    assert(bones_eye_mod_roof_preview(NULL,0,0,0,0));
    d.build_scene(d.user_context);assert(count==22u);
    BonesModQuad q={0};q.size=sizeof(q);q.material_id=0;
    for(unsigned i=0;i<4u;++i)q.vertices[i]=(BonesModVertex){0.f,0.f,2.f,(float)(i&1u),(float)(i>>1u)};
    BonesModFrame f={0};f.size=sizeof(f);f.quad_count=1;f.quads=&q;
    d.present(d.user_context,&f);
    assert(bones_eye_mod_pixels()!=NULL);
    const uint32_t *pixels=bones_eye_mod_pixels();
    uint64_t sum=0;for(unsigned i=0;i<320u*200u;++i)sum+=pixels[i];
    assert(sum!=0);
    d.stop(d.user_context);assert(bones_eye_mod_pixels()==NULL);
    puts("Eye 0.5.1 / Bones 0.4 ModKit: provider test PASS");
    return 0;
}
