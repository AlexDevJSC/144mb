/* Each cell is a canonical 64x64 decoded Eye4 material. No artistic postprocessing. */
#include "eye4.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#define W (8*64)
#define H (3*64)
static uint32_t sheet[W*H];
static uint32_t buf[4096];
int main(int argc,char **argv){
    Eye4Recipe r[8];FILE *f;
    if(argc!=2)return 2;
    for(int method=0;method<3;++method){
        eye4_demo_recipes(r,(Eye4Method)method);
        for(int m=0;m<8;++m){
            if(!eye4_generate(&r[m],buf,4096))return 3;
            for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)
                sheet[(size_t)(method*64+(int)y)*W+(size_t)m*64u+x]=buf[y*64u+x];
        }
    }
    f=fopen(argv[1],"wb");if(!f)return 4;
    fprintf(f,"P6\n%d %d\n255\n",W,H);
    for(unsigned i=0;i<W*H;++i){
        uint8_t rgb[3]={(uint8_t)(sheet[i]>>16),(uint8_t)(sheet[i]>>8),(uint8_t)sheet[i]};
        if(fwrite(rgb,1,3,f)!=3){fclose(f);return 5;}
    }
    return fclose(f)==0?0:6;
}
