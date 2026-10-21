#include "ear_bones_contacts.h"
#include <string.h>

void ear_bones_contacts_init(EarBonesContactSink *sink, EarEngine *audio,
                             uint32_t world_seed) {
    if (!sink) return;
    memset(sink, 0, sizeof(*sink));
    sink->audio = audio;
    sink->world_seed = world_seed;
}

int ear_bones_contacts_receive(EarBonesContactSink *sink,
                               const EarSurfaceEvent *event) {
    int accepted;
    if (!sink) return 0;
    accepted = ear_surface_event(sink->audio, event, sink->world_seed);
    if (accepted) {
        if (sink->accepted != UINT32_MAX) ++sink->accepted;
    } else {
        if (sink->rejected != UINT32_MAX) ++sink->rejected;
    }
    return accepted;
}
