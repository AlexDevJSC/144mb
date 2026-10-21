#include "ear_surface.h"
#include "ear_bones_contacts.h"
#include "surface_materials.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Simulated Bones dispatcher. Bones itself is not linked or modified. */
#define BONES_MOCK_QUEUE 32
static EarSurfaceEvent pending[BONES_MOCK_QUEUE];
static unsigned pending_count;
static EarEngine a, b;
static EarBonesContactSink sink;
static int16_t sound_a[24000], sound_b[24000], sound_other[24000];

static int mock_bones_emit_contact(const EarSurfaceEvent *event) {
    if (!event || pending_count >= BONES_MOCK_QUEUE) return 0;
    pending[pending_count++] = *event;
    return 1;
}

static void mock_bones_dispatch(EarEngine *audio) {
    unsigned i;
    ear_bones_contacts_init(&sink, audio, UINT32_C(0x1442026));
    for (i = 0; i < pending_count; ++i)
        assert(ear_bones_contacts_receive(&sink, &pending[i]));
    assert(sink.accepted == pending_count);
    assert(sink.rejected == 0);
    pending_count = 0;
}

static int any_nonzero(const int16_t *s, size_t n) {
    size_t i;
    for (i=0; i<n; ++i) if (s[i]) return 1;
    return 0;
}

static void test_one(uint8_t kind, uint8_t category, int16_t *result) {
    EarSurfaceEvent e = {UINT64_C(0x0102030405060708), 77u, 900u, 82u,
                         kind, category, 220u, 0u};
    ear_init(&a, NULL);
    assert(mock_bones_emit_contact(&e));
    mock_bones_dispatch(&a);
    ear_render(&a, result, 24000);
    assert(any_nonzero(result, 24000));
    ear_init(&b, NULL);
    assert(ear_surface_event(&b, &e, UINT32_C(0x1442026)));
    ear_render(&b, sound_b, 4000);
    ear_render(&b, sound_b + 4000, 20000);
    assert(memcmp(result, sound_b, sizeof(sound_b)) == 0);
}

int main(void) {
    P144SurfaceBinding material, modified;
    EarSurfaceEvent invalid = {1u, 7u, 3u, 19u, EAR_CONTACT_STEP,
                               EAR_ACOUSTIC_WOOD, 200u, 0u};
    unsigned i;
    static const uint8_t expected[P144_VIS_COUNT] = {
        EAR_ACOUSTIC_MINERAL, EAR_ACOUSTIC_MINERAL, EAR_ACOUSTIC_WOOD,
        EAR_ACOUSTIC_EARTH, EAR_ACOUSTIC_METAL, EAR_ACOUSTIC_FABRIC,
        EAR_ACOUSTIC_VEGETATION, EAR_ACOUSTIC_MINERAL
    };
    assert(sizeof(P144SurfaceBinding) == 4u);
    for (i = 0; i < P144_VIS_COUNT; ++i) {
        assert(p144_surface_default_binding((uint16_t)i, &material));
        assert(material.visual_material_id == i);
        assert(material.acoustic_id == expected[i]);
        assert(p144_surface_binding_valid(&material));
    }
    /* Explicit override preserves visual identity: metal under fabric. */
    assert(p144_surface_default_binding(P144_VIS_METAL, &modified));
    modified.acoustic_id = EAR_ACOUSTIC_FABRIC;
    assert(modified.visual_material_id == P144_VIS_METAL);
    assert(p144_surface_binding_valid(&modified));
    modified.acoustic_id = EAR_ACOUSTIC_COUNT;
    assert(!p144_surface_binding_valid(&modified));
    assert(!p144_surface_default_binding(250u, &material));
    assert(!p144_surface_default_binding(1u, NULL));
    ear_init(&a, NULL);
    invalid.acoustic = EAR_ACOUSTIC_COUNT;
    assert(!ear_surface_event(&a, &invalid, 1u));
    invalid.acoustic = EAR_ACOUSTIC_WOOD; invalid.intensity = 0;
    assert(!ear_surface_event(&a, &invalid, 1u));
    invalid.intensity = 200u; invalid.kind = 99u;
    assert(!ear_surface_event(&a, &invalid, 1u));
    invalid.kind = EAR_CONTACT_STEP; invalid.reserved = 1u;
    assert(!ear_surface_event(&a, &invalid, 1u));
    assert(!ear_surface_event(NULL, &invalid, 1u));
    assert(!ear_surface_event(&a, NULL, 1u));
    ear_bones_contacts_init(&sink, &a, 1u);
    assert(!ear_bones_contacts_receive(&sink, &invalid));
    assert(sink.rejected == 1u && sink.accepted == 0u);
    assert(!ear_bones_contacts_receive(NULL, &invalid));

    for (i = 0; i < EAR_ACOUSTIC_COUNT; ++i) {
        test_one(EAR_CONTACT_STEP, (uint8_t)i, sound_a);
        test_one(EAR_CONTACT_IMPACT, (uint8_t)i, sound_a);
        test_one(EAR_CONTACT_LANDING, (uint8_t)i, sound_a);
    }
    test_one(EAR_CONTACT_STEP, EAR_ACOUSTIC_MINERAL, sound_a);
    test_one(EAR_CONTACT_STEP, EAR_ACOUSTIC_WOOD, sound_other);
    assert(memcmp(sound_a, sound_other, sizeof(sound_a)) != 0);
    test_one(EAR_CONTACT_STEP, EAR_ACOUSTIC_METAL, sound_other);
    assert(memcmp(sound_a, sound_other, sizeof(sound_a)) != 0);
    test_one(EAR_CONTACT_IMPACT, EAR_ACOUSTIC_MINERAL, sound_a);
    test_one(EAR_CONTACT_IMPACT, EAR_ACOUSTIC_WOOD, sound_other);
    assert(memcmp(sound_a, sound_other, sizeof(sound_a)) != 0);
    /* A full volatile queue, not a persistent journal. */
    pending_count = 0;
    for (i = 0; i < BONES_MOCK_QUEUE; ++i) assert(mock_bones_emit_contact(&invalid));
    assert(!mock_bones_emit_contact(&invalid));
    pending_count = 0;
    puts("PASS: binding identities/defaults/overrides, 6 categories x 3 contacts,");
    puts("      independent sonic profiles, deterministic split render, invalid input,");
    puts("      simulated Bones FIFO event capacity (NOT integration with Bones binary)");
    return 0;
}
