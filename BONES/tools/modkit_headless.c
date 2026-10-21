#include "../include/bones/modkit_host.h"
#include "../src/demo/test_house.h"
#include "../third_party/eye/sw_renderer.h"
#include <stdio.h>
#include <stdint.h>
static BonesFoundation f;
extern const uint32_t *bones_eye_mod_pixels(void);
int main(int argc,char **argv) {
    BonesModHost *host;BonesModule house;const uint32_t *pixels;
    FILE *out;uint32_t i;
    const char *path=argc>1?argv[1]:"build/modkit_preview.ppm";
    if(!bones_foundation_init(&f,UINT64_C(0x1440001),UINT64_C(0x01442026),1))return 1;
    house=bones_test_house_module();
    if(!bones_register_module(&f.engine,&house))return 2;
    host=bones_mod_host_create(&f);
    if(!host)return 3;
    if(bones_mod_register_builtins(host)!=0||bones_mod_host_start(host)!=0||bones_mod_host_frame(host)!=0)return 4;
    pixels=bones_eye_mod_pixels();if(!pixels)return 5;
    out=fopen(path,"wb");if(!out)return 6;
    fprintf(out,"P6\n%d %d\n255\n",P144_VIEW_WIDTH,P144_VIEW_HEIGHT);
    for(i=0;i<P144_VIEW_WIDTH*P144_VIEW_HEIGHT;i++){
        uint32_t c=pixels[i];unsigned char b[3]={(unsigned char)(c>>16),(unsigned char)(c>>8),(unsigned char)c};
        if(fwrite(b,1,3,out)!=3){fclose(out);return 7;}
    }
    fclose(out);bones_mod_host_destroy(host);
    printf("ModKit Eye frame: %s\n",path);
    return 0;
}
