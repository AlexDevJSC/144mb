#ifndef BONES_CORE_H
#define BONES_CORE_H
/* Contract fixture reconstructed from Bones v0.1 API described in conversation.
   For real integration replace include path with the canonical Bones header. */
#include <stdint.h>
#include <stdbool.h>
#define BONES_API_VERSION 1u
#define BONES_GENERATOR_VERSION 1u
#define BONES_TICK_RATE 60u
#define BONES_TICK_DT (1.0f/60.0f)
#define BONES_MAX_COLLIDERS 16u
#define BONES_MAX_MODULES 8u
#define BONES_MAX_QUADS 96u
#define BONES_MAX_SPRITES 8u
typedef struct {float x,y,z;} BonesVec3;
typedef struct {BonesVec3 min,max;} BonesAabb;
typedef struct {int64_t x,z;} BonesSectorCoord;
typedef enum {BONES_MAT_STONE=0,BONES_MAT_FLOOR=1,BONES_MAT_WOOD=2} BonesMaterial;
typedef struct {uint64_t id; BonesAabb bounds; uint8_t solid,render_box,material;} BonesCollider;
typedef struct {uint64_t seed;uint32_t generator_version,collider_count;BonesCollider colliders[BONES_MAX_COLLIDERS];} BonesWorld;
typedef struct {BonesVec3 feet,velocity;float yaw,pitch,radius,height,eye_height;bool grounded,jump_held;} BonesPlayer;
typedef struct {uint8_t forward,backward,left,right,sprint,jump,yaw_left,yaw_right,pitch_up,pitch_down;float mouse_dx,mouse_dy;} BonesInput;
typedef struct {BonesVec3 position;float yaw,pitch;} BonesCamera;
typedef struct {BonesVec3 position;float u,v;} BonesVertex;
typedef struct {BonesVertex vertices[4];uint8_t material;} BonesQuad;
typedef struct {BonesVec3 feet;float width,height;uint32_t animation_tick;} BonesSprite;
typedef struct {BonesCamera camera;BonesQuad quads[BONES_MAX_QUADS];BonesSprite sprites[BONES_MAX_SPRITES];uint32_t quad_count,sprite_count;} BonesScene;
struct BonesEngine;
typedef struct BonesModule {uint32_t version;void *user_data;void (*on_tick)(void *,struct BonesEngine *);void (*on_scene)(void *,const struct BonesEngine *,BonesScene *);} BonesModule;
typedef struct BonesEngine {BonesWorld world;BonesPlayer player;uint64_t tick;BonesModule modules[BONES_MAX_MODULES];uint32_t module_count;} BonesEngine;
typedef struct {uint32_t version;void *user_data;void (*begin)(void *,const BonesCamera *);void (*draw_quad)(void *,const BonesCamera *,const BonesQuad *);void (*draw_sprite)(void *,const BonesCamera *,const BonesSprite *);void (*end)(void *);} BonesRenderBackend;
void bones_init(BonesEngine *e,uint64_t seed);
bool bones_step(BonesEngine *e,const BonesInput *in,float dt);
bool bones_register_module(BonesEngine *e,const BonesModule *m);
bool bones_world_add_collider(BonesWorld *w,const BonesCollider *c);
bool bones_world_remove_collider(BonesWorld *w,uint64_t id);
#endif
