#include "eye4.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t p[EYE4_MAX_PACKET],bad[EYE4_MAX_PACKET];
static uint32_t pixels[4096],again[4096],arena[4096*2];
int main(void) {
    Eye4Recipe r[8];Eye4Bank bank={0};Eye4Cache cache;size_t n;
    for(int method=0;method<3;++method) {
        eye4_demo_recipes(r,(Eye4Method)method);
        for(unsigned i=0;i<8;++i) {
            assert(eye4_valid(&r[i]));
            assert(eye4_generate(&r[i],pixels,4096));
            assert(eye4_generate(&r[i],again,4096));
            assert(!memcmp(pixels,again,sizeof pixels));
            if(method==2 && i==0) {
                uint64_t h=eye_hash_pixels(pixels,4096);
                printf("hash_muro_hibrido=%016llx\n",(unsigned long long)h);
            }
        }
        n=eye4_write(r,8,p,sizeof(p));assert(n==140u);
        assert(eye4_read(p,n,&bank)&&bank.count==8u);
        assert(bank.recipe[0].method==(unsigned)method);
        memcpy(bad,p,n);bad[0]='X';assert(!eye4_read(bad,n,&bank));
        memcpy(bad,p,n);bad[22]^=1u;assert(!eye4_read(bad,n,&bank));
        memcpy(bad,p,n);bad[3]='3';assert(!eye4_read(bad,n,&bank));
        assert(!eye4_read(p,n-1,&bank));
        assert(eye4_cache_init(&cache,&bank,arena,8192,2));
        assert(eye4_cache_get(&cache,0));assert(eye4_cache_get(&cache,1));
        assert(eye4_cache_get(&cache,0));assert(cache.hits==1);
        assert(eye4_cache_get(&cache,2));
        assert(eye4_cache_get(&cache,1)); /* evicted, regenerate */
        assert(cache.misses==4);
        assert(!eye4_cache_get(&cache,55));
        assert(!eye4_cache_init(&cache,&bank,arena,4096,2));
    }
    puts("PASS EYE4: three paths, deterministic, versioned codec, corruption, LRU eviction");
    return 0;
}
