/* Verifies the real game-facing path: embedded bytes -> RAM -> discarded -> regenerated. */
#include "eye4.h"
#include "materiales_eye4_embebidos.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
static uint32_t arena[2*4096u];
static uint32_t snapshot[4096u];
int main(void){
    Eye4Bank bank;Eye4Cache cache;const uint32_t *p;
    assert(eye4_read(eye4_builtin_pack,EYE4_BUILTIN_PACK_SIZE,&bank));
    assert(bank.count==8u);
    assert(eye4_cache_init(&cache,&bank,arena,8192u,2u));
    p=eye4_cache_get(&cache,2u);assert(p);memcpy(snapshot,p,sizeof snapshot);
    assert(eye4_cache_get(&cache,0u));
    assert(eye4_cache_get(&cache,1u)); /* evicts id 2 */
    p=eye4_cache_get(&cache,2u);assert(p);
    assert(!memcmp(snapshot,p,sizeof snapshot));
    printf("PASS embedded EYE4: %zu bytes, LRU discard/regenerate, hash=%016llx\n",
           (size_t)EYE4_BUILTIN_PACK_SIZE,
           (unsigned long long)eye_hash_pixels(p,4096u));
    return 0;
}
