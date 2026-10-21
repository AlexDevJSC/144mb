#if !defined(_WIN32)
#include "../../BONES/include/bones/modkit.h"
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <assert.h>
int main(int argc,char **argv){
    void *h,*sym;BonesDynamicQuery q;BonesModDescriptor d={0};
    if(argc!=2)return 2;
    h=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!h){fprintf(stderr,"dlopen: %s\n",dlerror());return 3;}
    sym=dlsym(h,"bones_mod_query_v1");
    assert(sym && sizeof(sym)==sizeof(q));
    memcpy(&q,&sym,sizeof(q));
    d.size=sizeof(d);
    assert(q(BONES_MOD_ABI,&d)==BONES_MOD_OK);
    assert(d.role==BONES_ROLE_EAR && strcmp(d.id,"ear")==0);
    dlclose(h);
    puts("PASS EAR .so exports bones_mod_query_v1");
    return 0;
}
#else
int main(void){return 0;}
#endif
