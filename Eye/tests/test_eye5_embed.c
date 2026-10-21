#include "eye5.h"
#include "recetas_eye5_embebidas.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t slot[4096],saved[4096];
int main(void){
    Eye5Bank bank;Eye5Cache cache;
    assert(EYE5_EMBEDDED_BYTES==60u);
    assert(eye5_read(eye5_recipes_embedded,EYE5_EMBEDDED_BYTES,&bank));
    assert(bank.count==4u);
    assert(eye5_cache_init(&cache,&bank,slot,4096u,1u));
    assert(eye5_cache_get(&cache,0u));memcpy(saved,slot,sizeof(slot));
    assert(eye5_cache_get(&cache,1u));
    assert(eye5_cache_get(&cache,0u));assert(memcmp(saved,slot,sizeof(slot))==0);
    assert(cache.misses==3u);
    puts("PASS EYE5 embed: packed recipes in executable, in-memory eviction/regeneration");
    return 0;
}
