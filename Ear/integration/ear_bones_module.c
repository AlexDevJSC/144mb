#include "ear_bones_module.h"

/* Bones owns all authoritative state; Ear never mutates BonesEngine.
   Event emission is explicit via ear_bones_emit when the game commits action. */
static void on_tick(void *userdata, BonesEngine *engine) {
    (void)userdata;
    (void)engine;
    /* Intentionally NO audio synthesis or wall-clock work in fixed tick. */
}
static void on_scene(void *userdata, const BonesEngine *engine,
                     BonesScene *scene) {
    (void)userdata;
    (void)engine;
    (void)scene;
    /* Intentionally NO audio work in the rendering scene callback. */
}

void ear_bones_module_init(EarBonesModule *m, EarEngine *audio,
                           uint32_t world_seed) {
    if (!m) return;
    ear_bones_bridge_init(&m->bridge, audio, world_seed);
    m->module.version = BONES_API_VERSION;
    m->module.user_data = m;
    m->module.on_tick = on_tick;
    m->module.on_scene = on_scene;
}
