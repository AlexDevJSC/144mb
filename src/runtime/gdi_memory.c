/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "gdi_ext.h"
#include "desktop.h"

#include <cairo-pdf.h>
#include <cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    WHITENESS = 0x00ff0062,
    SYSTEM_FONT = 13, DEFAULT_GUI_FONT = 17, LOGICAL_DPI = 96, MAX_TEXT = 65535
};

enum object_kind { BITMAP, PEN, BRUSH, FONT, CONTEXT };

struct gdi_object;

struct bitmap {
    int32_t width, height;
    bool mono;
    uint32_t *pixels;
    struct gdi_object *owner;
    bool dib, bottom_up;
};

struct ink {
    uint32_t color;
    int pattern;
};

struct font {
    cairo_scaled_font_t *scaled;
    int32_t weight;
    uint8_t italic, underline, strike, charset, pitch;
};

struct printer {
    int fd;
    FILE *spool;
    cairo_surface_t *document;
    bool page;
};

struct context {
    struct gdi_object *bitmap, *pen, *brush, *font, *original;
    struct nt_rect clip;
    bool clipped, thread_owned;
    pthread_t creator;
    uint32_t text_color;
    int32_t background_mode;
    int32_t device_width, device_height;
    bool device_mono;
    struct printer *printer;
};

struct gdi_object {
    uintptr_t handle;
    enum object_kind kind;
    unsigned int selections;
    union {
        struct bitmap bitmap;
        struct ink ink;
        struct font font;
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
    STOCK(8, PEN, BLACK, INK_NONE),
    [SYSTEM_FONT] = {STOCK_BASE + SYSTEM_FONT * HANDLE_STEP, FONT, 0, {.font = {0}}},
    [DEFAULT_GUI_FONT] = {STOCK_BASE + DEFAULT_GUI_FONT * HANDLE_STEP, FONT, 0, {.font = {0}}}
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
        return stock[slot].handle != 0 ? &stock[slot] : NULL;
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
    object->value.bitmap = (struct bitmap){.width = width, .height = height, .mono = mono, .pixels = pixels};
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
    --dc->font->selections;
    if (dc->printer != NULL) {
        if (dc->printer->document != NULL)
            cairo_surface_destroy(dc->printer->document);
        if (dc->printer->spool != NULL)
            fclose(dc->printer->spool);
        close(dc->printer->fd);
        free(dc->printer);
    }
    release_bitmap(dc->original);
    object->handle = 0;
}

_Static_assert(sizeof(struct nt_logfont) == 60, "LOGFONTA layout");
_Static_assert(sizeof(struct nt_text_metrics) == 56, "TEXTMETRICA layout");

static cairo_scaled_font_t *scale_font(cairo_font_face_t *face, double width, double height)
{
    cairo_matrix_t font_matrix, transform;
    cairo_matrix_init_scale(&font_matrix, width, height);
    cairo_matrix_init_identity(&transform);
    cairo_font_options_t *options = cairo_font_options_create();
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_NONE);
    cairo_scaled_font_t *font = cairo_scaled_font_create(face, &font_matrix, &transform, options);
    cairo_font_options_destroy(options);
    return font;
}

static struct font native_font(const struct nt_logfont *definition, const char *utf8)
{
    const size_t length = strnlen(definition->face, sizeof(definition->face));
    char *family = utf8 != NULL ? strdup(utf8) : nt_runtime_ansi_to_utf8(definition->face, length);
    if (family == NULL)
        return (struct font){0};
    const bool fixed = (definition->pitch & 3) == 1;
    const int32_t weight = definition->weight != 0 ? definition->weight : 400;
    const char *fallback = fixed ? "monospace" : ((definition->pitch & 0xf0) == 0x10 ? "serif" : "sans-serif");
    cairo_font_face_t *face = cairo_toy_font_face_create(family[0] != '\0' ? family : fallback,
        definition->italic ? CAIRO_FONT_SLANT_ITALIC : CAIRO_FONT_SLANT_NORMAL,
        weight >= 600 ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    free(family);
    const double pixels = definition->height != 0 ? fabs((double)definition->height) : 16.0;
    cairo_scaled_font_t *scaled = scale_font(face, pixels, pixels);
    if (cairo_scaled_font_status(scaled) == CAIRO_STATUS_SUCCESS &&
        (definition->height > 0 || definition->width != 0)) {
        cairo_font_extents_t extents;
        cairo_scaled_font_extents(scaled, &extents);
        const double height = definition->height > 0 && extents.height > 0 ?
                              pixels * pixels / extents.height : pixels;
        const double width = definition->width > 0 && extents.max_x_advance > 0 ?
                             pixels * definition->width / extents.max_x_advance : height;
        cairo_scaled_font_destroy(scaled);
        scaled = scale_font(face, width, height);
    }
    cairo_font_face_destroy(face);
    if (cairo_scaled_font_status(scaled) != CAIRO_STATUS_SUCCESS) {
        cairo_scaled_font_destroy(scaled);
        return (struct font){0};
    }
    return (struct font){scaled, weight, definition->italic, definition->underline,
                         definition->strike, definition->charset, definition->pitch};
}

static bool initialize_stock_fonts(void)
{
    if (stock[SYSTEM_FONT].value.font.scaled != NULL)
        return true;
    const struct nt_logfont definition = {.height = -13, .weight = 400, .charset = 1};
    struct font font = native_font(&definition, NULL);
    if (font.scaled == NULL)
        return false;
    stock[SYSTEM_FONT].value.font = font;
    stock[DEFAULT_GUI_FONT].value.font = font;
    stock[DEFAULT_GUI_FONT].value.font.scaled = cairo_scaled_font_reference(font.scaled);
    return true;
}

bool nt_runtime_apply_font(uintptr_t handle, cairo_t *target)
{
    pthread_mutex_lock(&gdi_lock);
    const struct gdi_object *object = find(handle);
    const bool available = object != NULL && object->kind == FONT && initialize_stock_fonts();
    if (available)
        cairo_set_scaled_font(target, object->value.font.scaled);
    pthread_mutex_unlock(&gdi_lock);
    return available;
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
    const struct context *device = reference != 0 ? find_context(reference) : NULL;
    if (reference != 0 && device == NULL) {
        finish(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    if (!initialize_stock_fonts()) {
        finish(NT_ERROR_NOT_ENOUGH_MEMORY);
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
        .bitmap = bitmap, .pen = &stock[7], .brush = &stock[0], .font = &stock[SYSTEM_FONT],
        .original = bitmap, .thread_owned = thread_owned, .creator = pthread_self(),
        .background_mode = OPAQUE, .device_width = device != NULL ? device->device_width : width,
        .device_height = device != NULL ? device->device_height : height,
        .device_mono = device != NULL ? device->device_mono : mono
    };
    bitmap->value.bitmap.owner = object;
    ++bitmap->selections;
    ++stock[7].selections;
    ++stock[0].selections;
    ++stock[SYSTEM_FONT].selections;
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
    if (index < 0 || index >= STOCK_COUNT || stock[index].handle == 0) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    if (stock[index].kind == FONT) {
        pthread_mutex_lock(&gdi_lock);
        const bool initialized = initialize_stock_fonts();
        finish(initialized ? NT_ERROR_SUCCESS : NT_ERROR_NOT_ENOUGH_MEMORY);
        if (!initialized)
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
        if (dc->printer != NULL || (bitmap->owner != NULL && bitmap->owner != dc_object) ||
            (object->selections != 0 && dc->bitmap != object)) {
            finish(NT_ERROR_ACCESS_DENIED);
            return 0;
        }
        slot = &dc->bitmap;
    } else if (object->kind == FONT)
        slot = &dc->font;
    else
        slot = object->kind == PEN ? &dc->pen : &dc->brush;
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
        else {
            if (object->kind == FONT)
                cairo_scaled_font_destroy(object->value.font.scaled);
            object->handle = 0;
        }
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
    const int32_t row = bitmap->bottom_up ? bitmap->height - 1 - y : y;
    return &bitmap->pixels[(size_t)row * (size_t)bitmap->width + (size_t)x];
}

static uint32_t swap_channels(uint32_t color)
{
    return (color & UINT32_C(0xff00ff00)) | ((color & 255u) << 16) | ((color >> 16) & 255u);
}

static uint32_t get_pixel(const struct bitmap *bitmap, int32_t x, int32_t y)
{
    const uint32_t color = *pixel_at(bitmap, x, y);
    return bitmap->dib ? swap_channels(color) : color;
}

static void store_pixel(const struct bitmap *bitmap, int32_t x, int32_t y, uint32_t color)
{
    *pixel_at(bitmap, x, y) = bitmap->dib ? swap_channels(color) : color;
}

uint32_t nt_runtime_read_pixel(uintptr_t handle, int32_t x, int32_t y)
{
    pthread_mutex_lock(&gdi_lock);
    const struct context *dc = find_context(handle);
    uint32_t color = UINT32_MAX;
    if (dc != NULL) {
        const struct bitmap *bitmap = &dc->bitmap->value.bitmap;
        if (contains((struct nt_rect){0, 0, bitmap->width, bitmap->height}, x, y))
            color = get_pixel(bitmap, x, y) & 0xffffffu;
    }
    finish(NT_ERROR_SUCCESS);
    return color;
}

bool nt_runtime_read_surface(uintptr_t handle, nt_surface_reader reader, void *context)
{
    pthread_mutex_lock(&gdi_lock);
    const struct context *dc = find_context(handle);
    bool result = false;
    if (dc != NULL) {
        const struct bitmap *bitmap = &dc->bitmap->value.bitmap;
        if (!bitmap->dib) {
            result = reader(bitmap->pixels, bitmap->width, bitmap->height, context);
        } else {
            const size_t pixels = (size_t)bitmap->width * (size_t)bitmap->height;
            uint32_t *copy = malloc(pixels * sizeof(*copy));
            if (copy != NULL) {
                for (int32_t y = 0; y < bitmap->height; ++y)
                    for (int32_t x = 0; x < bitmap->width; ++x)
                        copy[(size_t)y * (size_t)bitmap->width + (size_t)x] =
                            get_pixel(bitmap, x, y) & 0xffffffu;
                result = reader(copy, bitmap->width, bitmap->height, context);
                free(copy);
            }
        }
    }
    pthread_mutex_unlock(&gdi_lock);
    return result;
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
    store_pixel(bitmap, x, y, value);
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
        store_pixel(bitmap, x, y, display_color(bitmap, brush.color));
    else if (dc->background_mode == OPAQUE)
        store_pixel(bitmap, x, y, WHITE);
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
                        store_pixel(bitmap, x, y, display_color(bitmap, pen.color));
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
        if (src->mono == dst->mono && src->dib == dst->dib) {
            memmove(output, input, (size_t)width * sizeof(*output));
        } else {
            for (int32_t col = 0; col < width; ++col) {
                uint32_t color = src->dib ? swap_channels(input[col]) : input[col];
                if (src->mono != dst->mono) {
                    /* The background color of this profile is white. */
                    const uint32_t rgb = color & 0xffffffu;
                    color = rgb == WHITE ? WHITE : src->mono ? dest->text_color : BLACK;
                }
                output[col] = dst->dib ? swap_channels(color) : color;
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
                        store_pixel(bitmap, col, row, rop == BLACKNESS ? BLACK : WHITE);
                }
            }
        }
    }
    finish(NT_ERROR_SUCCESS);
    return 1;
}

static uintptr_t create_font(const struct nt_logfont *definition, const char *utf8)
{
    if (definition == NULL || definition->height < -1024 || definition->height > 1024 ||
        definition->width < 0 || definition->width > 1024 ||
        definition->weight < 0 || definition->weight > 1000) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (definition->escapement != 0 || definition->orientation != 0 ||
        (definition->charset != 0 && definition->charset != 1)) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    struct font font = native_font(definition, utf8);
    if (font.scaled == NULL) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    pthread_mutex_lock(&gdi_lock);
    struct gdi_object *object = allocate(FONT);
    if (object == NULL) {
        cairo_scaled_font_destroy(font.scaled);
        finish(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    object->value.font = font;
    const uintptr_t handle = object->handle;
    finish(NT_ERROR_SUCCESS);
    return handle;
}

uintptr_t NT_MS_ABI nt_CreateFontIndirectA(const struct nt_logfont *definition)
{
    return create_font(definition, NULL);
}

uintptr_t NT_MS_ABI nt_CreateFontA(int32_t height, int32_t width, int32_t escapement,
                                 int32_t orientation, int32_t weight, uint32_t italic,
                                 uint32_t underline, uint32_t strike, uint32_t charset,
                                 uint32_t out_precision, uint32_t clip_precision, uint32_t quality,
                                 uint32_t pitch, const char *face)
{
    if (charset > UINT8_MAX || out_precision > UINT8_MAX || clip_precision > UINT8_MAX ||
        quality > UINT8_MAX || pitch > UINT8_MAX) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    struct nt_logfont definition = {
        height, width, escapement, orientation, weight, italic != 0, underline != 0, strike != 0,
        (uint8_t)charset, (uint8_t)out_precision, (uint8_t)clip_precision, (uint8_t)quality, (uint8_t)pitch, {0}
    };
    if (face != NULL)
        memcpy(definition.face, face, strnlen(face, sizeof(definition.face) - 1));
    return nt_CreateFontIndirectA(&definition);
}

static int32_t font_height(cairo_font_extents_t extents)
{
    return (int32_t)ceil(extents.ascent) + (int32_t)ceil(extents.descent);
}

int32_t NT_MS_ABI nt_GetTextMetricsA(uintptr_t handle, struct nt_text_metrics *metrics)
{
    if (metrics == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    pthread_mutex_lock(&gdi_lock);
    const struct context *dc = find_context(handle);
    if (dc == NULL) {
        finish(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    const struct font *font = &dc->font->value.font;
    cairo_font_extents_t vertical;
    cairo_text_extents_t alphabet;
    cairo_scaled_font_extents(font->scaled, &vertical);
    cairo_scaled_font_text_extents(font->scaled, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", &alphabet);
    if (cairo_scaled_font_status(font->scaled) != CAIRO_STATUS_SUCCESS) {
        finish(NT_ERROR_GEN_FAILURE);
        return 0;
    }
    const int32_t height = font_height(vertical);
    *metrics = (struct nt_text_metrics){
        .height = height, .ascent = (int32_t)ceil(vertical.ascent), .descent = (int32_t)ceil(vertical.descent),
        .external_leading = vertical.height > height ? (int32_t)ceil(vertical.height) - height : 0,
        .average_width = (int32_t)ceil(alphabet.x_advance / 52), .max_width = (int32_t)ceil(vertical.max_x_advance),
        .weight = font->weight, .dpi_x = LOGICAL_DPI, .dpi_y = LOGICAL_DPI, .first = 0x20, .last = 0xff,
        .replacement = '?', .break_char = ' ', .italic = font->italic, .underline = font->underline,
        .strike = font->strike, .pitch = (font->pitch & 3) == 1 ? 2 : 3, .charset = font->charset
    };
    finish(NT_ERROR_SUCCESS);
    return 1;
}

static int32_t text_extent(uintptr_t handle, char *utf8, struct nt_size *size)
{
    if (utf8 == NULL) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    pthread_mutex_lock(&gdi_lock);
    const struct context *dc = find_context(handle);
    uint32_t error = NT_ERROR_INVALID_HANDLE;
    if (dc != NULL) {
        cairo_scaled_font_t *font = dc->font->value.font.scaled;
        cairo_font_extents_t vertical;
        cairo_text_extents_t horizontal;
        cairo_scaled_font_extents(font, &vertical);
        cairo_scaled_font_text_extents(font, utf8, &horizontal);
        error = NT_ERROR_GEN_FAILURE;
        if (cairo_scaled_font_status(font) == CAIRO_STATUS_SUCCESS) {
            *size = (struct nt_size){(int32_t)ceil(horizontal.x_advance), font_height(vertical)};
            error = NT_ERROR_SUCCESS;
        }
    }
    finish(error);
    free(utf8);
    return error == NT_ERROR_SUCCESS;
}

int32_t NT_MS_ABI nt_GetTextExtentPoint32A(uintptr_t dc, const char *text, int32_t count,
                                         struct nt_size *size)
{
    if (text == NULL || count < 0 || count > MAX_TEXT || size == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return text_extent(dc, nt_runtime_ansi_to_utf8(text, (size_t)count), size);
}

int32_t NT_MS_ABI nt_GetTextExtentPoint32W(uintptr_t dc, const uint16_t *text, int32_t count,
                                         struct nt_size *size)
{
    if (text == NULL || count < 0 || count > MAX_TEXT || size == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return text_extent(dc, nt_runtime_utf16_to_utf8(text, (size_t)count), size);
}

int32_t NT_MS_ABI nt_GetDeviceCaps(uintptr_t handle, int32_t index)
{
    pthread_mutex_lock(&gdi_lock);
    const struct context *dc = find_context(handle);
    int32_t result = 0;
    if (dc != NULL) {
        switch (index) {
        case 2: result = dc->printer != NULL ? 2 : 1; break; /* TECHNOLOGY */
        case 8: result = dc->device_width; break; /* HORZRES */
        case 10: result = dc->device_height; break; /* VERTRES */
        case 12: result = dc->device_mono ? 1 : 32; break; /* BITSPIXEL */
        case 14: result = 1; break; /* PLANES */
        case 38: result = 1; break; /* RASTERCAPS, RC_BITBLT */
        case 88: case 90: result = LOGICAL_DPI; break;
        default: break;
        }
    }
    finish(dc != NULL ? NT_ERROR_SUCCESS : NT_ERROR_INVALID_HANDLE);
    return result;
}

struct draw_mask {
    cairo_surface_t *surface;
    cairo_t *draw;
    struct nt_rect clip;
};

static struct draw_mask create_mask(struct nt_rect clip)
{
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_A8,
                                                         clip.right - clip.left, clip.bottom - clip.top);
    cairo_t *draw = cairo_create(surface);
    cairo_set_antialias(draw, CAIRO_ANTIALIAS_NONE);
    cairo_set_source_rgba(draw, 1, 1, 1, 1);
    cairo_translate(draw, -clip.left, -clip.top);
    return (struct draw_mask){surface, draw, clip};
}

static void free_mask(struct draw_mask mask)
{
    cairo_destroy(mask.draw);
    cairo_surface_destroy(mask.surface);
}

static bool mask_ok(struct draw_mask mask)
{
    return cairo_status(mask.draw) == CAIRO_STATUS_SUCCESS &&
           cairo_surface_status(mask.surface) == CAIRO_STATUS_SUCCESS;
}

static void apply_mask(const struct context *dc, struct draw_mask mask, bool brush, uint32_t color)
{
    cairo_surface_flush(mask.surface);
    const unsigned char *bits = cairo_image_surface_get_data(mask.surface);
    const int stride = cairo_image_surface_get_stride(mask.surface);
    const struct bitmap *bitmap = &dc->bitmap->value.bitmap;
    const uint32_t pixel = display_color(bitmap, color);
    for (int y = mask.clip.top; y < mask.clip.bottom; ++y) {
        const unsigned char *row = bits + (y - mask.clip.top) * stride;
        for (int x = mask.clip.left; x < mask.clip.right; ++x) {
            if (row[x - mask.clip.left] != 0) {
                if (brush)
                    paint_brush(dc, x, y);
                else
                    store_pixel(bitmap, x, y, pixel);
            }
        }
    }
}

static bool paint_path(const struct context *dc, struct draw_mask mask, bool fill)
{
    const struct ink pen = dc->pen->value.ink;
    if (fill && dc->brush->value.ink.pattern != INK_NONE) {
        cairo_set_fill_rule(mask.draw, CAIRO_FILL_RULE_EVEN_ODD);
        cairo_fill_preserve(mask.draw);
        if (!mask_ok(mask))
            return false;
        apply_mask(dc, mask, true, 0);
        cairo_set_operator(mask.draw, CAIRO_OPERATOR_CLEAR);
        cairo_paint(mask.draw);
        cairo_set_operator(mask.draw, CAIRO_OPERATOR_OVER);
    }
    if (pen.pattern != INK_NONE) {
        cairo_set_line_width(mask.draw, 1);
        cairo_stroke(mask.draw);
        if (!mask_ok(mask))
            return false;
        apply_mask(dc, mask, false, pen.color);
    }
    return mask_ok(mask);
}

int32_t NT_MS_ABI nt_Ellipse(uintptr_t handle, int32_t left, int32_t top,
                            int32_t right, int32_t bottom)
{
    pthread_mutex_lock(&gdi_lock);
    const struct context *dc = find_context(handle);
    if (dc == NULL) {
        finish(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    if (left >= right || top >= bottom) {
        finish(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    struct nt_rect clip = bounds(dc);
    if (!nt_IntersectRect(&clip, &clip, &(struct nt_rect){left, top, right, bottom})) {
        finish(NT_ERROR_SUCCESS);
        return 1;
    }
    const double horizontal = ((double)right - left) / 2;
    const double vertical = ((double)bottom - top) / 2;
    const struct draw_mask mask = create_mask(clip);
    cairo_save(mask.draw);
    cairo_translate(mask.draw, left + horizontal, top + vertical);
    cairo_scale(mask.draw, horizontal, vertical);
    cairo_arc(mask.draw, 0, 0, 1, 0, 6.28318530717958647692);
    cairo_restore(mask.draw);
    const bool painted = paint_path(dc, mask, true);
    free_mask(mask);
    finish(painted ? NT_ERROR_SUCCESS : NT_ERROR_GEN_FAILURE);
    return painted;
}

static int32_t draw_points(uintptr_t handle, const struct nt_point *points, int32_t count, bool close)
{
    if (points == NULL || count < (close ? 3 : 2) || count > 4096) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    pthread_mutex_lock(&gdi_lock);
    const struct context *dc = find_context(handle);
    if (dc == NULL) {
        finish(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    const struct nt_rect clip = bounds(dc);
    if (clip.left >= clip.right || clip.top >= clip.bottom) {
        finish(NT_ERROR_SUCCESS);
        return 1;
    }
    const struct draw_mask mask = create_mask(clip);
    cairo_move_to(mask.draw, points[0].x + 0.5, points[0].y + 0.5);
    for (int32_t i = 1; i < count; ++i)
        cairo_line_to(mask.draw, points[i].x + 0.5, points[i].y + 0.5);
    if (close)
        cairo_close_path(mask.draw);
    const bool painted = paint_path(dc, mask, close);
    free_mask(mask);
    finish(painted ? NT_ERROR_SUCCESS : NT_ERROR_GEN_FAILURE);
    return painted;
}

int32_t NT_MS_ABI nt_Polygon(uintptr_t dc, const struct nt_point *points, int32_t count)
{
    return draw_points(dc, points, count, true);
}

int32_t NT_MS_ABI nt_Polyline(uintptr_t dc, const struct nt_point *points, int32_t count)
{
    return draw_points(dc, points, count, false);
}

int32_t NT_MS_ABI nt_ExtTextOutW(uintptr_t handle, int32_t x, int32_t y, uint32_t options,
                                const struct nt_rect *rect, const uint16_t *text,
                                uint32_t count, const int32_t *advances)
{
    enum { ETO_OPAQUE = 2, ETO_CLIPPED = 4 };
    if ((options & ~(ETO_OPAQUE | ETO_CLIPPED)) != 0 || advances != NULL) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    if ((options != 0 && rect == NULL) || (count != 0 && text == NULL) || count > MAX_TEXT) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    char *utf8 = nt_runtime_utf16_to_utf8(text, count);
    if (utf8 == NULL) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    pthread_mutex_lock(&gdi_lock);
    const struct context *dc = find_context(handle);
    if (dc == NULL) {
        free(utf8);
        finish(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    const struct nt_rect visible = bounds(dc);
    struct nt_rect clip = visible;
    if ((options & ETO_CLIPPED) != 0)
        nt_IntersectRect(&clip, &clip, rect);
    struct draw_mask mask = {0};
    bool painted = true;
    if (count != 0 && clip.left < clip.right && clip.top < clip.bottom) {
        mask = create_mask(clip);
        const struct font *font = &dc->font->value.font;
        cairo_set_scaled_font(mask.draw, font->scaled);
        cairo_font_extents_t extents;
        cairo_font_extents(mask.draw, &extents);
        const double baseline = y + ceil(extents.ascent);
        cairo_move_to(mask.draw, x, baseline);
        cairo_show_text(mask.draw, utf8);
        if (font->underline || font->strike) {
            cairo_text_extents_t text_metrics;
            cairo_text_extents(mask.draw, utf8, &text_metrics);
            if (font->underline)
                cairo_rectangle(mask.draw, x, baseline + 1, text_metrics.x_advance, 1);
            if (font->strike)
                cairo_rectangle(mask.draw, x, baseline - ceil(extents.ascent / 3), text_metrics.x_advance, 1);
            cairo_fill(mask.draw);
        }
        painted = mask_ok(mask);
    }
    if (painted && (options & ETO_OPAQUE) != 0) {
        struct nt_rect background;
        if (nt_IntersectRect(&background, &visible, rect)) {
            const struct bitmap *bitmap = &dc->bitmap->value.bitmap;
            for (int row = background.top; row < background.bottom; ++row) {
                for (int col = background.left; col < background.right; ++col)
                    store_pixel(bitmap, col, row, WHITE);
            }
        }
    }
    if (mask.draw != NULL) {
        if (painted)
            apply_mask(dc, mask, false, dc->text_color);
        free_mask(mask);
    }
    free(utf8);
    finish(painted ? NT_ERROR_SUCCESS : NT_ERROR_GEN_FAILURE);
    return painted;
}

_Static_assert(sizeof(struct nt_docinfo) == 40, "DOCINFOA layout");

uintptr_t nt_runtime_create_pdf_printer(int fd, int32_t width, int32_t height)
{
    const int duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0)
        return 0;
    struct printer *printer = calloc(1, sizeof(*printer));
    const uintptr_t handle = printer != NULL ? nt_runtime_create_surface(width, height) : 0;
    if (handle == 0) {
        close(duplicate);
        free(printer);
        return 0;
    }
    printer->fd = duplicate;
    pthread_mutex_lock(&gdi_lock);
    find_context(handle)->printer = printer;
    finish(NT_ERROR_SUCCESS);
    return handle;
}

static cairo_status_t spool_bytes(void *output, const unsigned char *bytes, unsigned int length)
{
    return fwrite(bytes, 1, length, output) == length ? CAIRO_STATUS_SUCCESS : CAIRO_STATUS_WRITE_ERROR;
}

static int32_t next_job = 1;

int32_t NT_MS_ABI nt_StartDocA(uintptr_t handle, const struct nt_docinfo *info)
{
    if (info == NULL || info->size != sizeof(*info) || info->name == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return -1;
    }
    if (info->type != 0 || info->data_type != NULL ||
        (info->output != NULL && strcmp(info->output, "FILE:") != 0)) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return -1;
    }
    const size_t length = strnlen(info->name, 4096);
    if (length == 4096) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return -1;
    }
    char *name = nt_runtime_ansi_to_utf8(info->name, length);
    if (name == NULL) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return -1;
    }
    pthread_mutex_lock(&gdi_lock);
    const struct context *dc = find_context(handle);
    struct printer *printer = dc != NULL ? dc->printer : NULL;
    if (printer == NULL || printer->document != NULL || next_job == INT32_MAX) {
        free(name);
        finish(printer == NULL ? NT_ERROR_INVALID_HANDLE : NT_ERROR_INVALID_PARAMETER);
        return -1;
    }
    FILE *spool = tmpfile();
    if (spool == NULL) {
        free(name);
        finish(NT_ERROR_GEN_FAILURE);
        return -1;
    }
    cairo_surface_t *document = cairo_pdf_surface_create_for_stream(spool_bytes, spool,
        dc->device_width * 72.0 / LOGICAL_DPI, dc->device_height * 72.0 / LOGICAL_DPI);
    cairo_pdf_surface_restrict_to_version(document, CAIRO_PDF_VERSION_1_4);
    cairo_pdf_surface_set_metadata(document, CAIRO_PDF_METADATA_TITLE, name);
    free(name);
    if (cairo_surface_status(document) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(document);
        fclose(spool);
        finish(NT_ERROR_GEN_FAILURE);
        return -1;
    }
    printer->spool = spool;
    printer->document = document;
    const int32_t id = next_job++;
    finish(NT_ERROR_SUCCESS);
    return id;
}

int32_t NT_MS_ABI nt_StartPage(uintptr_t handle)
{
    pthread_mutex_lock(&gdi_lock);
    struct context *dc = find_context(handle);
    struct printer *printer = dc != NULL ? dc->printer : NULL;
    if (printer == NULL || printer->document == NULL || printer->page) {
        finish(printer == NULL ? NT_ERROR_INVALID_HANDLE : NT_ERROR_INVALID_PARAMETER);
        return -1;
    }
    const struct bitmap *bitmap = &dc->bitmap->value.bitmap;
    const size_t count = (size_t)bitmap->width * (size_t)bitmap->height;
    for (size_t i = 0; i < count; ++i)
        bitmap->pixels[i] = WHITE;
    dc->clipped = false;
    printer->page = true;
    finish(NT_ERROR_SUCCESS);
    return 1;
}

int32_t NT_MS_ABI nt_EndPage(uintptr_t handle)
{
    pthread_mutex_lock(&gdi_lock);
    const struct context *dc = find_context(handle);
    struct printer *printer = dc != NULL ? dc->printer : NULL;
    if (printer == NULL || printer->document == NULL || !printer->page) {
        finish(printer == NULL ? NT_ERROR_INVALID_HANDLE : NT_ERROR_INVALID_PARAMETER);
        return -1;
    }
    const struct bitmap *bitmap = &dc->bitmap->value.bitmap;
    cairo_surface_t *raster = cairo_image_surface_create(CAIRO_FORMAT_RGB24, bitmap->width, bitmap->height);
    bool copied = cairo_surface_status(raster) == CAIRO_STATUS_SUCCESS;
    if (copied) {
        unsigned char *bytes = cairo_image_surface_get_data(raster);
        const int stride = cairo_image_surface_get_stride(raster);
        for (int y = 0; y < bitmap->height; ++y) {
            uint32_t *row = (uint32_t *)(bytes + y * stride);
            for (int x = 0; x < bitmap->width; ++x) {
                const uint32_t color = get_pixel(bitmap, x, y) & 0xffffffu;
                row[x] = (color & 255) << 16 | (color & 0xff00) | (color >> 16 & 255);
            }
        }
        cairo_surface_mark_dirty(raster);
        cairo_t *draw = cairo_create(printer->document);
        cairo_scale(draw, 72.0 / LOGICAL_DPI, 72.0 / LOGICAL_DPI);
        cairo_set_source_surface(draw, raster, 0, 0);
        cairo_paint(draw);
        cairo_show_page(draw);
        copied = cairo_status(draw) == CAIRO_STATUS_SUCCESS;
        cairo_destroy(draw);
    }
    cairo_surface_destroy(raster);
    if (copied)
        printer->page = false;
    finish(copied ? NT_ERROR_SUCCESS : NT_ERROR_GEN_FAILURE);
    return copied ? 1 : -1;
}

static bool copy_print_job(const struct printer *printer)
{
    if (fflush(printer->spool) != 0 || fseeko(printer->spool, 0, SEEK_SET) != 0 ||
        ftruncate(printer->fd, 0) != 0)
        return false;
    unsigned char buffer[16384];
    off_t offset = 0;
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), printer->spool)) != 0) {
        size_t written = 0;
        while (written < bytes) {
            const ssize_t step = pwrite(printer->fd, buffer + written, bytes - written, offset);
            if (step < 0 && errno == EINTR)
                continue;
            if (step <= 0)
                return false;
            written += (size_t)step;
            offset += step;
        }
    }
    return !ferror(printer->spool) && fdatasync(printer->fd) == 0;
}

int32_t NT_MS_ABI nt_EndDoc(uintptr_t handle)
{
    pthread_mutex_lock(&gdi_lock);
    const struct context *dc = find_context(handle);
    struct printer *printer = dc != NULL ? dc->printer : NULL;
    if (printer == NULL || printer->document == NULL || printer->page) {
        finish(printer == NULL ? NT_ERROR_INVALID_HANDLE : NT_ERROR_INVALID_PARAMETER);
        return -1;
    }
    cairo_surface_finish(printer->document);
    const bool printed = cairo_surface_status(printer->document) == CAIRO_STATUS_SUCCESS &&
                         copy_print_job(printer);
    cairo_surface_destroy(printer->document);
    fclose(printer->spool);
    printer->document = NULL;
    printer->spool = NULL;
    finish(printed ? NT_ERROR_SUCCESS : NT_ERROR_GEN_FAILURE);
    return printed ? 1 : -1;
}

uintptr_t NT_MS_ABI nt_CreateFontW(int32_t height, int32_t width, int32_t escapement, int32_t orientation,
                                  int32_t weight, uint32_t italic, uint32_t underline, uint32_t strike,
                                  uint32_t charset, uint32_t output_precision, uint32_t clip_precision,
                                  uint32_t quality, uint32_t pitch, const uint16_t *face)
{
    if (charset > UINT8_MAX || output_precision > UINT8_MAX || clip_precision > UINT8_MAX ||
        quality > UINT8_MAX || pitch > UINT8_MAX) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const struct nt_logfont definition = {
        height, width, escapement, orientation, weight, italic != 0, underline != 0, strike != 0,
        (uint8_t)charset, (uint8_t)output_precision, (uint8_t)clip_precision, (uint8_t)quality, (uint8_t)pitch, {0}
    };
    char *utf8 = face != NULL ? nt_runtime_utf16_to_utf8(face, nt_runtime_wide_length(face, 31)) : NULL;
    if (face != NULL && utf8 == NULL) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    const uintptr_t handle = create_font(&definition, utf8);
    free(utf8);
    return handle;
}

uintptr_t NT_MS_ABI nt_CreateDIBSection(uintptr_t handle, const struct nt_bitmap_info_header *info,
                                        uint32_t usage, void **pixels, uintptr_t section, uint32_t offset)
{
    _Static_assert(sizeof(struct nt_bitmap_info_header) == 40, "BITMAPINFOHEADER layout");
    if (pixels != NULL)
        *pixels = NULL;
    if (info == NULL || pixels == NULL || info->size < sizeof(*info) || info->height == INT32_MIN ||
        info->height == 0 || info->width <= 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const int32_t height = info->height < 0 ? -info->height : info->height;
    if (usage != 0 || section != 0 || offset != 0 || info->planes != 1 ||
        info->bits != 32 || info->compression != 0 || !valid_size(info->width, height)) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    pthread_mutex_lock(&gdi_lock);
    if (handle != 0 && find_context(handle) == NULL) {
        finish(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    struct gdi_object *bitmap = allocate_bitmap(info->width, height, false);
    if (bitmap == NULL) {
        finish(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    bitmap->value.bitmap.dib = true;
    bitmap->value.bitmap.bottom_up = info->height > 0;
    *pixels = bitmap->value.bitmap.pixels;
    const uintptr_t result = bitmap->handle;
    finish(NT_ERROR_SUCCESS);
    return result;
}

int32_t NT_MS_ABI nt_GdiFlush(void)
{
    /* Raster operations have completed when their lock is released. */
    pthread_mutex_lock(&gdi_lock);
    finish(NT_ERROR_SUCCESS);
    return 1;
}

int32_t NT_MS_ABI nt_FillRect(uintptr_t handle, const struct nt_rect *rect, uintptr_t brush)
{
    if (rect == NULL || brush == 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const bool system = brush <= 32;
    const uint32_t color = system ? nt_GetSysColor((int32_t)brush - 1) : 0;
    pthread_mutex_lock(&gdi_lock);
    const struct context *dc = find_context(handle);
    const struct gdi_object *object = system ? NULL : find(brush);
    if (dc == NULL || (!system && (object == NULL || object->kind != BRUSH))) {
        finish(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    const struct ink ink = system ? (struct ink){color, INK_SOLID} : object->value.ink;
    struct nt_rect visible = bounds(dc);
    if (nt_IntersectRect(&visible, &visible, rect) && ink.pattern != INK_NONE) {
        const struct bitmap *bitmap = &dc->bitmap->value.bitmap;
        for (int32_t y = visible.top; y < visible.bottom; ++y) {
            for (int32_t x = visible.left; x < visible.right; ++x) {
                const bool inked = hatch_pixel(ink.pattern, x, y);
                if (inked || dc->background_mode == OPAQUE)
                    store_pixel(bitmap, x, y, display_color(bitmap, inked ? ink.color : WHITE));
            }
        }
    }
    finish(NT_ERROR_SUCCESS);
    return 1;
}

int32_t NT_MS_ABI nt_DrawTextW(uintptr_t dc, uint16_t *text, int32_t count, struct nt_rect *rect, uint32_t flags)
{
    enum { CENTER = 1, RIGHT = 2, VCENTER = 4, BOTTOM = 8, SINGLE = 32, NOCLIP = 256,
           CALCULATE = 1024, NOPREFIX = 2048 };
    const uint32_t allowed = CENTER | RIGHT | VCENTER | BOTTOM | SINGLE | NOCLIP | CALCULATE | NOPREFIX;
    if (text == NULL || rect == NULL || count < -1 || count > MAX_TEXT || (flags & ~allowed) != 0 ||
        ((flags & (VCENTER | BOTTOM)) != 0 && (flags & SINGLE) == 0)) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    const size_t length = count < 0 ? nt_runtime_wide_length(text, MAX_TEXT) : (size_t)count;
    uint16_t *processed = malloc((length + 1) * sizeof(*processed));
    if (processed == NULL) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    size_t units = 0;
    for (size_t i = 0; i < length && text[i] != 0; ++i) {
        uint16_t ch = text[i];
        if (ch == '&' && (flags & NOPREFIX) == 0) {
            if (i + 1 == length || text[i + 1] != '&')
                continue;
            ++i;
        }
        if (ch == '\r' && i + 1 < length && text[i + 1] == '\n')
            continue;
        if (ch == '\r')
            ch = '\n';
        processed[units++] = ch == '\n' && (flags & SINGLE) != 0 ? ' ' : ch;
    }
    int32_t height = 0, max_width = 0;
    bool ok = true;
    size_t start = 0;
    while (start < units) {
        size_t end = start;
        while (end < units && processed[end] != '\n')
            ++end;
        struct nt_size size;
        const uint16_t sample = ' ';
        if (!nt_GetTextExtentPoint32W(dc, start != end ? processed + start : &sample,
                                     start != end ? (int32_t)(end - start) : 1, &size)) {
            ok = false;
            break;
        }
        if (start == end)
            size.width = 0;
        if (size.width > max_width)
            max_width = size.width;
        int64_t x = rect->left, y = (int64_t)rect->top + height;
        const int64_t width = (int64_t)rect->right - rect->left, available = (int64_t)rect->bottom - rect->top;
        if ((flags & CENTER) != 0)
            x += (width - size.width) / 2;
        else if ((flags & RIGHT) != 0)
            x += width - size.width;
        if ((flags & VCENTER) != 0)
            y += (available - size.height) / 2;
        else if ((flags & BOTTOM) != 0)
            y += available - size.height;
        if (height > INT32_MAX - size.height || x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX ||
            ((flags & CALCULATE) == 0 && !nt_ExtTextOutW(dc, (int32_t)x, (int32_t)y,
                (flags & NOCLIP) == 0 ? 4 : 0, rect, processed + start, (uint32_t)(end - start), NULL))) {
            ok = false;
            break;
        }
        height += size.height;
        start = end + 1;
    }
    free(processed);
    if (ok && (flags & CALCULATE) != 0) {
        const int64_t right = (int64_t)rect->left + max_width, bottom = (int64_t)rect->top + height;
        if (right > INT32_MAX || bottom > INT32_MAX) {
            nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
            return 0;
        }
        rect->right = (int32_t)right;
        rect->bottom = (int32_t)bottom;
    }
    return ok ? height : 0;
}

static uint32_t blend_pixel(uint32_t dst, uint32_t src, unsigned int global, bool per_pixel)
{
    const unsigned int source_alpha = per_pixel ? src >> 24 : 255;
    const unsigned int alpha = (source_alpha * global + 127) / 255;
    const unsigned int remainder = 255 - alpha;
    uint32_t result = (alpha + ((dst >> 24) * remainder + 127) / 255) << 24;
    for (unsigned int shift = 0; shift < 24; shift += 8) {
        const unsigned int value = (((src >> shift) & 255) * global +
                                    ((dst >> shift) & 255) * remainder + 127) / 255;
        result |= (value < 255 ? value : 255) << shift;
    }
    return result;
}

int32_t NT_MS_ABI nt_AlphaBlend(uintptr_t destination, int32_t x, int32_t y, int32_t width, int32_t height,
                                uintptr_t source, int32_t sx, int32_t sy, int32_t sw, int32_t sh, uint32_t blend)
{
    const bool per_pixel = (blend >> 24) != 0;
    if (width <= 0 || height <= 0 || sw <= 0 || sh <= 0 || sx < 0 || sy < 0 ||
        (blend & UINT32_C(0xfe00ffff)) != 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const unsigned int alpha = (blend >> 16) & 255;
    pthread_mutex_lock(&gdi_lock);
    const struct context *dest = find_context(destination), *src = find_context(source);
    if (dest == NULL || src == NULL) {
        finish(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    const struct bitmap *input = &src->bitmap->value.bitmap, *output = &dest->bitmap->value.bitmap;
    const int64_t sr = (int64_t)sx + sw, sb = (int64_t)sy + sh;
    if (sr > input->width || sb > input->height || (per_pixel && !input->dib) ||
        (input == output && (int64_t)x + width > sx && sr > x && (int64_t)y + height > sy && sb > y)) {
        finish(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    struct nt_rect visible = bounds(dest);
    if (clip_transfer(&visible, x, y, width, height)) {
        for (int32_t row = visible.top; row < visible.bottom; ++row) {
            const int32_t source_y = sy + (int32_t)(((int64_t)row - y) * sh / height);
            for (int32_t col = visible.left; col < visible.right; ++col) {
                const int32_t source_x = sx + (int32_t)(((int64_t)col - x) * sw / width);
                const uint32_t value = blend_pixel(get_pixel(output, col, row),
                    get_pixel(input, source_x, source_y), alpha, per_pixel);
                store_pixel(output, col, row, output->dib ? value : display_color(output, value & 0xffffffu));
            }
        }
    }
    finish(NT_ERROR_SUCCESS);
    return 1;
}
