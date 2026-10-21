#include "eye_bones_bridge.h"

int eye_bones_legacy_fill(const EyeTextureBank *bank,
                          const unsigned material_ids[EYE_BONES_LEGACY_SLOTS],
                          uint32_t *target,
                          size_t target_capacity) {
    unsigned i;
    if (!bank || !material_ids || !target ||
        target_capacity < EYE_BONES_LEGACY_SLOTS * EYE_BONES_LEGACY_TEXELS ||
        (bank->version != 1u && bank->version != 2u && bank->version != 3u)) return 0;
    /* All material IDs validated BEFORE the first write. Bank recipes have
     * already been validated by eye_bank_open(). */
    for (i = 0; i < EYE_BONES_LEGACY_SLOTS; ++i) {
        if (material_ids[i] >= bank->count ||
            !(bank->version == 3u ? eye3_valid(&bank->recipes[material_ids[i]]) :
                                    eye_valid(&bank->recipes[material_ids[i]]))) return 0;
    }
    for (i = 0; i < EYE_BONES_LEGACY_SLOTS; ++i) {
        /* After input validation, the renderer is an integer-only total
         * function for every valid recipe and full 64x64 output buffer. */
        if (!eye_bank_render(bank, material_ids[i],
                  target + (size_t)i * EYE_BONES_LEGACY_TEXELS,
                  EYE_BONES_LEGACY_TEXELS)) return 0;
    }
    return 1;
}

int eye_bones_legacy_fill_tone(const EyeTextureBank *bank,
                               const unsigned material_ids[EYE_BONES_LEGACY_SLOTS],
                               uint32_t *target, size_t capacity, EyeTone tone) {
    unsigned i;
    if (tone != EYE_TONE_ORIGINAL && tone != EYE_TONE_AGED) return 0;
    if (!eye_bones_legacy_fill(bank, material_ids, target, capacity)) return 0;
    if (tone == EYE_TONE_AGED)
        for (i = 0; i < EYE_BONES_LEGACY_SLOTS * EYE_BONES_LEGACY_TEXELS; ++i)
            target[i] = eye_tone_pixel(target[i], tone);
    return 1;
}
