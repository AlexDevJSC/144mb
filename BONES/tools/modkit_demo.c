#include "../include/bones/modkit_host.h"
#include "../src/demo/test_house.h"
#include <stdio.h>
static BonesFoundation foundation; /* runtime objects not allocated on main stack */
int main(void) {
    BonesModHost *host;BonesModule house;BonesInput input={0};unsigned i;
    if(!bones_foundation_init(&foundation,UINT64_C(0x1440001),UINT64_C(0x01442026),1))return 1;
    house=bones_test_house_module();
    if(!bones_register_module(&foundation.engine,&house))return 2;
    host=bones_mod_host_create(&foundation);
    if(!host)return 3;
    if(bones_mod_register_builtins(host)!=BONES_MOD_OK||bones_mod_host_start(host)!=BONES_MOD_OK)return 4;
    input.forward=1;
    for(i=0;i<60;i++)if(bones_mod_host_step(host,&input)!=BONES_MOD_OK)return 5;
    if(bones_mod_host_frame(host)!=BONES_MOD_OK)return 6;
    printf("Bones ModKit: %u registered modules, tick=%llu, player=(%.2f %.2f %.2f)\n",
           bones_mod_host_count(host),(unsigned long long)foundation.engine.tick,
           foundation.engine.player.feet.x,foundation.engine.player.feet.y,foundation.engine.player.feet.z);
    bones_mod_host_destroy(host);return 0;
}
