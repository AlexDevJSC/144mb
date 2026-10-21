/* Optional REAL Eye1/Eye2 integration test. Does not alter Eye or Bones.
   Build only when Eye sources are present in the SAME checkout.
   BVR1 is NOT a final Bones save format. */
#include "brain/brain_visual.h"
#include "eye2.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define V1 1u  /* Lab mapping only: Eye pack magic "EYE1". Not catalog ABI. */
#define V2 2u  /* Lab mapping only: Eye pack magic "EYE2". Not catalog ABI. */

int main(void) {
    EyeRecipe recipe={0};
    EyeTextureBank bank={0};
    uint8_t eye1[EYE_HEADER_BYTES+EYE_RECIPE_BYTES];
    uint8_t eye2[EYE_HEADER_BYTES+EYE_RECIPE_BYTES];
    uint8_t bvr[ BRAIN_VISUAL_HEADER_BYTES + BRAIN_VISUAL_RECORD_BYTES ];
    uint32_t pixels1[EYE_SIZE*EYE_SIZE], pixels1_again[EYE_SIZE*EYE_SIZE];
    uint32_t pixels2[EYE_SIZE*EYE_SIZE];
    BrainVisualContext context={{UINT64_C(2),UINT64_C(3)},UINT64_C(456),1u,V1};
    BrainVisualOverride store[1], restored[1], over={0};
    BrainVisualSet set={0}, decoded={0};
    BrainVisualRef base={0}, result={0};
    size_t written=0;
    const uint32_t available[]={V1,V2};
    uint64_t hash1, hash2;

    recipe.family=EYE_STONE;
    recipe.palette=2;
    recipe.scale=3;
    recipe.pattern=5;
    recipe.wear=10;
    recipe.growth=8;
    recipe.contrast=12;
    recipe.seed=UINT32_C(0x1440);
    assert(eye_valid(&recipe));
    assert(eye_pack_write(&recipe,1,eye1,sizeof(eye1))==sizeof(eye1));
    assert(eye2_pack_write(&recipe,1,eye2,sizeof(eye2))==sizeof(eye2));
    assert(memcmp(eye1,"EYE1",4)==0 && memcmp(eye2,"EYE2",4)==0);
    assert(eye_bank_open(&bank,eye1,sizeof(eye1)));
    assert(bank.count==1 && bank.version==V1);
    assert(eye_bank_render(&bank,0,pixels1,EYE_SIZE*EYE_SIZE));
    assert(eye_bank_render(&bank,0,pixels1_again,EYE_SIZE*EYE_SIZE));
    assert(memcmp(pixels1,pixels1_again,sizeof(pixels1))==0);
    hash1=eye_hash_pixels(pixels1,EYE_SIZE*EYE_SIZE);
    assert(eye_bank_open(&bank,eye2,sizeof(eye2)));
    assert(bank.count==1 && bank.version==V2);
    assert(eye_bank_render(&bank,0,pixels2,EYE_SIZE*EYE_SIZE));
    hash2=eye_hash_pixels(pixels2,EYE_SIZE*EYE_SIZE);
    assert(hash1!=hash2); /* These algorithms are intentionally different. */

    assert(brain_visual_init(&set,context,store,1)==BRAIN_VIS_OK);
    over.key.object_id=UINT64_C(0x7001);
    over.key.surface_id=2;
    over.state=BRAIN_VIS_STATE_SCORCHED;
    over.flags=BRAIN_VIS_OVERRIDE_VARIANT;
    over.variant_override=9; /* NOT a currently defined Eye recipe index. */
    over.cause.branch=1;
    over.cause.serial=7;
    assert(brain_visual_upsert(&set,over)==BRAIN_VIS_OK);
    assert(brain_visual_encode(&set,bvr,sizeof(bvr),&written)==BRAIN_VIS_OK);
    assert(written==sizeof(bvr));
    assert(brain_visual_decode(&decoded,context,bvr,written,restored,1)==BRAIN_VIS_OK);
    base.material_id=0;base.variant_id=0;base.eye_generator_version=V1;
    assert(brain_visual_resolve(&decoded,over.key,base,&result)==BRAIN_VIS_OK);
    assert(result.material_id==0 && result.variant_id==9 &&
           result.state==BRAIN_VIS_STATE_SCORCHED);
    base.eye_generator_version=V2;
    assert(brain_visual_resolve(&decoded,over.key,base,&result)==BRAIN_VIS_ERR_EYE_VERSION);
    assert(brain_visual_check_eye_version(V1,available,2)==BRAIN_VIS_OK);
    assert(brain_visual_check_eye_version(3u,available,2)==BRAIN_VIS_ERR_EYE_VERSION);

    puts("PASS REAL EYE1/EYE2: versioned pack, pixel generation, stable pixels, BVR1 logical roundtrip, incompatible version rejected");
    printf("Eye pixel hashes: %llu (EYE1), %llu (EYE2)\n",
           (unsigned long long)hash1,(unsigned long long)hash2);
    return 0;
}
