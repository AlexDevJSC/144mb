#include "brain/brain_visual.h"

#include <limits.h>
#include <string.h>

static int key_compare(BrainVisualSurfaceKey a, BrainVisualSurfaceKey b) {
    if (a.sector_x != b.sector_x) return a.sector_x < b.sector_x ? -1 : 1;
    if (a.sector_y != b.sector_y) return a.sector_y < b.sector_y ? -1 : 1;
    if (a.object_id != b.object_id) return a.object_id < b.object_id ? -1 : 1;
    if (a.surface_id != b.surface_id) return a.surface_id < b.surface_id ? -1 : 1;
    return 0;
}
static int valid_context(BrainVisualContext c) {
    return (c.world_id.hi || c.world_id.lo) && c.world_generator_version &&
           c.eye_generator_version;
}
static int valid_change(BrainVisualOverride o) {
    return o.key.object_id && o.cause.branch && o.cause.serial &&
           o.state <= BRAIN_VIS_STATE_PAINTED &&
           !(o.flags & (uint8_t)~(BRAIN_VIS_OVERRIDE_MATERIAL | BRAIN_VIS_OVERRIDE_VARIANT)) &&
           (o.state != BRAIN_VIS_STATE_DEFAULT || o.flags) &&
           ((o.flags & BRAIN_VIS_OVERRIDE_MATERIAL) || o.material_override == 0) &&
           ((o.flags & BRAIN_VIS_OVERRIDE_VARIANT) || o.variant_override == 0);
}
static int change_equal(BrainVisualOverride a, BrainVisualOverride b) {
    return key_compare(a.key, b.key) == 0 && a.state == b.state && a.flags == b.flags &&
           a.material_override == b.material_override && a.variant_override == b.variant_override &&
           a.cause.branch == b.cause.branch && a.cause.serial == b.cause.serial;
}
static size_t locate(const BrainVisualSet *set, BrainVisualSurfaceKey key, int *found) {
    size_t lo = 0, hi = set->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (key_compare(set->entries[mid].key, key) < 0) lo = mid + 1;
        else hi = mid;
    }
    *found = lo < set->count && key_compare(set->entries[lo].key, key) == 0;
    return lo;
}
BrainVisualResult brain_visual_init(BrainVisualSet *set, BrainVisualContext ctx,
                                    BrainVisualOverride *storage, size_t capacity) {
    if (!set || !valid_context(ctx) || (capacity && !storage) ||
        capacity > BRAIN_VISUAL_MAX_RECORDS) return BRAIN_VIS_ERR_ARGUMENT;
    set->context = ctx;
    set->entries = storage;
    set->capacity = capacity;
    set->count = 0;
    return BRAIN_VIS_OK;
}
BrainVisualResult brain_visual_upsert(BrainVisualSet *set, BrainVisualOverride change) {
    size_t pos, i;
    int found;
    if (!set || !valid_change(change) || set->count > set->capacity ||
        (set->count && !set->entries)) return BRAIN_VIS_ERR_ARGUMENT;
    pos = locate(set, change.key, &found);
    if (found) {
        BrainVisualOverride existing = set->entries[pos];
        if (change_equal(existing, change)) return BRAIN_VIS_DUPLICATE;
        if (existing.cause.branch == change.cause.branch &&
            existing.cause.serial == change.cause.serial) return BRAIN_VIS_ERR_CONFLICT;
        /* Host-provided later event supersedes existing state for THIS surface.
           PBW branch ordering/merging remains outside this isolated prototype. */
        set->entries[pos] = change;
        return BRAIN_VIS_OK;
    }
    if (set->count == set->capacity) return BRAIN_VIS_ERR_CAPACITY;
    for (i = set->count; i > pos; --i) set->entries[i] = set->entries[i-1];
    set->entries[pos] = change;
    ++set->count;
    return BRAIN_VIS_OK;
}
BrainVisualResult brain_visual_find(const BrainVisualSet *set,
                                    BrainVisualSurfaceKey key, BrainVisualOverride *out) {
    int found;
    size_t pos;
    if (!set || !out || !key.object_id || set->count > set->capacity ||
        (set->count && !set->entries)) return BRAIN_VIS_ERR_ARGUMENT;
    pos = locate(set, key, &found);
    if (!found) return BRAIN_VIS_ERR_NOT_FOUND;
    *out = set->entries[pos];
    return BRAIN_VIS_OK;
}
BrainVisualResult brain_visual_resolve(const BrainVisualSet *set,
                                       BrainVisualSurfaceKey key,
                                       BrainVisualRef eye_generated_base,
                                       BrainVisualRef *out) {
    BrainVisualOverride override;
    BrainVisualResult result;
    if (!set || !out || !key.object_id) return BRAIN_VIS_ERR_ARGUMENT;
    if (eye_generated_base.eye_generator_version != set->context.eye_generator_version)
        return BRAIN_VIS_ERR_EYE_VERSION;
    result = brain_visual_find(set, key, &override);
    if (result != BRAIN_VIS_OK && result != BRAIN_VIS_ERR_NOT_FOUND) return result;
    *out = eye_generated_base;
    out->state = (result == BRAIN_VIS_OK) ? override.state : BRAIN_VIS_STATE_DEFAULT;
    if (result == BRAIN_VIS_OK) {
        if (override.flags & BRAIN_VIS_OVERRIDE_MATERIAL) out->material_id = override.material_override;
        if (override.flags & BRAIN_VIS_OVERRIDE_VARIANT) out->variant_id = override.variant_override;
    }
    return BRAIN_VIS_OK;
}
BrainVisualResult brain_visual_check_eye_version(uint32_t saved_version,
                                                  const uint32_t *available_versions,
                                                  size_t count) {
    size_t i;
    if (!saved_version || (count && !available_versions)) return BRAIN_VIS_ERR_ARGUMENT;
    for (i = 0; i < count; ++i) if (available_versions[i] == saved_version) return BRAIN_VIS_OK;
    return BRAIN_VIS_ERR_EYE_VERSION;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    unsigned i;
    for (i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (i * 8u));
}
static void put64(uint8_t *p, uint64_t v) {
    unsigned i;
    for (i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (i * 8u));
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t get32(const uint8_t *p) {
    uint32_t v = 0;
    unsigned i;
    for (i = 0; i < 4; ++i) v |= (uint32_t)p[i] << (i * 8u);
    return v;
}
static uint64_t get64(const uint8_t *p) {
    uint64_t v = 0;
    unsigned i;
    for (i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (i * 8u);
    return v;
}
static int64_t get_signed64(const uint8_t *p) {
    uint64_t bits = get64(p);
    if (bits <= (uint64_t)INT64_MAX) return (int64_t)bits;
    if (bits == (UINT64_C(1) << 63)) return INT64_MIN;
    return -(int64_t)((~bits) + UINT64_C(1));
}
static uint32_t checksum(const uint8_t *data, size_t length) {
    uint32_t h = UINT32_C(2166136261);
    size_t i;
    for (i = 0; i < length; ++i) {
        if (i >= 44u && i < 48u) continue;
        h ^= data[i]; h *= UINT32_C(16777619);
    }
    return h;
}
static void encode_entry(uint8_t *p, BrainVisualOverride e) {
    put64(p + 0, (uint64_t)e.key.sector_x);
    put64(p + 8, (uint64_t)e.key.sector_y);
    put64(p + 16, e.key.object_id);
    put32(p + 24, e.key.surface_id);
    p[28] = e.state; p[29] = e.flags; put16(p + 30, 0);
    put32(p + 32, e.material_override);
    put32(p + 36, e.variant_override);
    put64(p + 40, e.cause.branch);
    put64(p + 48, e.cause.serial);
}
static BrainVisualOverride decode_entry(const uint8_t *p) {
    BrainVisualOverride e;
    memset(&e, 0, sizeof e);
    e.key.sector_x = get_signed64(p + 0);
    e.key.sector_y = get_signed64(p + 8);
    e.key.object_id = get64(p + 16);
    e.key.surface_id = get32(p + 24);
    e.state = p[28]; e.flags = p[29];
    e.material_override = get32(p + 32);
    e.variant_override = get32(p + 36);
    e.cause.branch = get64(p + 40);
    e.cause.serial = get64(p + 48);
    return e;
}
BrainVisualResult brain_visual_encode(const BrainVisualSet *set,
                                       uint8_t *bytes, size_t capacity, size_t *written) {
    size_t need, i;
    if (!set || !bytes || !written || !valid_context(set->context) ||
        set->count > set->capacity || set->count > BRAIN_VISUAL_MAX_RECORDS ||
        (set->count && !set->entries)) return BRAIN_VIS_ERR_ARGUMENT;
    need = BRAIN_VISUAL_HEADER_BYTES + set->count * BRAIN_VISUAL_RECORD_BYTES;
    if (capacity < need) return BRAIN_VIS_ERR_CAPACITY;
    for (i = 0; i < set->count; ++i) {
        if (!valid_change(set->entries[i]) ||
            (i && key_compare(set->entries[i-1].key, set->entries[i].key) >= 0))
            return BRAIN_VIS_ERR_MALFORMED;
    }
    memcpy(bytes, "BVR1", 4u);
    put16(bytes + 4, BRAIN_VISUAL_SCHEMA_VERSION);
    put16(bytes + 6, 0);
    put64(bytes + 8, set->context.world_id.hi);
    put64(bytes + 16, set->context.world_id.lo);
    put64(bytes + 24, set->context.world_seed);
    put32(bytes + 32, set->context.world_generator_version);
    put32(bytes + 36, set->context.eye_generator_version);
    put32(bytes + 40, (uint32_t)set->count);
    put32(bytes + 44, 0);
    for (i = 0; i < set->count; ++i)
        encode_entry(bytes + BRAIN_VISUAL_HEADER_BYTES + i * BRAIN_VISUAL_RECORD_BYTES,
                     set->entries[i]);
    put32(bytes + 44, checksum(bytes, need));
    *written = need;
    return BRAIN_VIS_OK;
}
BrainVisualResult brain_visual_decode(BrainVisualSet *out,
                                       BrainVisualContext expected_world,
                                       const uint8_t *bytes, size_t length,
                                       BrainVisualOverride *storage, size_t capacity) {
    size_t count, i;
    BrainVisualContext incoming;
    BrainVisualSurfaceKey previous = {0};
    if (!out || !bytes || !valid_context(expected_world) ||
        (capacity && !storage) || capacity > BRAIN_VISUAL_MAX_RECORDS ||
        length < BRAIN_VISUAL_HEADER_BYTES) return BRAIN_VIS_ERR_ARGUMENT;
    if (memcmp(bytes, "BVR1", 4u) || get16(bytes + 4) != BRAIN_VISUAL_SCHEMA_VERSION ||
        get16(bytes + 6) != 0) return BRAIN_VIS_ERR_MALFORMED;
    count = get32(bytes + 40);
    if (count > BRAIN_VISUAL_MAX_RECORDS ||
        length != BRAIN_VISUAL_HEADER_BYTES + count * BRAIN_VISUAL_RECORD_BYTES)
        return BRAIN_VIS_ERR_MALFORMED;
    if (count > capacity) return BRAIN_VIS_ERR_CAPACITY;
    if (checksum(bytes, length) != get32(bytes + 44)) return BRAIN_VIS_ERR_MALFORMED;
    incoming.world_id.hi = get64(bytes + 8);
    incoming.world_id.lo = get64(bytes + 16);
    incoming.world_seed = get64(bytes + 24);
    incoming.world_generator_version = get32(bytes + 32);
    incoming.eye_generator_version = get32(bytes + 36);
    if (incoming.world_id.hi != expected_world.world_id.hi ||
        incoming.world_id.lo != expected_world.world_id.lo ||
        incoming.world_seed != expected_world.world_seed ||
        incoming.world_generator_version != expected_world.world_generator_version)
        return BRAIN_VIS_ERR_WORLD;
    if (incoming.eye_generator_version != expected_world.eye_generator_version)
        return BRAIN_VIS_ERR_EYE_VERSION;
    for (i = 0; i < count; ++i) {
        const uint8_t *row = bytes + BRAIN_VISUAL_HEADER_BYTES + i * BRAIN_VISUAL_RECORD_BYTES;
        BrainVisualOverride current = decode_entry(row);
        if (get16(row + 30) != 0 || !valid_change(current) ||
            (i && key_compare(previous, current.key) >= 0)) return BRAIN_VIS_ERR_MALFORMED;
        previous = current.key;
    }
    /* Two-pass validation: output/storage remain unchanged on failure. */
    for (i = 0; i < count; ++i)
        storage[i] = decode_entry(bytes + BRAIN_VISUAL_HEADER_BYTES + i * BRAIN_VISUAL_RECORD_BYTES);
    out->context = incoming;
    out->entries = storage;
    out->capacity = capacity;
    out->count = count;
    return BRAIN_VIS_OK;
}
