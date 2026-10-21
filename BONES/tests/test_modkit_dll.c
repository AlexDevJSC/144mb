#include "../include/bones/modkit_host.h"
#include <assert.h>
#include <stdio.h>
static BonesFoundation f;
int main(int argc,char **argv) {
    BonesModHost *h;
    assert(argc==2);
    assert(bones_foundation_init(&f,111,555,1));
    h=bones_mod_host_create(&f);assert(h);
    assert(bones_mod_host_load(h,argv[1])==0);
    assert(bones_mod_host_count(h)==1);
    assert(bones_mod_host_start(h)==0);
    assert(bones_mod_host_step(h,NULL)==0);
    assert(bones_mod_host_frame(h)==0);
    bones_mod_host_destroy(h);
    puts("PASS ModKit: dynamic module load, step, frame, unload");
    return 0;
}
