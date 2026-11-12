/* SPDX-License-Identifier: MIT */
#include "wic.h"
#include "user_folders.h"
#include "winbase.h"

#include <SDL_image.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct nt_folder_id guid;
typedef int32_t result;
#define H(value) ((result)UINT32_C(value))
enum wic_kind { FACTORY, DECODER, FRAME, CONVERTER };
enum pixel_format { RGBA, BGRA, RGB, BGR, PIXEL_FORMAT_COUNT };
enum { MAX_IMAGE_BYTES = 64 * 1024 * 1024 };

static const guid iid_unknown = {0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
static const guid iid_source = {0x00000120,0xa8f2,0x4877,{0xba,0x0a,0xfd,0x2b,0x66,0x45,0xfb,0x94}};
static const guid cls_factory = {0xcacaf262,0x9370,0x4615,{0xa1,0x3b,0x9f,0x55,0x39,0xda,0x4c,0x0a}};
static const guid cls_factory2 = {0x317d06e8,0x5f24,0x433d,{0xbd,0xf7,0x79,0xce,0x68,0xd8,0xab,0xc2}};
static const guid iids[] = {
    [FACTORY] = {0xec5ec8a9,0xc395,0x4314,{0x9c,0x77,0x54,0xd7,0xa9,0x35,0xff,0x70}},
    [DECODER] = {0x9edde9e7,0x8dee,0x47ea,{0x99,0xdf,0xe6,0xfa,0xf2,0xed,0x44,0xbf}},
    [FRAME] = {0x3b16811b,0x6a43,0x4ec9,{0xa8,0x13,0x3d,0x93,0x0c,0x13,0xb9,0x40}},
    [CONVERTER] = {0x00000301,0xa8f2,0x4877,{0xba,0x0a,0xfd,0x2b,0x66,0x45,0xfb,0x94}},
};
static const guid pixel_formats[] = {
    [RGBA] = {0xf5c7ad2d,0x6a8d,0x43dd,{0xa7,0xa8,0xa2,0x99,0x35,0x26,0x1a,0xe9}},
    [BGRA] = {0x6fddc324,0x4e03,0x4bfe,{0xb1,0x85,0x3d,0x77,0x76,0x8d,0xc9,0x0f}},
    [RGB] = {0x6fddc324,0x4e03,0x4bfe,{0xb1,0x85,0x3d,0x77,0x76,0x8d,0xc9,0x0d}},
    [BGR] = {0x6fddc324,0x4e03,0x4bfe,{0xb1,0x85,0x3d,0x77,0x76,0x8d,0xc9,0x0c}},
};
static const struct {
    const char *type;
    int (*recognize)(SDL_RWops *);
    guid container;
} codecs[] = {
    {"BMP", IMG_isBMP, {0x0af1d87e,0xfcfe,0x4188,{0xbd,0xeb,0xa7,0x90,0x64,0x71,0xcb,0xe3}}},
    {"PNG", IMG_isPNG, {0x1b7cfaf4,0x713f,0x473c,{0xbb,0xcd,0x61,0x37,0x42,0x5f,0xae,0xaf}}},
    {"JPG", IMG_isJPG, {0x19e4a5aa,0x5662,0x4fc5,{0xa0,0xc0,0x17,0x58,0x02,0x8e,0x10,0x57}}},
    {"TIF", IMG_isTIF, {0x163bcc30,0xe2e9,0x4f0b,{0x96,0x1d,0xa3,0xe9,0xfd,0xb7,0x88,0xa3}}},
    {"GIF", IMG_isGIF, {0x1f8a5601,0x7d4d,0x4cbd,{0x9c,0x82,0x1b,0xc8,0xd4,0xee,0xb9,0xa5}}},
};

struct wic_image {
    atomic_uint refs;
    SDL_Surface *pixels;
    guid container;
};
struct wic_object {
    const nt_guest_proc *vtable;
    atomic_uint refs;
    enum wic_kind kind;
    pthread_mutex_t lock;
    struct wic_image *image;
    enum pixel_format format;
};
struct wic_rect { int32_t x, y, width, height; };

static bool same_id(const guid *a, const guid *b)
{
    return a && !memcmp(a, b, sizeof(*a));
}
static void image_release(struct wic_image *image)
{
    if (image && atomic_fetch_sub(&image->refs, 1) == 1) {
        SDL_FreeSurface(image->pixels);
        free(image);
    }
}
static uint32_t NT_MS_ABI object_add_ref(struct wic_object *object)
{
    return atomic_fetch_add(&object->refs, 1) + 1;
}
static uint32_t NT_MS_ABI object_release(struct wic_object *object)
{
    uint32_t left = atomic_fetch_sub(&object->refs, 1) - 1;
    if (!left) {
        image_release(object->image);
        pthread_mutex_destroy(&object->lock);
        free(object);
    }
    return left;
}
static result NT_MS_ABI object_query(struct wic_object *object, const guid *iid, void **out)
{
    if (!out)
        return H(0x80004003);
    *out = NULL;
    if (!same_id(iid, &iid_unknown) && !same_id(iid, &iids[object->kind]) &&
        !((object->kind == FRAME || object->kind == CONVERTER) && same_id(iid, &iid_source)))
        return H(0x80004002);
    object_add_ref(object);
    *out = object;
    return 0;
}
static result NT_MS_ABI unsupported(void)
{
    return H(0x88982f81);
}

static result make_object(enum wic_kind kind, struct wic_image *image, void **out);

static result NT_MS_ABI factory_decode(struct wic_object *object, const uint16_t *filename,
        const guid *vendor, uint32_t access, uint32_t cache, void **out)
{
    (void)object;
    if (!out)
        return H(0x80004003);
    *out = NULL;
    if (!filename || (access != 0 && access != 0x80000000u) || cache > 1)
        return H(0x80070057);
    if (vendor)
        return H(0x88982f50);
    size_t length = nt_runtime_wide_length(filename, PATH_MAX);
    if (!length || length == PATH_MAX)
        return H(0x80070057);
    char *path = nt_runtime_utf16_to_utf8(filename, length);
    if (!path)
        return H(0x80070057);
    void *bytes = NULL;
    size_t size = 0;
    int error = nt_runtime_read_guest_file(path, MAX_IMAGE_BYTES, &bytes, &size);
    free(path);
    if (error)
        return error == ENOMEM ? H(0x8007000e) : error == ENOENT ? H(0x80070002) : H(0x80004005);
    SDL_RWops *reader = SDL_RWFromConstMem(bytes, (int)size);
    if (!reader) {
        free(bytes);
        return H(0x88982f07);
    }
    size_t codec = 0;
    while (codec < sizeof(codecs) / sizeof(*codecs) && !codecs[codec].recognize(reader))
        ++codec;
    SDL_Surface *decoded = NULL;
    if (codec < sizeof(codecs) / sizeof(*codecs))
        decoded = IMG_LoadTyped_RW(reader, 1, codecs[codec].type);
    else
        SDL_RWclose(reader);
    free(bytes);
    if (!decoded)
        return H(0x88982f07);
    SDL_Surface *pixels = SDL_ConvertSurfaceFormat(decoded, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(decoded);
    if (!pixels)
        return H(0x8007000e);
    struct wic_image *image = malloc(sizeof(*image));
    if (!image) {
        SDL_FreeSurface(pixels);
        return H(0x8007000e);
    }
    atomic_init(&image->refs, 1);
    image->pixels = pixels;
    image->container = codecs[codec].container;
    result status = make_object(DECODER, image, out);
    image_release(image);
    return status;
}
static result NT_MS_ABI factory_converter(struct wic_object *object, void **out)
{
    (void)object;
    return make_object(CONVERTER, NULL, out);
}
static result NT_MS_ABI decoder_container(struct wic_object *object, guid *out)
{
    if (!out)
        return H(0x80004003);
    *out = object->image->container;
    return 0;
}
static result NT_MS_ABI decoder_frame_count(struct wic_object *object, uint32_t *out)
{
    (void)object;
    if (!out)
        return H(0x80004003);
    *out = 1;
    return 0;
}
static result NT_MS_ABI decoder_frame(struct wic_object *object, uint32_t index, void **out)
{
    if (!out)
        return H(0x80004003);
    *out = NULL;
    if (index != 0)
        return H(0x88982f62);
    return make_object(FRAME, object->image, out);
}

static struct wic_image *current_image(struct wic_object *object, enum pixel_format *format)
{
    pthread_mutex_lock(&object->lock);
    struct wic_image *image = object->image;
    *format = object->format;
    pthread_mutex_unlock(&object->lock);
    return image;
}
static result NT_MS_ABI source_size(struct wic_object *object, uint32_t *width, uint32_t *height)
{
    if (!width || !height)
        return H(0x80004003);
    enum pixel_format format;
    const struct wic_image *image = current_image(object, &format);
    if (!image)
        return H(0x88982f04);
    *width = (uint32_t)image->pixels->w;
    *height = (uint32_t)image->pixels->h;
    return 0;
}
static result NT_MS_ABI source_format(struct wic_object *object, guid *out)
{
    if (!out)
        return H(0x80004003);
    enum pixel_format format;
    if (!current_image(object, &format))
        return H(0x88982f04);
    *out = pixel_formats[format];
    return 0;
}
static result NT_MS_ABI source_resolution(struct wic_object *object, double *x, double *y)
{
    if (!x || !y)
        return H(0x80004003);
    enum pixel_format format;
    if (!current_image(object, &format))
        return H(0x88982f04);
    *x = *y = 96.0;
    return 0;
}
static result NT_MS_ABI source_palette(struct wic_object *object, void *palette)
{
    (void)object; (void)palette;
    return H(0x88982f45);
}
static result NT_MS_ABI source_copy(struct wic_object *object, const struct wic_rect *region,
        uint32_t stride, uint32_t capacity, unsigned char *out)
{
    enum pixel_format format;
    const struct wic_image *image = current_image(object, &format);
    if (!image)
        return H(0x88982f04);
    const SDL_Surface *surface = image->pixels;
    struct wic_rect rect = region ? *region : (struct wic_rect){0, 0, surface->w, surface->h};
    if (!out || rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0 ||
        rect.x > surface->w || rect.y > surface->h ||
        rect.width > surface->w - rect.x || rect.height > surface->h - rect.y)
        return H(0x80070057);
    const uint32_t channels = format == RGBA || format == BGRA ? 4 : 3;
    const size_t row_bytes = (size_t)rect.width * channels;
    if (stride < row_bytes || capacity < (size_t)(rect.height - 1) * stride + row_bytes)
        return H(0x88982f8c);
    for (int32_t y = 0; y < rect.height; ++y) {
        const unsigned char *input = (const unsigned char *)surface->pixels +
            (size_t)(rect.y + y) * surface->pitch + (size_t)rect.x * 4;
        unsigned char *row = out + (size_t)y * stride;
        if (format == RGBA) {
            memcpy(row, input, row_bytes);
        } else {
            const bool bgr = format == BGRA || format == BGR;
            for (int32_t x = 0; x < rect.width; ++x, input += 4, row += channels) {
                row[0] = input[bgr ? 2 : 0];
                row[1] = input[1];
                row[2] = input[bgr ? 0 : 2];
                if (channels == 4)
                    row[3] = input[3];
            }
        }
    }
    return 0;
}
static enum pixel_format find_format(const guid *format)
{
    for (enum pixel_format i = 0; i < PIXEL_FORMAT_COUNT; ++i)
        if (same_id(format, &pixel_formats[i]))
            return i;
    return PIXEL_FORMAT_COUNT;
}

static const nt_guest_proc frame_vtable[11], converter_vtable[10];

static result NT_MS_ABI converter_init(struct wic_object *object, struct wic_object *source,
        const guid *target, uint32_t dither, void *palette, double alpha, uint32_t palette_type)
{
    (void)alpha; (void)palette_type;
    if (!source || source == object || !target)
        return H(0x80070057);
    if (source->vtable != frame_vtable && source->vtable != converter_vtable)
        return H(0x88982f81);
    enum pixel_format format = find_format(target);
    if (format == PIXEL_FORMAT_COUNT || dither || palette)
        return H(0x88982f80);
    enum pixel_format source_format;
    struct wic_image *image = current_image(source, &source_format);
    if (!image)
        return H(0x88982f04);
    pthread_mutex_lock(&object->lock);
    if (object->image) {
        pthread_mutex_unlock(&object->lock);
        return H(0x88982f04);
    }
    object->image = image;
    object->format = format;
    atomic_fetch_add(&image->refs, 1);
    pthread_mutex_unlock(&object->lock);
    return 0;
}
static result NT_MS_ABI converter_can_convert(struct wic_object *object,
        const guid *source, const guid *target, int32_t *can)
{
    (void)object;
    if (!source || !target || !can)
        return H(0x80070057);
    *can = find_format(source) != PIXEL_FORMAT_COUNT && find_format(target) != PIXEL_FORMAT_COUNT;
    return 0;
}

#define METHOD(fn) ((nt_guest_proc)(fn))
#define METHODS_IUNKNOWN METHOD(object_query), METHOD(object_add_ref), METHOD(object_release)
#define BITMAP_SOURCE METHODS_IUNKNOWN, METHOD(source_size), METHOD(source_format), \
    METHOD(source_resolution), METHOD(source_palette), METHOD(source_copy)
#define NA METHOD(unsupported)
/* Slots follow the public IWICImagingFactory, IWICBitmapDecoder and IWICBitmapSource interfaces. */
static const nt_guest_proc factory_vtable[] = {
    METHODS_IUNKNOWN, METHOD(factory_decode), NA, NA, NA, NA, NA, NA,
    METHOD(factory_converter), NA, NA, NA, NA, NA, NA, NA, NA, NA, NA, NA, NA, NA, NA, NA, NA, NA,
};
static const nt_guest_proc decoder_vtable[] = {
    METHODS_IUNKNOWN, NA, NA, METHOD(decoder_container), NA, METHOD(source_palette),
    NA, NA, NA, NA, METHOD(decoder_frame_count), METHOD(decoder_frame),
};
static const nt_guest_proc frame_vtable[11] = {BITMAP_SOURCE, NA, NA, NA};
static const nt_guest_proc converter_vtable[10] = {BITMAP_SOURCE, METHOD(converter_init), METHOD(converter_can_convert)};
#undef NA
#undef BITMAP_SOURCE
#undef METHODS_IUNKNOWN
#undef METHOD

static result make_object(enum wic_kind kind, struct wic_image *image, void **out)
{
    static const nt_guest_proc *const tables[] = {factory_vtable, decoder_vtable, frame_vtable, converter_vtable};
    if (!out)
        return H(0x80004003);
    *out = NULL;
    struct wic_object *object = calloc(1, sizeof(*object));
    if (!object)
        return H(0x8007000e);
    if (pthread_mutex_init(&object->lock, NULL) != 0) {
        free(object);
        return H(0x8007000e);
    }
    object->vtable = tables[kind];
    object->kind = kind;
    object->format = RGBA;
    object->image = image;
    atomic_init(&object->refs, 1);
    if (image)
        atomic_fetch_add(&image->refs, 1);
    *out = object;
    return 0;
}

int32_t NT_MS_ABI nt_CoCreateInstance(const void *class_id, void *outer, uint32_t context,
                                      const void *iid, void **out)
{
    if (!out)
        return H(0x80004003);
    *out = NULL;
    if (!class_id || !iid)
        return H(0x80070057);
    if (!nt_runtime_com_initialized())
        return H(0x800401f0);
    if (outer)
        return H(0x80040110);
    if (!(context & 1) || (!same_id(class_id, &cls_factory) && !same_id(class_id, &cls_factory2)))
        return H(0x80040154);
    if (!same_id(iid, &iids[FACTORY]) && !same_id(iid, &iid_unknown))
        return H(0x80004002);
    return make_object(FACTORY, NULL, out);
}
