#include "brain/brain.h"

BrainResult brain_bones_collect_intents(const BrainWorld *w,
                                         const BrainId *ids,
                                         const BrainPerception *perceptions,
                                         size_t count, BrainIntent *outputs,
                                         size_t capacity, size_t *written) {
    size_t i;
    BrainResult rc;
    if (!w || !written || (count && (!ids || !outputs)) || count > capacity)
        return BRAIN_ERR_ARGUMENT;
    *written = 0;
    for (i = 0; i < count; ++i) {
        /* perceptions[i] belongs to ids[i] (or NULL for no detected threat). */
        rc = brain_world_intent(w, ids[i], perceptions ? &perceptions[i] : NULL,
                                &outputs[i]);
        if (rc != BRAIN_OK) return rc;
        *written = i + 1;
    }
    return BRAIN_OK;
}
