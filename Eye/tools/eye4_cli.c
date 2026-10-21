#include "eye4.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv) {
    uint8_t packet[EYE4_MAX_PACKET];Eye4Recipe recipes[8];Eye4Bank bank;
    size_t n;FILE *f;int m;
    if(argc<3) {fputs("Eye4: build 0|1|2 output.eyep | inspect input.eyep | embed input.eyep output.h\n",stderr);return 2;}
    if(!strcmp(argv[1],"build")&&argc==4) {
        if(strlen(argv[2])!=1||argv[2][0]<'0'||argv[2][0]>'2')return 2;
        m=argv[2][0]-'0';eye4_demo_recipes(recipes,(Eye4Method)m);
        n=eye4_write(recipes,8,packet,sizeof packet);
        f=fopen(argv[3],"wb");if(!f)return 3;
        if(fwrite(packet,1,n,f)!=n||fclose(f))return 4;
        printf("EYE4 (%d): %zu bytes, 8 recipes\n",m,n);return 0;
    }
    if((!strcmp(argv[1],"inspect")&&argc==3)||(!strcmp(argv[1],"embed")&&argc==4)){
        f=fopen(argv[2],"rb");if(!f)return 3;
        n=fread(packet,1,sizeof packet,f);
        if(fgetc(f)!=EOF){fclose(f);return 4;}
        fclose(f);if(!eye4_read(packet,n,&bank))return 5;
        if(!strcmp(argv[1],"inspect")) {
            printf("EYE4 count=%u total=%zu bytes, recipe=16 bytes\n",bank.count,n);
            for(unsigned i=0;i<bank.count;++i)printf("[%u] motif=%u method=%u seed=%08x\n",
              i,bank.recipe[i].motif,bank.recipe[i].method,bank.recipe[i].base.seed);
            return 0;
        }
        f=fopen(argv[3],"wb");if(!f)return 3;
        fprintf(f,"#ifndef EYE4_BUILTIN_PACK_H\n#define EYE4_BUILTIN_PACK_H\n#include <stdint.h>\n");
        fprintf(f,"static const uint8_t eye4_builtin_pack[%zu] = {\n",n);
        for(size_t i=0;i<n;++i)fprintf(f,"%s0x%02x%s",(i%12u==0)?"  ":" ",packet[i],
           (i+1u<n)?",":"");
        fprintf(f,"\n};\n#define EYE4_BUILTIN_PACK_SIZE %zu\n#endif\n",n);
        return fclose(f)==0?0:4;
    }
    return 2;
}
