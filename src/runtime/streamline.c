/* SPDX-License-Identifier: MIT */
#include "streamline.h"
#include "graphics_linux.h"

#include <pthread.h>

enum {
    SL_OK = 0, SL_NO_DEVICE = 5, SL_MISSING_RESOURCE_STATE = 18,
    SL_INIT_NOT_CALLED = 23, SL_INVALID_PARAMETER = 25, SL_DUPLICATED_CONSTANTS = 27,
    SL_INVALID_API = 28, SL_UNSUPPORTED_INTERFACE = 30, SL_FEATURE_MISSING = 31,
    SL_INVALID_STATE = 38,
    SL_D3D12 = 1, SL_FRAME_TAGGING = 1 << 7,
    SL_MAX_FRAMES = 16, SL_MAX_VIEWPORTS = 8, SL_MAX_TAGS = 32,
    SL_VALID_UNTIL_PRESENT = 1, SL_VALID_UNTIL_EVALUATE = 2,
    SL_CONSTANTS_V1_SIZE = 451, SL_CONSTANTS_V2_SIZE = 456,
};

static const GUID preferences_id = {0x1ca10965,0xbf8e,0x432b,{0x8d,0xa1,0x67,0x16,0xd8,0x79,0xfb,0x14}};
static const GUID frame_id = {0x830a0f35,0xdb84,0x4171,{0xa8,0x04,0x59,0xb2,0x06,0x49,0x9b,0x18}};
static const GUID viewport_id = {0x171b6435,0x9b3c,0x4fc8,{0x99,0x94,0xfb,0xe5,0x25,0x69,0xaa,0xa4}};
static const GUID constants_id = {0xdcd35ad7,0x4e4a,0x4bad,{0xa9,0x0c,0xe0,0xc4,0x9e,0xb2,0x3a,0xfe}};
static const GUID resource_id = {0x3a9d70cf,0x2418,0x4b72,{0x83,0x91,0x13,0xf8,0x72,0x1c,0x72,0x61}};
static const GUID tag_id = {0x4c6a5aad,0xb445,0x496c,{0x87,0xff,0x1a,0xf3,0x84,0x5b,0xe6,0x53}};
static const GUID requirements_id = {0x66714097,0xac6d,0x4bc6,{0x89,0x15,0x1e,0x0f,0x55,0xa6,0xb6,0x1f}};
static const GUID adapter_info_id = {0x0677315f,0xa746,0x4492,{0x9f,0x42,0xcb,0x61,0x42,0xc9,0xc3,0xd4}};
static const GUID device_id = {0x189819f1,0x1db6,0x4b57,{0xbe,0x54,0x18,0x21,0x33,0x9b,0x85,0xf7}};
static const GUID d3d_resource_id = {0x696442be,0xa72e,0x4059,{0xbc,0x79,0x5b,0x5c,0x98,0x04,0x0f,0xad}};

struct sl_header {
    const void *next;
    GUID type;
    size_t version;
};
struct sl_preferences {
    struct sl_header header;
    uint8_t show_console;
    uint32_t log_level;
    const uint16_t **plugin_paths;
    uint32_t plugin_path_count;
    const uint16_t *log_path;
    void *allocate, *release, *log;
    uint64_t flags;
    const uint32_t *features;
    uint32_t feature_count, application_id, engine;
    const char *engine_version, *project_id;
    uint32_t render_api;
};
struct sl_viewport { struct sl_header header; uint32_t index; };
struct sl_token {
    const struct sl_token_api *vtable;
    struct sl_header header;
    uint32_t index;
};
struct sl_token_api { uint32_t (NT_MS_ABI *frame_index)(const struct sl_token *); };
struct sl_resource {
    struct sl_header header;
    uint32_t type;
    IUnknown *native;
    void *memory, *view;
    uint32_t state;
};
struct sl_tag {
    struct sl_header header;
    const struct sl_resource *resource;
    uint32_t type, lifecycle;
    uint32_t extent[4];
};

_Static_assert(sizeof(struct sl_header) == 32, "Streamline BaseStructure ABI");
_Static_assert(offsetof(struct sl_preferences, render_api) == 136, "Streamline Preferences ABI");
_Static_assert(offsetof(struct sl_token, header) == 8, "Streamline FrameToken ABI");
_Static_assert(offsetof(struct sl_resource, state) == 64, "Streamline Resource ABI");
_Static_assert(sizeof(struct sl_tag) == 64, "Streamline ResourceTag ABI");

struct sl_stored_tag {
    IUnknown *resource;
    uint32_t type, state, lifecycle, extent[4];
};
struct sl_view {
    bool in_use, has_constants;
    uint32_t index;
    unsigned char constants[SL_CONSTANTS_V2_SIZE - sizeof(struct sl_header)];
    struct sl_stored_tag tags[SL_MAX_TAGS];
};
struct sl_frame {
    struct sl_token token;
    bool in_use;
    struct sl_view views[SL_MAX_VIEWPORTS];
};
static pthread_mutex_t sl_lock = PTHREAD_MUTEX_INITIALIZER;
static bool initialized;
static uint64_t preferences_flags;
static IUnknown *active_device;
static uint32_t frame_counter, next_slot;
static struct sl_frame frames[SL_MAX_FRAMES];

static bool valid_header(const struct sl_header *header, const GUID *type, size_t max_version)
{
    return header && IsEqualGUID(&header->type, type) &&
           header->version >= 1 && header->version <= max_version;
}

static size_t detach_frame(struct sl_frame *frame, IUnknown **resources)
{
    size_t count = 0;
    for (size_t i = 0; i < SL_MAX_VIEWPORTS; ++i)
        for (size_t j = 0; j < SL_MAX_TAGS; ++j)
            if (frame->views[i].tags[j].resource)
                resources[count++] = frame->views[i].tags[j].resource;
    memset(frame->views, 0, sizeof(frame->views));
    frame->in_use = false;
    return count;
}
static void release_resources(IUnknown **resources, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        IUnknown_Release(resources[i]);
}

static struct sl_frame *find_frame(const void *token)
{
    for (size_t i = 0; i < SL_MAX_FRAMES; ++i)
        if (frames[i].in_use && token == &frames[i].token)
            return &frames[i];
    return NULL;
}

static struct sl_view *find_view(struct sl_frame *frame, uint32_t index)
{
    struct sl_view *free_slot = NULL;
    for (size_t i = 0; i < SL_MAX_VIEWPORTS; ++i) {
        struct sl_view *view = &frame->views[i];
        if (view->in_use && view->index == index)
            return view;
        if (!view->in_use && !free_slot)
            free_slot = view;
    }
    return free_slot;
}

static int32_t feature_status(bool need_device)
{
    /* The native profile has no installed vendor features. */
    pthread_mutex_lock(&sl_lock);
    int32_t result = !initialized ? SL_INIT_NOT_CALLED :
        need_device && !active_device ? SL_NO_DEVICE : SL_FEATURE_MISSING;
    pthread_mutex_unlock(&sl_lock);
    return result;
}

int32_t NT_MS_ABI nt_slInit(const void *input, uint64_t sdk_version)
{
    const struct sl_preferences *preferences = input;
    if ((sdk_version & 0xffffu) != 0xfedcu || sdk_version >> 48 != 2 ||
        !valid_header(input, &preferences_id, 1) ||
        (preferences->feature_count && !preferences->features))
        return SL_INVALID_PARAMETER;
    if (preferences->render_api != SL_D3D12)
        return SL_INVALID_API;
    pthread_mutex_lock(&sl_lock);
    if (initialized) {
        pthread_mutex_unlock(&sl_lock);
        return SL_INVALID_STATE;
    }
    initialized = true;
    preferences_flags = preferences->flags;
    frame_counter = 0;
    next_slot = 0;
    pthread_mutex_unlock(&sl_lock);
    return SL_OK;
}

int32_t NT_MS_ABI nt_slShutdown(void)
{
    IUnknown *resources[SL_MAX_FRAMES * SL_MAX_VIEWPORTS * SL_MAX_TAGS];
    size_t count = 0;
    pthread_mutex_lock(&sl_lock);
    if (!initialized) {
        pthread_mutex_unlock(&sl_lock);
        return SL_INIT_NOT_CALLED;
    }
    for (size_t i = 0; i < SL_MAX_FRAMES; ++i)
        count += detach_frame(&frames[i], resources + count);
    IUnknown *device = active_device;
    active_device = NULL;
    initialized = false;
    pthread_mutex_unlock(&sl_lock);
    release_resources(resources, count);
    if (device)
        IUnknown_Release(device);
    return SL_OK;
}

int32_t NT_MS_ABI nt_slSetD3DDevice(void *input)
{
    IUnknown *device = input, *held = NULL;
    if (!device)
        return SL_INVALID_PARAMETER;
    if (FAILED(IUnknown_QueryInterface(device, &device_id, (void **)&held)) || !held)
        return SL_UNSUPPORTED_INTERFACE;
    pthread_mutex_lock(&sl_lock);
    int32_t result = !initialized ? SL_INIT_NOT_CALLED :
        active_device && active_device != held ? SL_INVALID_STATE : SL_OK;
    if (result == SL_OK && !active_device) {
        active_device = held;
        held = NULL;
    }
    pthread_mutex_unlock(&sl_lock);
    if (held)
        IUnknown_Release(held);
    return result;
}

int32_t NT_MS_ABI nt_slGetNativeInterface(void *input, void **native)
{
    if (!native)
        return SL_INVALID_PARAMETER;
    *native = NULL;
    if (!input)
        return SL_INVALID_PARAMETER;
    IUnknown_AddRef((IUnknown *)input);
    *native = input;
    return SL_OK;
}

static uint32_t NT_MS_ABI token_index(const struct sl_token *token)
{
    pthread_mutex_lock(&sl_lock);
    uint32_t index = token->index;
    pthread_mutex_unlock(&sl_lock);
    return index;
}

int32_t NT_MS_ABI nt_slGetNewFrameToken(void **token, const uint32_t *index)
{
    static const struct sl_token_api vtable = {token_index};
    if (!token)
        return SL_INVALID_PARAMETER;
    *token = NULL;
    pthread_mutex_lock(&sl_lock);
    if (!initialized) {
        pthread_mutex_unlock(&sl_lock);
        return SL_INIT_NOT_CALLED;
    }
    uint32_t number = index ? *index : frame_counter++;
    for (size_t i = 0; i < SL_MAX_FRAMES; ++i) {
        if (frames[i].in_use && frames[i].token.index == number) {
            *token = &frames[i].token;
            pthread_mutex_unlock(&sl_lock);
            return SL_OK;
        }
    }
    struct sl_frame *frame = &frames[next_slot++ % SL_MAX_FRAMES];
    IUnknown *resources[SL_MAX_VIEWPORTS * SL_MAX_TAGS];
    size_t count = detach_frame(frame, resources);
    frame->token = (struct sl_token){&vtable, {NULL, frame_id, 1}, number};
    frame->in_use = true;
    *token = &frame->token;
    pthread_mutex_unlock(&sl_lock);
    release_resources(resources, count);
    return SL_OK;
}

int32_t NT_MS_ABI nt_slSetConstants(const void *constants, const void *token, const void *viewport)
{
    const struct sl_header *header = constants;
    const struct sl_viewport *view_id = viewport;
    if (!valid_header(header, &constants_id, 2) || !valid_header(viewport, &viewport_id, 1))
        return SL_INVALID_PARAMETER;
    pthread_mutex_lock(&sl_lock);
    struct sl_frame *frame = find_frame(token);
    struct sl_view *view = frame ? find_view(frame, view_id->index) : NULL;
    int32_t result = !initialized ? SL_INIT_NOT_CALLED : !active_device ? SL_NO_DEVICE :
        !view ? SL_INVALID_PARAMETER : view->has_constants ? SL_DUPLICATED_CONSTANTS : SL_OK;
    if (result == SL_OK) {
        size_t size = header->version == 1 ? SL_CONSTANTS_V1_SIZE : SL_CONSTANTS_V2_SIZE;
        memcpy(view->constants, header + 1, size - sizeof(*header));
        view->index = view_id->index;
        view->in_use = view->has_constants = true;
    }
    pthread_mutex_unlock(&sl_lock);
    return result;
}

static int32_t store_tags(struct sl_view *view, const struct sl_stored_tag *tags,
                          uint32_t count, IUnknown **replaced)
{
    struct sl_stored_tag updated[SL_MAX_TAGS];
    memcpy(updated, view->tags, sizeof(updated));
    for (uint32_t i = 0; i < count; ++i) {
        size_t slot = SL_MAX_TAGS;
        for (size_t j = 0; j < SL_MAX_TAGS; ++j) {
            if (updated[j].resource && updated[j].type == tags[i].type) {
                slot = j;
                break;
            }
            if (!updated[j].resource && slot == SL_MAX_TAGS)
                slot = j;
        }
        if (slot == SL_MAX_TAGS) {
            if (!tags[i].resource)
                continue;
            return SL_INVALID_STATE;
        }
        replaced[i] = updated[slot].resource;
        updated[slot] = tags[i];
    }
    memcpy(view->tags, updated, sizeof(updated));
    return SL_OK;
}

int32_t NT_MS_ABI nt_slSetTagForFrame(const void *token, const void *viewport, const void *input,
                                    uint32_t count, void *command_list)
{
    const struct sl_viewport *view_id = viewport;
    const struct sl_tag *tags = input;
    if (!valid_header(viewport, &viewport_id, 1) || (count && !tags) || count > SL_MAX_TAGS)
        return SL_INVALID_PARAMETER;
    struct sl_stored_tag pending[SL_MAX_TAGS] = {0};
    IUnknown *replaced[SL_MAX_TAGS] = {0};
    int32_t result = SL_OK;
    for (uint32_t i = 0; i < count; ++i) {
        if (!valid_header(&tags[i].header, &tag_id, 1) || tags[i].header.next) {
            result = SL_INVALID_PARAMETER;
            break;
        }
        pending[i].type = tags[i].type;
        pending[i].lifecycle = tags[i].lifecycle;
        memcpy(pending[i].extent, tags[i].extent, sizeof(tags[i].extent));
        const struct sl_resource *resource = tags[i].resource;
        if (!resource)
            continue;
        if (!valid_header(&resource->header, &resource_id, 1) || !resource->native ||
            (tags[i].lifecycle != SL_VALID_UNTIL_PRESENT &&
             (tags[i].lifecycle != SL_VALID_UNTIL_EVALUATE || !command_list))) {
            result = SL_INVALID_PARAMETER;
            break;
        }
        if (resource->state == UINT32_MAX) {
            result = SL_MISSING_RESOURCE_STATE;
            break;
        }
        pending[i].state = resource->state;
        if (FAILED(IUnknown_QueryInterface(resource->native, &d3d_resource_id,
                                          (void **)&pending[i].resource)) || !pending[i].resource) {
            result = SL_UNSUPPORTED_INTERFACE;
            break;
        }
    }
    if (result == SL_OK) {
        pthread_mutex_lock(&sl_lock);
        struct sl_frame *frame = find_frame(token);
        struct sl_view *view = frame ? find_view(frame, view_id->index) : NULL;
        result = !initialized ? SL_INIT_NOT_CALLED : !active_device ? SL_NO_DEVICE :
            !view ? SL_INVALID_PARAMETER : !(preferences_flags & SL_FRAME_TAGGING) ?
            SL_INVALID_STATE : store_tags(view, pending, count, replaced);
        if (result == SL_OK) {
            view->index = view_id->index;
            view->in_use = true;
        }
        pthread_mutex_unlock(&sl_lock);
    }
    for (uint32_t i = 0; i < count; ++i) {
        IUnknown *resource = result == SL_OK ? replaced[i] : pending[i].resource;
        if (resource)
            IUnknown_Release(resource);
    }
    return result;
}

int32_t NT_MS_ABI nt_slGetFeatureRequirements(uint32_t feature, void *requirements)
{
    (void)feature;
    return valid_header(requirements, &requirements_id, 2) ? feature_status(false) : SL_INVALID_PARAMETER;
}
int32_t NT_MS_ABI nt_slIsFeatureSupported(uint32_t feature, const void *adapter)
{
    (void)feature;
    return !adapter || valid_header(adapter, &adapter_info_id, 1) ?
        feature_status(false) : SL_INVALID_PARAMETER;
}
int32_t NT_MS_ABI nt_slSetFeatureLoaded(uint32_t feature, uint8_t loaded)
{
    (void)feature; (void)loaded;
    return feature_status(true);
}
int32_t NT_MS_ABI nt_slGetFeatureFunction(uint32_t feature, const char *name, void **function)
{
    (void)feature;
    if (!function)
        return SL_INVALID_PARAMETER;
    *function = NULL;
    return name && *name ? feature_status(true) : SL_INVALID_PARAMETER;
}
int32_t NT_MS_ABI nt_slFreeResources(uint32_t feature, const void *viewport)
{
    (void)feature;
    return valid_header(viewport, &viewport_id, 1) ? feature_status(true) : SL_INVALID_PARAMETER;
}
int32_t NT_MS_ABI nt_slEvaluateFeature(uint32_t feature, const void *token, const void *inputs,
                                      uint32_t count, void *command_list)
{
    (void)feature;
    if ((count && !inputs) || !command_list)
        return SL_INVALID_PARAMETER;
    pthread_mutex_lock(&sl_lock);
    int32_t result = !initialized ? SL_INIT_NOT_CALLED : !active_device ? SL_NO_DEVICE :
        !find_frame(token) ? SL_INVALID_PARAMETER : SL_FEATURE_MISSING;
    pthread_mutex_unlock(&sl_lock);
    return result;
}
