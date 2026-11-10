/* SPDX-License-Identifier: MIT */
#include "compat.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

enum {
    MAX_OBJECTS = 512,
    MAX_DIMENSION = 4096,
    MAX_BITMAP_BYTES = 64 * 1024 * 1024,
    STOCK_BASE = 0x20000002,
    HANDLE_STEP = 16,
    INK_SOLID = -1,
    INK_NONE = -2,
    BLACK = 0,
    WHITE = 0xffffff,
    MM_TEXT = 1,
    TRANSPARENT = 1,
    OPAQUE = 2,
    NULLREGION = 1,
    SIMPLEREGION = 2,
    SRCCOPY = 0x00cc0020,
    PATCOPY = 0x00f00021,
    BLACKNESS = 0x00000042,
    WHITENESS = 0x00ff0062
};

enum object_kind { BITMAP, PEN, BRUSH, CONTEXT };

struct gdi_object;

struct bitmap {
    int32_t width, height;
    bool mono;
    uint32_t *pixels;
    struct gdi_object *owner;
};

struct ink {
    uint32_t color;
    int pattern;
};

struct context {
    struct gdi_object *bitmap, *pen, *brush, *original;
    struct nt_rect clip;
    bool clipped, thread_owned;
    pthread_t creator;
    uint32_t text_color;
    int32_t background_mode;
};

struct gdi_object {
    uintptr_t handle;
    enum object_kind kind;
    unsigned int selections;
    union {
        struct bitmap bitmap;
        struct ink ink;
        struct context dc;
    } value;
};

#define STOCK(i, kind, color, pattern) \
    {STOCK_BASE + (i) * HANDLE_STEP, (kind), 0, {.ink = {(color), (pattern)}}}
static struct gdi_object stock[] = {
    STOCK(0, BRUSH, WHITE, INK_SOLID),
    STOCK(1, BRUSH, 0xc0c0c0, INK_SOLID),
    STOCK(2, BRUSH, 0x808080, INK_SOLID),
    STOCK(3, BRUSH, 0x404040, INK_SOLID),
    STOCK(4, BRUSH, BLACK, INK_SOLID),
    STOCK(5, BRUSH, BLACK, INK_NONE),
    STOCK(6, PEN, WHITE, INK_SOLID),
    STOCK(7, PEN, BLACK, INK_SOLID),
    STOCK(8, PEN, BLACK, INK_NONE)
};
#undef STOCK

enum { STOCK_COUNT = sizeof(stock) / sizeof(stock[0]) };
static struct gdi_object objects[MAX_OBJECTS];
static uintptr_t next_handle = STOCK_BASE + STOCK_COUNT * HANDLE_STEP;
static size_t bitmap_bytes;
static pthread_mutex_t gdi_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t owner_once = PTHREAD_ONCE_INIT;
static pthread_key_t owner_key;
static int owner_key_error;

static void finish(uint32_t error)
{
    pthread_mutex_unlock(&gdi_lock);
    if (error != NT_ERROR_SUCCESS)
        nt_SetLastError(error);
}

static struct gdi_object *find(uintptr_t handle)
{
    if (handle < STOCK_BASE || (handle - STOCK_BASE) % HANDLE_STEP != 0)
        return NULL;
    const uintptr_t slot = (handle - STOCK_BASE) / HANDLE_STEP;
    if (slot < STOCK_COUNT)
        return &stock[slot];
    for (size_t i = 0; i < MAX_OBJECTS; ++i) {
        if (objects[i].handle == handle)
            return &objects[i];
    }
    return NULL;
}

static struct context *find_context(uintptr_t handle)
{
    struct gdi_object *object = find(handle);
    return object != NULL && object->kind == CONTEXT ? &object->value.dc : NULL;
}

static struct gdi_object *allocate(enum object_kind kind)
{
    if (next_handle > UINTPTR_MAX - HANDLE_STEP)
        return NULL;
    for (size_t i = 0; i < MAX_OBJECTS; ++i) {
        if (objects[i].handle == 0) {
            objects[i] = (struct gdi_object){.handle = next_handle, .kind = kind};
            next_handle += HANDLE_STEP;
            return &objects[i];
        }
    }
    return NULL;
}

static bool valid_size(int32_t width, int32_t height)
{
    return width >= 0 && width <= MAX_DIMENSION && height >= 0 && height <= MAX_DIMENSION;
}

static struct gdi_object *allocate_bitmap(int32_t width, int32_t height, bool mono)
{
    const size_t count = (size_t)width * (size_t)height;
    const size_t bytes = count * sizeof(uint32_t);
    if (bytes > MAX_BITMAP_BYTES - bitmap_bytes)
        return NULL;
    struct gdi_object *object = allocate(BITMAP);
    if (object == NULL)
        return NULL;
    uint32_t *pixels = calloc(count, sizeof(*pixels));
    if (pixels == NULL) {
        object->handle = 0;
        return NULL;
    }
    bitmap_bytes += bytes;
    object->value.bitmap = (struct bitmap){width, height, mono, pixels, NULL};
    return object;
}

static void release_bitmap(struct gdi_object *object)
{
    struct bitmap *bitmap = &object->value.bitmap;
    bitmap_bytes -= (size_t)bitmap->width * (size_t)bitmap->height * sizeof(uint32_t);
    free(bitmap->pixels);
    object->handle = 0;
}

static void release_context(struct gdi_object *object)
{
    struct context *dc = &object->value.dc;
    --dc->bitmap->selections;
    --dc->pen->selections;
    --dc->brush->selections;
    release_bitmap(dc->original);
    object->handle = 0;
}

static void release_thread_contexts(void *unused)
{
    (void)unused;
    const pthread_t self = pthread_self();
    pthread_mutex_lock(&gdi_lock);
    for (size_t i = 0; i < MAX_OBJECTS; ++i) {
        struct gdi_object *object = &objects[i];
        if (object->handle != 0 && object->kind == CONTEXT && object->value.dc.thread_owned &&
            pthread_equal(object->value.dc.creator, self))
            release_context(object);
    }
    pthread_mutex_unlock(&gdi_lock);
}

static void initialize_owner_key(void)
{
    owner_key_error = pthread_key_create(&owner_key, release_thread_contexts);
}

static uintptr_t create_context(uintptr_t reference, int32_t width, int32_t height,
                                bool mono, bool thread_owned)
{
    static char owner_marker;
    if (thread_owned &&
        (pthread_once(&owner_once, initialize_owner_key) != 0 || owner_key_error != 0 ||
         pthread_setspecific(owner_key, &owner_marker) != 0)) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    pthread_mutex_lock(&gdi_lock);
    if (reference != 0 && find_context(reference) == NULL) {
        finish(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    struct gdi_object *object = allocate(CONTEXT);
    if (object == NULL) {
        finish(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    struct gdi_object *bitmap = allocate_bitmap(width, height, mono);
    if (bitmap == NULL) {
        object->handle = 0;
        finish(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    object->value.dc = (struct context){
        .bitmap = bitmap, .pen = &stock[7], .brush = &stock[0], .original = bitmap,
        .thread_owned = thread_owned, .creator = pthread_self(), .background_mode = OPAQUE
    };
    bitmap->value.bitmap.owner = object;
    ++bitmap->selections;
    ++stock[7].selections;
    ++stock[0].selections;
    const uintptr_t handle = object->handle;
    finish(NT_ERROR_SUCCESS);
    return handle;
}

uintptr_t nt_runtime_create_surface(int32_t width, int32_t height)
{
    if (!valid_size(width, height) || width == 0 || height == 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return create_context(0, width, height, false, false);
}

uintptr_t NT_MS_ABI nt_CreateCompatibleDC(uintptr_t reference)
{
    return create_context(reference, 1, 1, true, reference == 0);
}

uintptr_t NT_MS_ABI nt_CreateCompatibleBitmap(uintptr_t handle, int32_t width, int32_t height)
{
    if (!valid_size(width, height)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    pthread_mutex_lock(&gdi_lock);
    const struct context *dc = find_context(handle);
    if (dc == NULL) {
        finish(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    const bool empty = width == 0 || height == 0;
    struct gdi_object *bitmap = allocate_bitmap(empty ? 1 : width, empty ? 1 : height,
                                                empty || dc->bitmap->value.bitmap.mono);
    const uintptr_t result = bitmap != NULL ? bitmap->handle : 0;
    finish(bitmap == NULL ? NT_ERROR_NOT_ENOUGH_MEMORY : NT_ERROR_SUCCESS);
    return result;
}

static uintptr_t create_ink(enum object_kind kind, uint32_t color, int pattern)
{
    if (color > WHITE) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    pthread_mutex_lock(&gdi_lock);
    struct gdi_object *object = allocate(kind);
    if (object == NULL) {
        finish(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    object->value.ink = (struct ink){color, pattern};
    const uintptr_t handle = object->handle;
    finish(NT_ERROR_SUCCESS);
    return handle;
}

uintptr_t NT_MS_ABI nt_CreatePen(int32_t style, int32_t width, uint32_t color)
{
    /* PS_SOLID and PS_NULL are the supported cosmetic pens. */
    if ((style != 0 && style != 5) || width < 0 || (style == 0 && width > 1)) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    return create_ink(PEN, color, style == 5 ? INK_NONE : INK_SOLID);
}

uintptr_t NT_MS_ABI nt_CreateSolidBrush(uint32_t color)
{
    return create_ink(BRUSH, color, INK_SOLID);
}

uintptr_t NT_MS_ABI nt_CreateHatchBrush(int32_t hatch, uint32_t color)
{
    if (hatch < 0 || hatch > 5) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return create_ink(BRUSH, color, hatch);
}

uintptr_t NT_MS_ABI nt_GetStockObject(int32_t index)
{
    if (index < 0 || index >= STOCK_COUNT) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    return stock[index].handle;
}

uintptr_t NT_MS_ABI nt_SelectObject(uintptr_t handle, uintptr_t selection)
{
    pthread_mutex_lock(&gdi_lock);
    struct gdi_object *dc_object = find(handle);
    struct gdi_object *object = find(selection);
    if (dc_object == NULL || dc_object->kind != CONTEXT || object == NULL ||
        object->kind == CONTEXT) {
        finish(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    struct context *dc = &dc_object->value.dc;
    struct gdi_object **slot;
    if (object->kind == BITMAP) {
        const struct bitmap *bitmap = &object->value.bitmap;
        if ((bitmap->owner != NULL && bitmap->owner != dc_object) ||
            (object->selections != 0 && dc->bitmap != object)) {
            finish(NT_ERROR_ACCESS_DENIED);
            return 0;
        }
        slot = &dc->bitmap;
    } else {
        slot = object->kind == PEN ? &dc->pen : &dc->brush;
    }
    const uintptr_t previous = (*slot)->handle;
    --(*slot)->selections;
    *slot = object;
    ++object->selections;
    finish(NT_ERROR_SUCCESS);
    return previous;
}

static int32_t delete_handle(uintptr_t handle, bool dc_only)
{
    pthread_mutex_lock(&gdi_lock);
    struct gdi_object *object = find(handle);
    if (object == NULL || (dc_only && object->kind != CONTEXT)) {
        finish(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    if (object->kind == CONTEXT) {
        release_context(object);
    } else if (handle >= STOCK_BASE + STOCK_COUNT * HANDLE_STEP) {
        if (object->selections != 0 ||
            (object->kind == BITMAP && object->value.bitmap.owner != NULL)) {
            finish(NT_ERROR_ACCESS_DENIED);
            return 0;
        }
        if (object->kind == BITMAP)
            release_bitmap(object);
        else
            object->handle = 0;
    }
    finish(NT_ERROR_SUCCESS);
    return 1;
}

int32_t NT_MS_ABI nt_DeleteObject(uintptr_t handle)
{
    return delete_handle(handle, false);
}

int32_t NT_MS_ABI nt_DeleteDC(uintptr_t handle)
{
    return delete_handle(handle, true);
}

int32_t NT_MS_ABI nt_SetBkMode(uintptr_t handle, int32_t mode)
{
    pthread_mutex_lock(&gdi_lock);
    struct context *dc = find_context(handle);
    if (dc == NULL || (mode != TRANSPARENT && mode != OPAQUE)) {
        finish(dc == NULL ? NT_ERROR_INVALID_HANDLE : NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const int32_t previous = dc->background_mode;
    dc->background_mode = mode;
    finish(NT_ERROR_SUCCESS);
    return previous;
}

int32_t NT_MS_ABI nt_SetMapMode(uintptr_t handle, int32_t mode)
{
    pthread_mutex_lock(&gdi_lock);
    const bool valid = find_context(handle) != NULL;
    finish(!valid ? NT_ERROR_INVALID_HANDLE :
           (mode == MM_TEXT ? NT_ERROR_SUCCESS : NT_ERROR_NOT_SUPPORTED));
    return valid && mode == MM_TEXT ? MM_TEXT : 0;
}

uint32_t NT_MS_ABI nt_SetTextColor(uintptr_t handle, uint32_t color)
{
    pthread_mutex_lock(&gdi_lock);
    struct context *dc = find_context(handle);
    if (dc == NULL || color > WHITE) {
        finish(dc == NULL ? NT_ERROR_INVALID_HANDLE : NT_ERROR_NOT_SUPPORTED);
        return UINT32_MAX;
    }
    const uint32_t previous = dc->text_color;
    dc->text_color = color;
    finish(NT_ERROR_SUCCESS);
    return previous;
}

static struct nt_rect bounds(const struct context *dc)
{
    const struct bitmap *bitmap = &dc->bitmap->value.bitmap;
    struct nt_rect rect = {0, 0, bitmap->width, bitmap->height};
    if (dc->clipped)
        nt_IntersectRect(&rect, &rect, &dc->clip);
    return rect;
}

static bool contains(struct nt_rect rect, int32_t x, int32_t y)
{
    return x >= rect.left && y >= rect.top && x < rect.right && y < rect.bottom;
}

static uint32_t *pixel_at(const struct bitmap *bitmap, int32_t x, int32_t y)
{
    return &bitmap->pixels[(size_t)y * (size_t)bitmap->width + (size_t)x];
}

uint32_t nt_runtime_read_pixel(uintptr_t handle, int32_t x, int32_t y)
{
    pthread_mutex_lock(&gdi_lock);
    const struct context *dc = find_context(handle);
    uint32_t color = UINT32_MAX;
    if (dc != NULL) {
        const struct bitmap *bitmap = &dc->bitmap->value.bitmap;
        if (contains((struct nt_rect){0, 0, bitmap->width, bitmap->height}, x, y))
            color = *pixel_at(bitmap, x, y);
    }
    finish(NT_ERROR_SUCCESS);
    return color;
}

static uint32_t display_color(const struct bitmap *bitmap, uint32_t color)
{
    if (!bitmap->mono)
        return color;
    const uint32_t red = color & 255, green = (color >> 8) & 255, blue = color >> 16;
    return red + green + blue >= 3 * 128 ? WHITE : BLACK;
}

uint32_t NT_MS_ABI nt_SetPixel(uintptr_t handle, int32_t x, int32_t y, uint32_t color)
{
    pthread_mutex_lock(&gdi_lock);
    const struct context *dc = find_context(handle);
    if (dc == NULL || color > WHITE || !contains(bounds(dc), x, y)) {
        finish(dc == NULL ? NT_ERROR_INVALID_HANDLE : NT_ERROR_INVALID_PARAMETER);
        return UINT32_MAX;
    }
    const struct bitmap *bitmap = &dc->bitmap->value.bitmap;
    const uint32_t value = display_color(bitmap, color);
    *pixel_at(bitmap, x, y) = value;
    finish(NT_ERROR_SUCCESS);
    return value;
}

int32_t NT_MS_ABI nt_IntersectClipRect(uintptr_t handle, int32_t left, int32_t top,
                                      int32_t right, int32_t bottom)
{
    pthread_mutex_lock(&gdi_lock);
    struct context *dc = find_context(handle);
    if (dc == NULL) {
        finish(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    const struct nt_rect requested = {left, top, right, bottom};
    const struct nt_rect visible = bounds(dc);
    const bool nonempty = nt_IntersectRect(&dc->clip, &visible, &requested);
    dc->clipped = true;
    finish(NT_ERROR_SUCCESS);
    return nonempty ? SIMPLEREGION : NULLREGION;
}

int32_t NT_MS_ABI nt_SelectClipRgn(uintptr_t handle, uintptr_t region)
{
    pthread_mutex_lock(&gdi_lock);
    struct context *dc = find_context(handle);
    if (dc == NULL || region != 0) {
        finish(dc == NULL ? NT_ERROR_INVALID_HANDLE : NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    dc->clipped = false;
    finish(NT_ERROR_SUCCESS);
    return SIMPLEREGION;
}

static bool hatch_pixel(int pattern, int32_t x, int32_t y)
{
    switch (pattern) {
    case 0: return y % 8 == 0;
    case 1: return x % 8 == 0;
    case 2: return (x + y) % 8 == 0;
    case 3: return (x - y) % 8 == 0;
    case 4: return x % 8 == 0 || y % 8 == 0;
    case 5: return (x + y) % 8 == 0 || (x - y) % 8 == 0;
    default: return true;
    }
}

static void paint_brush(const struct context *dc, int32_t x, int32_t y)
{
    const struct ink brush = dc->brush->value.ink;
    if (brush.pattern == INK_NONE)
        return;
    const struct bitmap *bitmap = &dc->bitmap->value.bitmap;
    if (hatch_pixel(brush.pattern, x, y))
        *pixel_at(bitmap, x, y) = display_color(bitmap, brush.color);
    else if (dc->background_mode == OPAQUE)
        *pixel_at(bitmap, x, y) = WHITE;
}

int32_t NT_MS_ABI nt_Rectangle(uintptr_t handle, int32_t left, int32_t top,
                              int32_t right, int32_t bottom)
{
    pthread_mutex_lock(&gdi_lock);
    const struct context *dc = find_context(handle);
    if (dc == NULL) {
        finish(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    if (right < left) {
        const int32_t old_left = left;
        left = right;
        right = old_left;
    }
    if (bottom < top) {
        const int32_t old_top = top;
        top = bottom;
        bottom = old_top;
    }
    if (left < right && top < bottom) {
        const struct ink pen = dc->pen->value.ink;
        const bool outline = pen.pattern != INK_NONE;
        const struct nt_rect shape = {left, top, right - !outline, bottom - !outline};
        struct nt_rect visible = bounds(dc);
        const struct bitmap *bitmap = &dc->bitmap->value.bitmap;
        if (nt_IntersectRect(&visible, &visible, &shape)) {
            for (int32_t y = visible.top; y < visible.bottom; ++y) {
                for (int32_t x = visible.left; x < visible.right; ++x) {
                    if (outline && (x == left || x == right - 1 || y == top || y == bottom - 1))
                        *pixel_at(bitmap, x, y) = display_color(bitmap, pen.color);
                    else
                        paint_brush(dc, x, y);
                }
            }
        }
    }
    finish(NT_ERROR_SUCCESS);
    return 1;
}

static bool clip_transfer(struct nt_rect *rect, int32_t x, int32_t y,
                          int32_t width, int32_t height)
{
    const int64_t right = (int64_t)x + width, bottom = (int64_t)y + height;
    if (rect->left < x)
        rect->left = x;
    if (rect->top < y)
        rect->top = y;
    if (rect->right > right)
        rect->right = (int32_t)right;
    if (rect->bottom > bottom)
        rect->bottom = (int32_t)bottom;
    return rect->left < rect->right && rect->top < rect->bottom;
}

static bool copy_pixels(const struct context *dest, struct nt_rect rect,
                        const struct context *source, int64_t source_x, int64_t source_y)
{
    const struct bitmap *src = &source->bitmap->value.bitmap;
    const struct bitmap *dst = &dest->bitmap->value.bitmap;
    const int32_t width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (source_x < 0 || source_y < 0 || source_x + width > src->width ||
        source_y + height > src->height)
        return false;
    const bool backwards = src == dst && rect.top > source_y;
    for (int32_t row = 0; row < height; ++row) {
        const int32_t offset = backwards ? height - row - 1 : row;
        const uint32_t *input = pixel_at(src, (int32_t)source_x, (int32_t)source_y + offset);
        uint32_t *output = pixel_at(dst, rect.left, rect.top + offset);
        if (src->mono == dst->mono) {
            memmove(output, input, (size_t)width * sizeof(*output));
        } else {
            for (int32_t col = 0; col < width; ++col) {
                /* The background color of this profile is white. */
                output[col] = src->mono ? (input[col] == WHITE ? WHITE : dest->text_color) :
                              (input[col] == WHITE ? WHITE : BLACK);
            }
        }
    }
    return true;
}

int32_t NT_MS_ABI nt_BitBlt(uintptr_t dest_handle, int32_t x, int32_t y,
                           int32_t width, int32_t height, uintptr_t source_handle,
                           int32_t source_x, int32_t source_y, uint32_t rop)
{
    pthread_mutex_lock(&gdi_lock);
    const struct context *dest = find_context(dest_handle);
    const struct context *source = rop == SRCCOPY ? find_context(source_handle) : NULL;
    if (dest == NULL || (rop == SRCCOPY && source == NULL)) {
        finish(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    if (width < 0 || height < 0) {
        finish(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (rop != SRCCOPY && rop != PATCOPY && rop != BLACKNESS && rop != WHITENESS) {
        finish(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    struct nt_rect visible = bounds(dest);
    if (clip_transfer(&visible, x, y, width, height)) {
        if (rop == SRCCOPY) {
            if (!copy_pixels(dest, visible, source, (int64_t)source_x + visible.left - x,
                             (int64_t)source_y + visible.top - y)) {
                finish(NT_ERROR_INVALID_PARAMETER);
                return 0;
            }
        } else {
            const struct bitmap *bitmap = &dest->bitmap->value.bitmap;
            for (int32_t row = visible.top; row < visible.bottom; ++row) {
                for (int32_t col = visible.left; col < visible.right; ++col) {
                    if (rop == PATCOPY)
                        paint_brush(dest, col, row);
                    else
                        *pixel_at(bitmap, col, row) = rop == BLACKNESS ? BLACK : WHITE;
                }
            }
        }
    }
    finish(NT_ERROR_SUCCESS);
    return 1;
}
