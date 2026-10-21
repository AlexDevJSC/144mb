#include "brain/brain_visual.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST_EYE1_ID UINT32_C(1001) /* TEST IDs ONLY; Eye has not assigned IDs. */
#define TEST_EYE2_ID UINT32_C(1002)
#define TEST_BUFFER_SIZE (BRAIN_VISUAL_HEADER_BYTES + 4u * BRAIN_VISUAL_RECORD_BYTES)

static uint8_t serialized_a[TEST_BUFFER_SIZE];
static uint8_t serialized_b[TEST_BUFFER_SIZE];
static BrainVisualOverride entries_a[4], entries_b[4], entries_decoded[4];

static BrainVisualSurfaceKey surface(int64_t sx, int64_t sy, uint64_t oid, uint32_t slot) {
    BrainVisualSurfaceKey k = {sx, sy, oid, slot};
    return k;
}
static BrainVisualOverride damage(BrainVisualSurfaceKey key, uint8_t state,
                                  uint64_t serial) {
    BrainVisualOverride o;
    memset(&o, 0, sizeof o);
    o.key = key; o.state = state; o.cause = (BrainEventId){42, serial};
    return o;
}
/* Mock Eye generator: no pixel buffer, no rendering, version-sensitive by design.
   Eye must replace this mock and document its real recipe/version algorithm. */
static BrainVisualRef fake_eye_recipe(BrainVisualContext c, BrainVisualSurfaceKey k) {
    uint64_t n = c.world_seed ^ c.world_id.lo ^
                 (uint64_t)k.sector_x * UINT64_C(0x9e3779b97f4a7c15) ^
                 (uint64_t)k.sector_y * UINT64_C(0xbf58476d1ce4e5b9) ^
                 k.object_id ^ ((uint64_t)k.surface_id << 32) ^
                 (uint64_t)c.eye_generator_version * UINT64_C(0x94d049bb133111eb);
    n ^= n >> 30; n *= UINT64_C(0xbf58476d1ce4e5b9);
    n ^= n >> 27; n *= UINT64_C(0x94d049bb133111eb);
    n ^= n >> 31;
    return (BrainVisualRef){(uint32_t)(1 + (n % 5u)), (uint32_t)(1 + (n % 700u)),
                             c.eye_generator_version, BRAIN_VIS_STATE_DEFAULT};
}
static void test_roundtrip(void) {
    BrainVisualContext c = {{2, 999}, UINT64_C(0x7766554433221100), 3, TEST_EYE1_ID};
    BrainVisualContext c_eye2 = c;
    BrainVisualSet a, b, decoded = {0};
    BrainVisualSurfaceKey burned_key = surface(-3, 19, UINT64_C(901), 5);
    BrainVisualSurfaceKey broken_key = surface(-3, 19, UINT64_C(902), 1);
    BrainVisualSurfaceKey clean_key = surface(5, -10, UINT64_C(12345), 77);
    BrainVisualOverride burned = damage(burned_key, BRAIN_VIS_STATE_SCORCHED, 15);
    BrainVisualOverride broken = damage(broken_key, BRAIN_VIS_STATE_BROKEN, 16);
    BrainVisualOverride missing;
    BrainVisualRef base_eye1, base_eye2, r;
    size_t n1 = 0, n2 = 0;
    uint32_t supported1[1] = {TEST_EYE1_ID};
    uint32_t supported2[2] = {TEST_EYE1_ID, TEST_EYE2_ID};
    int i;

    assert(brain_visual_init(&a, c, entries_a, 4) == BRAIN_VIS_OK);
    assert(brain_visual_init(&b, c, entries_b, 4) == BRAIN_VIS_OK);
    /* No generated-only visual material is inserted into persistent overrides. */
    assert(a.count == 0);
    assert(brain_visual_resolve(&a, clean_key, fake_eye_recipe(c, clean_key), &r) == BRAIN_VIS_OK);
    assert(r.variant_id == fake_eye_recipe(c, clean_key).variant_id);
    assert(a.count == 0);

    broken.flags = BRAIN_VIS_OVERRIDE_MATERIAL | BRAIN_VIS_OVERRIDE_VARIANT;
    broken.material_override = 55; broken.variant_override = 777;
    assert(brain_visual_upsert(&a, broken) == BRAIN_VIS_OK);
    assert(brain_visual_upsert(&a, burned) == BRAIN_VIS_OK);
    assert(brain_visual_upsert(&a, burned) == BRAIN_VIS_DUPLICATE);
    assert(brain_visual_upsert(&b, burned) == BRAIN_VIS_OK);
    assert(brain_visual_upsert(&b, broken) == BRAIN_VIS_OK);
    assert(brain_visual_encode(&a, serialized_a, sizeof serialized_a, &n1) == BRAIN_VIS_OK);
    assert(brain_visual_encode(&b, serialized_b, sizeof serialized_b, &n2) == BRAIN_VIS_OK);
    assert(n1 == BRAIN_VISUAL_HEADER_BYTES + 2 * BRAIN_VISUAL_RECORD_BYTES);
    assert(n1 == n2 && memcmp(serialized_a, serialized_b, n1) == 0);
    assert(memcmp(serialized_a, "BVR1", 4) == 0);
    /* Test an actual binary file roundtrip. The library itself does no I/O. */
    {
        uint8_t from_disk[TEST_BUFFER_SIZE] = {0};
        FILE *temp = tmpfile();
        assert(temp);
        assert(fwrite(serialized_a, 1, n1, temp) == n1);
        assert(fseek(temp, 0L, SEEK_SET) == 0);
        assert(fread(from_disk, 1, n1, temp) == n1);
        assert(fgetc(temp) == EOF);
        assert(fclose(temp) == 0);
        assert(memcmp(from_disk, serialized_a, n1) == 0);
        assert(brain_visual_decode(&decoded, c, from_disk, n1,
                                    entries_decoded, 4) == BRAIN_VIS_OK);
    }
    printf("PASS: registro canonico de %zu bytes, solo 2 cambios de mundo\n", n1);

    assert(brain_visual_decode(&decoded, c, serialized_a, n1,
                                entries_decoded, 4) == BRAIN_VIS_OK);
    assert(decoded.count == 2);
    assert(brain_visual_resolve(&decoded, burned_key, fake_eye_recipe(c, burned_key), &r) == BRAIN_VIS_OK);
    assert(r.state == BRAIN_VIS_STATE_SCORCHED);
    assert(brain_visual_resolve(&decoded, broken_key, fake_eye_recipe(c, broken_key), &r) == BRAIN_VIS_OK);
    assert(r.state == BRAIN_VIS_STATE_BROKEN && r.material_id == 55 && r.variant_id == 777);
    assert(brain_visual_find(&decoded, clean_key, &missing) == BRAIN_VIS_ERR_NOT_FOUND);
    assert(brain_visual_resolve(&decoded, clean_key, fake_eye_recipe(c, clean_key), &r) == BRAIN_VIS_OK);
    assert(r.state == BRAIN_VIS_STATE_DEFAULT);
    assert(r.material_id == fake_eye_recipe(c, clean_key).material_id);
    assert(r.variant_id == fake_eye_recipe(c, clean_key).variant_id);

    /* EYE1 and EYE2 differ even when world seed + surface key remain identical. */
    c_eye2.eye_generator_version = TEST_EYE2_ID;
    base_eye1 = fake_eye_recipe(c, clean_key);
    base_eye2 = fake_eye_recipe(c_eye2, clean_key);
    assert(base_eye1.eye_generator_version != base_eye2.eye_generator_version);
    assert(base_eye1.variant_id != base_eye2.variant_id ||
           base_eye1.material_id != base_eye2.material_id);
    assert(brain_visual_resolve(&decoded, clean_key, base_eye2, &r) == BRAIN_VIS_ERR_EYE_VERSION);
    assert(brain_visual_decode(&decoded, c_eye2, serialized_a, n1,
                                entries_decoded, 4) == BRAIN_VIS_ERR_EYE_VERSION);
    assert(decoded.count == 2); /* No partial state on failure. */
    assert(brain_visual_check_eye_version(TEST_EYE1_ID, &supported1[0], 1) == BRAIN_VIS_OK);
    assert(brain_visual_check_eye_version(TEST_EYE1_ID, &supported2[0], 2) == BRAIN_VIS_OK);
    assert(brain_visual_check_eye_version(TEST_EYE2_ID, &supported1[0], 1) == BRAIN_VIS_ERR_EYE_VERSION);

    { BrainVisualContext wrong = c; wrong.world_id.lo++;
      assert(brain_visual_decode(&decoded, wrong, serialized_a, n1,
                                 entries_decoded, 4) == BRAIN_VIS_ERR_WORLD);
      wrong = c; wrong.world_generator_version++;
      assert(brain_visual_decode(&decoded, wrong, serialized_a, n1,
                                 entries_decoded, 4) == BRAIN_VIS_ERR_WORLD);
    }
    assert(brain_visual_decode(&decoded, c, serialized_a, n1 - 1,
                                entries_decoded, 4) == BRAIN_VIS_ERR_MALFORMED);
    assert(brain_visual_encode(&a, serialized_b, n1 - 1, &n2) == BRAIN_VIS_ERR_CAPACITY);
    assert(brain_visual_decode(&decoded, c, serialized_a, n1,
                                entries_decoded, 1) == BRAIN_VIS_ERR_CAPACITY);
    /* A single flipped bit must fail the integrity check. */
    memcpy(serialized_b, serialized_a, n1);
    serialized_b[n1 - 1] ^= 0x08u;
    assert(brain_visual_decode(&decoded, c, serialized_b, n1,
                                entries_decoded, 4) == BRAIN_VIS_ERR_MALFORMED);
    assert(decoded.count == 2);

    /* ID reuse with conflicting event content is forbidden. */
    burned.state = BRAIN_VIS_STATE_DESTROYED;
    assert(brain_visual_upsert(&a, burned) == BRAIN_VIS_ERR_CONFLICT);
    assert(brain_visual_find(&a, burned_key, &missing) == BRAIN_VIS_OK);
    assert(missing.state == BRAIN_VIS_STATE_SCORCHED);
    /* Later semantic event on the same surface is allowed in this local prototype. */
    burned.cause.serial = 17;
    assert(brain_visual_upsert(&a, burned) == BRAIN_VIS_OK);
    assert(brain_visual_find(&a, burned_key, &missing) == BRAIN_VIS_OK);
    assert(missing.state == BRAIN_VIS_STATE_DESTROYED);
    /* Loaded sectors in arbitrary order must not change the same base reference. */
    for (i = 0; i < 200; ++i) {
        BrainVisualSurfaceKey unrelated = surface(i - 100, -i, (uint64_t)i + 1u, 0);
        (void)fake_eye_recipe(c, unrelated);
    }
    assert(fake_eye_recipe(c, clean_key).variant_id == base_eye1.variant_id);
    assert(fake_eye_recipe(c, clean_key).material_id == base_eye1.material_id);
    puts("PASS: reconstruccion referencias, versiones Eye, orden sectores, errores y duplicados");
}
int main(void) { test_roundtrip(); return 0; }
