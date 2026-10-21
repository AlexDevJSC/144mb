#include "eye3.h"
#include "eye_bones_bridge.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static EyeTextureBank bank;
static EyeRecipe recipes[16], readback[16];
static uint8_t packed[EYE_PACK_MAX_BYTES];
static uint32_t pixels[EYE_SIZE*EYE_SIZE];
static uint32_t again[EYE_SIZE*EYE_SIZE];
static uint32_t cached[2u*EYE_SIZE*EYE_SIZE];

int main(void) {
    unsigned i,x,y,n=999u;
    EyeMaterialCache cache;
    size_t bytes;
    const uint32_t *ptr;
    for(i=0;i<EYE3_FAMILY_COUNT;++i) {
        EyeRecipe r={(uint8_t)i,(uint8_t)(i%4u),1u,(uint8_t)(i%8u),
                     10u,5u,13u,0u,UINT32_C(0x1a440301)+i};
        assert(eye3_generate(&r,pixels,4096u));
        assert(eye3_generate(&r,again,4096u));
        assert(!memcmp(pixels,again,sizeof pixels));
        for(y=0;y<64u;++y) for(x=0;x<64u;++x) {
            assert(eye3_pixel(&r,(int)x,(int)y)==eye3_pixel(&r,(int)x+64,(int)y-64));
            assert((eye3_pixel(&r,(int)x,(int)y)>>24)==255u);
        }
    }
    for(i=0;i<16u;++i) {
        recipes[i]=(EyeRecipe){(uint8_t)(i%11u),(uint8_t)(i%4u),
            (uint8_t)(1u+i%4u),(uint8_t)(i%8u),(uint8_t)(4u+i%12u),
            (uint8_t)(i%14u),12u,0u,UINT32_C(0x1a440340)+i};
    }
    bytes=eye3_pack_write(recipes,16u,packed,sizeof packed);
    assert(bytes==204u && packed[3]=='3');
    assert(eye_bank_open(&bank,packed,bytes));
    assert(bank.version==3u && bank.count==16u);
    assert(eye3_pack_read(packed,bytes,readback,16u,&n) && n==16u);
    assert(eye_bank_render(&bank,12u,pixels,4096u));
    assert(eye3_generate(&recipes[12],again,4096u));
    assert(!memcmp(pixels,again,sizeof pixels));
    assert(!eye_bank_render(&bank,16u,pixels,4096u));
    /* EYE2 decoder and bank must not silently reinterpret EYE3. */
    assert(!eye2_pack_read(packed,bytes,readback,16u,&n));
    { uint8_t copy[EYE_PACK_MAX_BYTES];
      memcpy(copy,packed,bytes);
      copy[3]='2';
      assert(!eye_bank_open(&bank,copy,bytes));
      copy[3]='3';copy[26]^=1u;
      assert(!eye_bank_open(&bank,copy,bytes));
      assert(!eye3_pack_read(copy,bytes,readback,16u,&n));
      assert(readback[0].seed==recipes[0].seed); /* no partial mutation */
    }
    assert(eye_bank_open(&bank,packed,bytes));
    assert(eye_cache_init(&cache,&bank,cached,2u*4096u,2u));
    assert(!eye_cache_get(&cache,16u));
    ptr=eye_cache_get(&cache,0u);
    assert(ptr && cache.misses==1u && cache.hits==0u);
    assert(eye_cache_get(&cache,0u)==ptr && cache.hits==1u);
    assert(eye_cache_get(&cache,1u) && cache.misses==2u);
    assert(eye_cache_get(&cache,0u)==ptr); /* cache slot 0 stays newer */
    assert(eye_cache_get(&cache,2u) && cache.misses==3u);
    assert(eye_cache_get(&cache,0u)==ptr); /* slot 1 evicted */
    assert(eye_cache_get(&cache,1u) && cache.misses==4u);
    assert(!eye_cache_init(&cache,&bank,cached,4096u,2u));
    assert(!eye_cache_init(&cache,&bank,cached,2u*4096u,0u));
    assert(!eye_cache_init(&cache,&bank,cached,2u*4096u,17u));
    assert(!eye_cache_init(&cache,&bank,NULL,2u*4096u,2u));
    assert(eye3_generate(&recipes[8],pixels,4096u));
    assert(eye_hash_pixels(pixels,4096u)==UINT64_C(0x30b5d99dd2751a38));
    assert(eye_uv_sample_repeat(pixels,0,0)==pixels[0]);
    assert(eye_uv_sample_repeat(pixels,65536,65536)==pixels[0]);
    assert(eye_uv_sample_repeat(pixels,-65536,-65536)==pixels[0]);
    assert(eye_uv_sample_repeat(pixels,-1024,-1024)==pixels[63u*64u+63u]);
    assert(eye_uv_sample_repeat(pixels,32768,32768)==pixels[32u*64u+32u]);
    assert(eye_uv_sample_repeat(NULL,0,0)==0u);
    {
      unsigned ids[3]={5u,8u,11u};
      static uint32_t legacy[3u*4096u];
      assert(eye_bones_legacy_fill(&bank,ids,legacy,3u*4096u));
      assert(eye_bank_render(&bank,8u,again,4096u));
      assert(!memcmp(legacy+4096u,again,sizeof again));
    }
    printf("PASS EYE3: 11 families, 64px seamless tiling, EYE1/EYE2 isolation, EYE3 hash, LRU cache, UV and bridge\n");
    printf("eye3_roof_hash=%016" PRIx64 " sample_pack_bytes=%zu\n",eye_hash_pixels(pixels,4096u),bytes);
    return 0;
}
