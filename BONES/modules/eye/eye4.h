#ifndef EYE4_H
#define EYE4_H
#include "eye3.h"

/* Eye4 standalone architectural compositor. Does not own world geometry. */
#define EYE4_VERSION 4u
#define EYE4_SIZE 64u
#define EYE4_RECORD_BYTES 16u
#define EYE4_HEADER_BYTES 12u
#define EYE4_MAX_RECIPES 64u
#define EYE4_MAX_PACKET (EYE4_HEADER_BYTES + EYE4_MAX_RECIPES * EYE4_RECORD_BYTES)
#define EYE4_CACHE_SLOTS 16u

typedef enum {
    EYE4_PROCEDURAL = 0,
    EYE4_STATIC = 1,
    EYE4_HYBRID = 2
} Eye4Method;

typedef enum {
    EYE4_PANEL = 0, EYE4_BRACE = 1, EYE4_WINDOW = 2,
    EYE4_DOOR = 3, EYE4_ROOF = 4, EYE4_FOUNDATION = 5,
    EYE4_ROAD = 6, EYE4_PANEL_UPPER = 7
} Eye4Motif;

/* 16 canonical file bytes: 12 EYE3 base bytes + motif, style, detail, method.
 * The struct is an in-memory API, NOT serialized using memcpy/sizeof. */
typedef struct {
    EyeRecipe base;
    uint8_t motif, style, detail, method;
} Eye4Recipe;

typedef struct {
    Eye4Recipe recipe[EYE4_MAX_RECIPES];
    unsigned count;
} Eye4Bank;

typedef struct {
    const Eye4Bank *bank;
    uint32_t *storage;
    uint16_t keys[EYE4_CACHE_SLOTS];
    uint32_t ages[EYE4_CACHE_SLOTS];
    uint32_t serial;
    unsigned count, slots, hits, misses;
} Eye4Cache;

int eye4_valid(const Eye4Recipe *r);
int eye4_generate(const Eye4Recipe *r, uint32_t *pixels, size_t count);
size_t eye4_write(const Eye4Recipe *r, unsigned count, uint8_t *dst, size_t cap);
int eye4_read(const uint8_t *src, size_t size, Eye4Bank *out);
int eye4_cache_init(Eye4Cache *c, const Eye4Bank *b, uint32_t *arena,
                    size_t capacity, unsigned slots);
const uint32_t *eye4_cache_get(Eye4Cache *c, unsigned id);
/* 8 architectural motifs, 3 reconstructors; each material's encoded recipe is 16 B. */
void eye4_demo_recipes(Eye4Recipe out[8], Eye4Method method);
#endif
