#ifndef BONES_MOCK_HEADER_H
#define BONES_MOCK_HEADER_H
/* TEST DOUBLE ONLY. From Bones-Eye contract text, NOT an actual Bones SDK. */
#define BONES_API_VERSION 1u
typedef struct BonesEngine BonesEngine;
typedef struct BonesScene BonesScene;
typedef struct {
    unsigned version;
    void *user_data;
    void (*on_tick)(void *user_data, BonesEngine *engine);
    void (*on_scene)(void *user_data, const BonesEngine *engine, BonesScene *scene);
} BonesModule;
#endif
