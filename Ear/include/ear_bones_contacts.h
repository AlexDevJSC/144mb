#ifndef EAR_BONES_CONTACTS_H
#define EAR_BONES_CONTACTS_H

#include "ear_surface.h"

/* Adapter entry for a FUTURE material-aware Bones payload. The Bones 0.2.0
   headers do NOT contain this interface; see contract/CONTRATO_EYE_EAR_BONES.txt.
   Strictly no attempt to derive audio category from BonesEvent.target_id. */
typedef struct {
    EarEngine *audio;        /* borrowed, owned by Ear */
    uint32_t world_seed;
    uint32_t accepted;
    uint32_t rejected;
} EarBonesContactSink;

void ear_bones_contacts_init(EarBonesContactSink *sink, EarEngine *audio,
                             uint32_t world_seed);
/* Called only after Bones resolved a local contact with its authoritative
   surface binding and populated EarSurfaceEvent. Not usable yet directly from
   BonesSubsystem.on_event until Bones adds a payload contract. */
int ear_bones_contacts_receive(EarBonesContactSink *sink,
                               const EarSurfaceEvent *event);

#endif
