#include "ear_bones_module.h"
#include <assert.h>
#include <stdio.h>

static EarEngine audio;
static EarBonesModule adapter;
int main(void) {
    ear_init(&audio, NULL);
    ear_bones_module_init(&adapter, &audio, UINT32_C(0x12345));
    assert(adapter.module.version == BONES_API_VERSION);
    assert(adapter.module.user_data == &adapter);
    assert(adapter.module.on_tick);
    assert(adapter.module.on_scene);
    adapter.module.on_tick(adapter.module.user_data, NULL);
    adapter.module.on_scene(adapter.module.user_data, NULL, NULL);
    ear_bones_emit(&adapter.bridge, EAR_BONES_UI_CONFIRM, 17u);
    puts("PASS: mock Bones v1 interface (NOT actual Bones compatibility)");
    return 0;
}
