#include "../include/bones/foundation.h"
#include "../src/adapters/eye_adapter.h"
#include "../src/demo/test_house.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static BonesFoundation foundation;
#define engine (foundation.engine)
static BonesEye eye;
static int write_ppm(const char *file) {
    FILE *f=fopen(file,"wb");
    uint32_t i;
    if(!f) {perror(file); return 0;}
    fprintf(f,"P6\n%d %d\n255\n",P144_VIEW_WIDTH,P144_VIEW_HEIGHT);
    for(i=0;i<P144_VIEW_WIDTH*P144_VIEW_HEIGHT;i++) {
        uint32_t c=eye.renderer.pixels[i];
        unsigned char rgb[3]={(unsigned char)(c>>16),(unsigned char)(c>>8),(unsigned char)c};
        if(fwrite(rgb,1,3,f)!=3) {fclose(f);return 0;}
    }
    fclose(f); return 1;
}
int main(int argc,char **argv) {
    BonesInput in={0};
    unsigned i;
    const char *output=argc>1 ? argv[1] : "build/bones_preview.ppm";
    if(!bones_foundation_init(&foundation,UINT64_C(0x1440001),UINT64_C(0x01442026),1)) return 11;
    {
        BonesModule m=bones_test_house_module();
        if(!bones_register_module(&engine,&m)) return 8;
    }
    bones_eye_init(&eye,UINT32_C(0x01442026));
    if(!bones_register_project_modules(&foundation,&eye)) return 7;
    if(!bones_foundation_render_frame(&foundation)) return 5;
    if(!write_ppm(output)) return 6;
    /* Camera-only showcase; do NOT change player position or physics. */
    {
        BonesScene elevated;
        bones_make_scene(&engine,&elevated);
        elevated.camera.position=(BonesVec3){4.0f,4.55f,-4.8f};
        elevated.camera.yaw=-0.43f;
        elevated.camera.pitch=-0.18f;
        if(!bones_draw_scene(&elevated,&eye.backend)) return 9;
        if(!write_ppm("build/bones_tejado.ppm")) return 10;
    }
    /* Walk 1s, turn and walk 1s, jump once. */
    in.forward=1;
    for(i=0;i<60;i++) if(!bones_foundation_step(&foundation,&in)) return 1;
    in.yaw_right=1;
    for(i=0;i<60;i++) if(!bones_foundation_step(&foundation,&in)) return 2;
    in.yaw_right=0; in.forward=0; in.jump=1;
    if(!bones_foundation_step(&foundation,&in)) return 3;
    in.jump=0;
    for(i=0;i<18;i++) if(!bones_foundation_step(&foundation,&in)) return 4;
    if(!bones_foundation_render_frame(&foundation)) return 5;
    if(!write_ppm("build/bones_movimiento.ppm")) return 6;
    printf("Bones+Eye: %llu ticks, pos=(%.3f,%.3f,%.3f), quads=%u, triangles=%u, frame=%s\n",
        (unsigned long long)engine.tick,engine.player.feet.x,engine.player.feet.y,
        engine.player.feet.z, (unsigned)engine.world.collider_count,
        (unsigned)eye.renderer.triangles, output);
    bones_stop(&engine);
    return 0;
}
