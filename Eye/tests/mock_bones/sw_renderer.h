/* ONLY A COMPILE-TIME DOUBLE of the stable legacy tex field. */
#ifndef P144_SW_RENDERER_MOCK_H
#define P144_SW_RENDERER_MOCK_H
#include <stdint.h>
#define P144_TEX_SIZE 64u
#define P144_MAT_COUNT 3u
typedef struct {
    uint32_t tex[P144_MAT_COUNT][P144_TEX_SIZE * P144_TEX_SIZE];
} P144_Renderer;
#endif
