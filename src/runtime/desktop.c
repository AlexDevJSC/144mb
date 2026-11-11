/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "windesktop.h"
#include "raw_input.h"
#include "winbase.h"
#include "desktop_resources.h"
#include "dialogs.h"
#include "menu_view.h"

#include <SDL.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

enum {
    MAX_CLASSES = 64, MAX_WINDOWS = 32, MAX_MESSAGES = 256, MAX_TITLE = 4096,
    MAX_SIZE = 4096, MAX_WINDOW_EXTRA = 65536, CLASS_ATOM = 0xc000,
    WS_CAPTION = 0x00c00000, WS_VISIBLE = 0x10000000, WS_DISABLED = 0x08000000,
    WS_THICKFRAME = 0x00040000, WS_EX_TOPMOST = 8, WS_EX_APPWINDOW = 0x00040000,
    CS_DBLCLKS = 8, SRCCOPY = 0x00cc0020, PATCOPY = 0x00f00021
};

_Static_assert(sizeof(struct nt_window_class) == 72, "WNDCLASSA layout");
_Static_assert(sizeof(struct nt_create_window) == 80, "CREATESTRUCTA layout");
_Static_assert(sizeof(struct nt_paint) == 72, "PAINTSTRUCT layout");

struct window_class {
    struct nt_window_class definition;
    char name[256];
    bool unicode;
};

struct window {
    uintptr_t handle, dc, instance, owner, input_context;
    uintptr_t menu;
    struct nt_menu_popup popups[NT_MENU_DEPTH];
    size_t popup_count;
    struct window_class *type;
    nt_window_proc proc;
    SDL_Window *native;
    char *title;
    unsigned char *extra;
    struct nt_rect dirty, normal;
    intptr_t userdata;
    uint16_t default_id;
    uint32_t style, exstyle;
    int width, height;
    unsigned int dc_users, input_refs;
    SDL_Rect input_rect;
    uint32_t mouse_track, hover_delay;
    uint64_t hover_due;
    int32_t hover_x, hover_y;
    bool visible, enabled, destroying, painting, erase, invalidated, dialog_window;
};

static pthread_mutex_t desktop_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool desktop_thread;
static bool desktop_started;
static struct window_class classes[MAX_CLASSES];
static struct window windows[MAX_WINDOWS];
static struct nt_message messages[MAX_MESSAGES];
static size_t class_count, message_count;
static bool overflow;
static uintptr_t next_handle = 0x25000005;
static unsigned char keyboard[256], async_keys[256];
static bool dpi_aware;
static uintptr_t current_cursor;
static SDL_AudioDeviceID beep_device;
static uintptr_t capture;

bool nt_runtime_open_desktop(void)
{
    pthread_mutex_lock(&desktop_lock);
    if (desktop_started || SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        pthread_mutex_unlock(&desktop_lock);
        return false;
    }
    desktop_started = desktop_thread = true;
    pthread_mutex_unlock(&desktop_lock);
    return true;
}

bool nt_runtime_desktop_active(void)
{
    return desktop_thread;
}

bool nt_runtime_show_assertion(const char *message)
{
    return desktop_thread &&
           SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Assertion failed", message, NULL) == 0;
}

int32_t NT_MS_ABI nt_MessageBeep(uint32_t type)
{
    enum { RATE = 22050, SAMPLES = 2205 };
    unsigned int frequency;
    switch (type) {
    case UINT32_MAX: case 0: frequency = 880; break;
    case 0x10: frequency = 440; break;
    case 0x20: frequency = 660; break;
    case 0x30: frequency = 550; break;
    case 0x40: frequency = 990; break;
    default:
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!desktop_thread) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    if (beep_device == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            nt_SetLastError(NT_ERROR_GEN_FAILURE);
            return 0;
        }
        const SDL_AudioSpec format = {.freq = RATE, .format = AUDIO_S16SYS, .channels = 1, .samples = 512};
        beep_device = SDL_OpenAudioDevice(NULL, 0, &format, NULL, 0);
        if (beep_device == 0) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            nt_SetLastError(NT_ERROR_GEN_FAILURE);
            return 0;
        }
        SDL_PauseAudioDevice(beep_device, 0);
    }
    if (SDL_GetQueuedAudioSize(beep_device) >= RATE * sizeof(int16_t)) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    int16_t samples[SAMPLES];
    for (unsigned int i = 0; i < SAMPLES; ++i) {
        const int amplitude = 1200 * (int)(SAMPLES - i) / SAMPLES;
        samples[i] = (int16_t)((i * frequency % RATE) < RATE / 2 ? amplitude : -amplitude);
    }
    if (SDL_QueueAudio(beep_device, samples, sizeof(samples)) != 0) {
        nt_SetLastError(NT_ERROR_GEN_FAILURE);
        return 0;
    }
    return 1;
}

static struct window *find_window(uintptr_t handle)
{
    if (desktop_thread && handle != 0) {
        for (size_t i = 0; i < MAX_WINDOWS; ++i) {
            if (windows[i].handle == handle)
                return &windows[i];
        }
    }
    return NULL;
}

bool nt_runtime_window_valid(uintptr_t handle)
{
    return find_window(handle) != NULL || nt_runtime_control_valid(handle);
}

bool nt_runtime_window_enabled(uintptr_t handle)
{
    const struct window *window = find_window(handle);
    return window != NULL && window->enabled && !window->destroying;
}

bool nt_runtime_dialog_window(uintptr_t handle)
{
    const struct window *window = find_window(handle);
    return window != NULL && window->dialog_window;
}

static struct window *require_window(uintptr_t handle)
{
    struct window *window = find_window(handle);
    if (window == NULL)
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
    return window;
}

static struct window *window_from_id(uint32_t id)
{
    for (size_t i = 0; i < MAX_WINDOWS; ++i) {
        if (windows[i].handle != 0 && SDL_GetWindowID(windows[i].native) == id)
            return &windows[i];
    }
    return NULL;
}

uint32_t nt_runtime_window_id(uintptr_t handle)
{
    const struct window *window = find_window(handle);
    return window != NULL ? SDL_GetWindowID(window->native) : 0;
}

static struct window_class *find_class(const char *name, uintptr_t instance)
{
    if (name == NULL)
        return NULL;
    const uintptr_t value = (uintptr_t)name;
    if (value <= UINT16_MAX) {
        if (value >= CLASS_ATOM && value - CLASS_ATOM < class_count &&
            classes[value - CLASS_ATOM].definition.proc != NULL &&
            classes[value - CLASS_ATOM].definition.instance == instance)
            return &classes[value - CLASS_ATOM];
        return NULL;
    }
    for (size_t i = 0; i < class_count; ++i) {
        if (classes[i].definition.proc != NULL && classes[i].definition.instance == instance &&
            nt_module_name_equal(classes[i].name, name))
            return &classes[i];
    }
    return NULL;
}

static uint16_t register_class(const struct nt_window_class *definition, bool unicode)
{
    if (!desktop_thread || definition == NULL || definition->proc == NULL ||
        definition->name == NULL || (uintptr_t)definition->name <= UINT16_MAX ||
        definition->name[0] == '\0') {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (definition->window_extra < 0 || definition->window_extra > MAX_WINDOW_EXTRA) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (definition->class_extra != 0 ||
        definition->menu != NULL ||
        (definition->cursor != 0 && nt_runtime_cursor(definition->cursor) == NULL) ||
        (definition->icon != 0 && nt_runtime_icon(definition->icon) == NULL)) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    if (find_class(definition->name, definition->instance) != NULL) {
        nt_SetLastError(1410); /* ERROR_CLASS_ALREADY_EXISTS */
        return 0;
    }
    const size_t length = strnlen(definition->name, sizeof(classes[0].name));
    size_t index = 0;
    while (index < class_count && classes[index].definition.proc != NULL)
        ++index;
    if (length == sizeof(classes[0].name) || index == MAX_CLASSES) {
        nt_SetLastError(length == sizeof(classes[0].name) ? NT_ERROR_INVALID_PARAMETER :
                       NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    struct window_class *type = &classes[index];
    type->definition = *definition;
    memcpy(type->name, definition->name, length + 1);
    type->definition.name = type->name;
    type->unicode = unicode;
    if (index == class_count)
        ++class_count;
    return (uint16_t)(CLASS_ATOM + index);
}

uint16_t NT_MS_ABI nt_RegisterClassA(const struct nt_window_class *definition)
{
    return register_class(definition, false);
}

static char *window_string(const uint16_t *value)
{
    const size_t units = nt_runtime_wide_length(value, MAX_TITLE);
    if (units == MAX_TITLE) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    char *result = nt_runtime_utf16_to_utf8(value, units);
    if (result == NULL)
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
    return result;
}

_Static_assert(sizeof(struct nt_window_class_ex) == 80, "WNDCLASSEXW layout");

uint16_t NT_MS_ABI nt_RegisterClassExW(const struct nt_window_class_ex *definition)
{
    if (definition == NULL || definition->size != sizeof(*definition) ||
        (uintptr_t)definition->name <= UINT16_MAX) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (definition->small_icon != 0 && nt_runtime_icon(definition->small_icon) == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    char *name = window_string(definition->name);
    if (name == NULL)
        return 0;
    const struct nt_window_class native = {definition->style, definition->proc,
        definition->class_extra, definition->window_extra, definition->instance,
        definition->icon != 0 ? definition->icon : definition->small_icon,
        definition->cursor, definition->background, (const char *)definition->menu, name};
    const uint16_t result = register_class(&native, true);
    free(name);
    return result;
}

int32_t NT_MS_ABI nt_UnregisterClassW(const uint16_t *name, uintptr_t instance)
{
    if (!desktop_thread || name == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    char *decoded = (uintptr_t)name > UINT16_MAX ? window_string(name) : NULL;
    if ((uintptr_t)name > UINT16_MAX && decoded == NULL)
        return 0;
    struct window_class *type = find_class(decoded != NULL ? decoded : (const char *)name, instance);
    free(decoded);
    if (type == NULL) {
        nt_SetLastError(1411); /* ERROR_CLASS_DOES_NOT_EXIST */
        return 0;
    }
    for (size_t i = 0; i < MAX_WINDOWS; ++i) {
        if (windows[i].handle != 0 && windows[i].type == type) {
            nt_SetLastError(1412); /* ERROR_CLASS_HAS_WINDOWS */
            return 0;
        }
    }
    *type = (struct window_class){0};
    return 1;
}

static bool valid_dimensions(int width, int height)
{
    return width > 0 && width <= MAX_SIZE && height > 0 && height <= MAX_SIZE;
}

static void invalidate(struct window *window, const struct nt_rect *rect, bool erase)
{
    const struct nt_rect client = {0, 0, window->width, window->height};
    struct nt_rect clipped = client;
    if (rect != NULL && !nt_IntersectRect(&clipped, &client, rect))
        return;
    if (window->invalidated)
        nt_UnionRect(&window->dirty, &window->dirty, &clipped);
    else
        window->dirty = clipped;
    window->invalidated = true;
    window->erase |= erase;
}

static void release_window(struct window *window)
{
    if (capture == window->handle) {
        capture = 0;
        SDL_CaptureMouse(SDL_FALSE);
    }
    nt_runtime_remove_controls(window->handle);
    nt_runtime_remove_window_timers(window->handle);
    nt_runtime_clipboard_window_closed(window->handle);
    nt_runtime_raw_window_closed(window->handle);
    nt_runtime_detach_menu(window->menu, window->handle, true);
    for (size_t i = 0; i < message_count;) {
        if (messages[i].window != window->handle) {
            ++i;
        } else {
            --message_count;
            memmove(messages + i, messages + i + 1, (message_count - i) * sizeof(messages[0]));
        }
    }
    SDL_DestroyWindow(window->native);
    nt_DeleteDC(window->dc);
    free(window->title);
    free(window->extra);
    *window = (struct window){0};
}

void nt_runtime_close_desktop(void)
{
    if (!desktop_thread)
        return;
    nt_runtime_close_clipboard();
    nt_runtime_close_raw_input();
    for (size_t i = 0; i < MAX_WINDOWS; ++i) {
        if (windows[i].handle != 0)
            release_window(&windows[i]);
    }
    class_count = message_count = 0;
    nt_runtime_close_dialogs();
    nt_runtime_close_controls();
    nt_runtime_close_desktop_resources();
    overflow = false;
    memset(keyboard, 0, sizeof(keyboard));
    memset(async_keys, 0, sizeof(async_keys));
    current_cursor = 0;
    dpi_aware = false;
    pthread_mutex_lock(&desktop_lock);
    if (beep_device != 0) {
        SDL_CloseAudioDevice(beep_device);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        beep_device = 0;
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    desktop_thread = desktop_started = false;
    pthread_mutex_unlock(&desktop_lock);
}

struct present_target {
    SDL_Surface *surface;
    int offset;
};

static int menu_height(const struct window *window)
{
    return window->menu != 0 ? NT_MENU_HEIGHT : 0;
}

static bool copy_to_native(const uint32_t *pixels, int32_t width, int32_t height, void *context)
{
    const struct present_target *target = context;
    SDL_Surface *surface = target->surface;
    if (width != surface->w || height != surface->h - target->offset)
        return false;
    unsigned char *first = (unsigned char *)surface->pixels + target->offset * surface->pitch;
    return SDL_ConvertPixels(width, height, SDL_PIXELFORMAT_XBGR8888, pixels,
                             width * (int)sizeof(*pixels), surface->format->format,
                             first, surface->pitch) == 0;
}

static bool resize_surface(struct window *window, int width, int height)
{
    if (window->width == width && window->height == height)
        return true;
    if (!valid_dimensions(width, height) || window->painting || window->dc_users != 0)
        return false;
    const uintptr_t resized = nt_runtime_create_surface(width, height);
    if (resized == 0)
        return false;
    const int w = width < window->width ? width : window->width;
    const int h = height < window->height ? height : window->height;
    if (!nt_BitBlt(resized, 0, 0, w, h, window->dc, 0, 0, SRCCOPY)) {
        nt_DeleteDC(resized);
        return false;
    }
    nt_DeleteDC(window->dc);
    window->dc = resized;
    window->width = width;
    window->height = height;
    for (size_t i = 0; i < window->popup_count; ++i)
        window->popups[i].first = 0;
    window->invalidated = false;
    invalidate(window, NULL, true);
    return true;
}

static bool present(struct window *window)
{
    if (!window->visible)
        return true;
    SDL_Surface *surface = SDL_GetWindowSurface(window->native);
    if (surface == NULL) {
        if (*SDL_GetError() == '\0')
            SDL_SetError("SDL window framebuffer unavailable");
        return false;
    }
    const int offset = menu_height(window);
    if (!resize_surface(window, surface->w, surface->h - offset)) {
        SDL_SetError("GDI surface resize to %dx%d failed", surface->w, surface->h);
        return false;
    }
    if (SDL_LockSurface(surface) != 0)
        return false;
    struct present_target target = {surface, offset};
    const bool copied = nt_runtime_read_surface(window->dc, copy_to_native, &target) &&
                        nt_runtime_draw_controls(surface, window->handle, offset) &&
                        (window->menu == 0 || nt_runtime_draw_menu(surface, window->menu,
                                                                  window->popups, window->popup_count));
    SDL_UnlockSurface(surface);
    return copied && SDL_UpdateWindowSurface(window->native) == 0;
}

bool nt_runtime_window_pixel(uintptr_t handle, int x, int y, uint32_t *color)
{
    struct window *window = find_window(handle);
    SDL_Surface *surface = window != NULL ? SDL_GetWindowSurface(window->native) : NULL;
    if (surface == NULL || x < 0 || y < 0 || x >= surface->w || y >= surface->h ||
        color == NULL || SDL_LockSurface(surface) != 0)
        return false;
    const unsigned char *pixel = (const unsigned char *)surface->pixels +
                                 y * surface->pitch + x * surface->format->BytesPerPixel;
    uint32_t converted;
    const int result = SDL_ConvertPixels(1, 1, surface->format->format, pixel, surface->pitch,
                                         SDL_PIXELFORMAT_XBGR8888, &converted, sizeof(converted));
    SDL_UnlockSurface(surface);
    if (result != 0)
        return false;
    *color = converted & UINT32_C(0xffffff);
    return true;
}

static uintptr_t create_window(uint32_t exstyle, const char *class_name, const char *title,
                                       uint32_t style, int32_t x, int32_t y, int32_t width,
                                       int32_t height, uintptr_t parent, uintptr_t menu,
                                       uintptr_t instance, void *parameter,
                                       bool unicode, const uint16_t *wide_name, const uint16_t *wide_title)
{
    if (!desktop_thread) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    const struct window *owner = find_window(parent);
    if (parent != 0 && (owner == NULL || owner->destroying)) {
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    if ((style & NT_WS_CHILD) != 0) {
        if (owner != NULL)
            return nt_runtime_create_control(class_name, title, style, exstyle, x, y, width, height,
                                             parent, menu);
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    if (menu != 0 ||
        (exstyle & ~(WS_EX_TOPMOST | WS_EX_APPWINDOW)) != 0) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    struct window_class *type = find_class(class_name, instance);
    if (type == NULL) {
        nt_SetLastError(1407); /* ERROR_CANNOT_FIND_WND_CLASS */
        return 0;
    }
    if (type->unicode != unicode) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    struct window *window = NULL;
    for (size_t i = 0; i < MAX_WINDOWS; ++i) {
        if (windows[i].handle == 0) {
            window = &windows[i];
            break;
        }
    }
    if (window == NULL || next_handle > UINTPTR_MAX - 16) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    if (width == INT32_MIN)
        width = 640;
    if (height == INT32_MIN)
        height = 480;
    if (title == NULL)
        title = "";
    const size_t title_length = strnlen(title, MAX_TITLE);
    if (!valid_dimensions(width, height) || title_length == MAX_TITLE) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    char *utf8 = unicode ? strdup(title) : nt_runtime_ansi_to_utf8(title, title_length);
    const size_t extra_size = (size_t)type->definition.window_extra;
    unsigned char *extra = extra_size != 0 ? calloc(1, extra_size) : NULL;
    if (utf8 == NULL || (extra_size != 0 && extra == NULL)) {
        free(utf8);
        free(extra);
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    uint32_t flags = SDL_WINDOW_HIDDEN | (dpi_aware ? SDL_WINDOW_ALLOW_HIGHDPI : 0);
    if ((style & WS_CAPTION) != WS_CAPTION)
        flags |= SDL_WINDOW_BORDERLESS;
    if ((style & WS_THICKFRAME) != 0)
        flags |= SDL_WINDOW_RESIZABLE;
    if ((exstyle & WS_EX_TOPMOST) != 0)
        flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    const int native_x = x == INT32_MIN ? (int)SDL_WINDOWPOS_UNDEFINED : x;
    const int native_y = y == INT32_MIN ? (int)SDL_WINDOWPOS_UNDEFINED : y;
    SDL_Window *native = SDL_CreateWindow(utf8, native_x, native_y, width, height, flags);
    const uintptr_t dc = native != NULL ? nt_runtime_create_surface(width, height) : 0;
    if (native == NULL || dc == 0) {
        free(utf8);
        if (native != NULL)
            SDL_DestroyWindow(native);
        free(extra);
        nt_SetLastError(native == NULL ? NT_ERROR_GEN_FAILURE : NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    SDL_SetWindowMaximumSize(native, MAX_SIZE, MAX_SIZE);
    SDL_Surface *icon = nt_runtime_icon(type->definition.icon);
    if (icon != NULL)
        SDL_SetWindowIcon(native, icon);
    const uintptr_t handle = next_handle;
    next_handle += 16;
    *window = (struct window){
        .handle = handle, .dc = dc, .instance = instance, .type = type, .owner = parent,
        .proc = type->definition.proc, .native = native, .title = utf8, .extra = extra,
        .style = style & ~WS_VISIBLE, .exstyle = exstyle, .width = width, .height = height,
        .enabled = (style & WS_DISABLED) == 0
    };
    nt_GetWindowRect(handle, &window->normal);
    struct nt_create_window create = {parameter, instance, menu, parent, height, width, y, x,
                                       (int32_t)style,
                                       unicode ? (const char *)wide_title : title,
                                       unicode ? (const char *)wide_name : type->name, exstyle};
    const intptr_t accepted = window->proc(handle, NT_WM_NCCREATE, 0, (intptr_t)&create);
    if (accepted == 0 || window->handle != handle) {
        if (window->handle == handle) {
            window->destroying = true;
            window->proc(handle, NT_WM_NCDESTROY, 0, 0);
            release_window(window);
        }
        return 0;
    }
    const intptr_t created = window->proc(handle, NT_WM_CREATE, 0, (intptr_t)&create);
    if (created == -1 || window->handle != handle) {
        if (window->handle == handle)
            nt_DestroyWindow(handle);
        return 0;
    }
    if ((style & WS_VISIBLE) != 0)
        nt_ShowWindow(handle, 1);
    return window->handle == handle ? handle : 0;
}

uintptr_t NT_MS_ABI nt_CreateWindowExA(uint32_t exstyle, const char *class_name, const char *title,
                                       uint32_t style, int32_t x, int32_t y, int32_t width, int32_t height,
                                       uintptr_t parent, uintptr_t menu, uintptr_t instance, void *parameter)
{
    return create_window(exstyle, class_name, title, style, x, y, width, height,
                         parent, menu, instance, parameter, false, NULL, NULL);
}

uintptr_t NT_MS_ABI nt_CreateWindowExW(uint32_t exstyle, const uint16_t *class_name, const uint16_t *title,
                                       uint32_t style, int32_t x, int32_t y, int32_t width, int32_t height,
                                       uintptr_t parent, uintptr_t menu, uintptr_t instance, void *parameter)
{
    const bool named = (uintptr_t)class_name > UINT16_MAX;
    char *class_utf8 = named ? window_string(class_name) : NULL;
    char *title_utf8 = title != NULL ? window_string(title) : NULL;
    uintptr_t result = 0;
    if ((!named || class_utf8 != NULL) && (title == NULL || title_utf8 != NULL))
        result = create_window(exstyle, named ? class_utf8 : (const char *)class_name, title_utf8,
                               style, x, y, width, height, parent, menu, instance, parameter, true,
                               class_name, title);
    free(class_utf8);
    free(title_utf8);
    return result;
}

int32_t NT_MS_ABI nt_DestroyWindow(uintptr_t handle)
{
    if (nt_runtime_destroy_control(handle))
        return 1;
    struct window *window = require_window(handle);
    if (window == NULL || window->destroying)
        return 0;
    window->destroying = true;
    if (capture == handle)
        nt_ReleaseCapture();
    window->proc(handle, NT_WM_DESTROY, 0, 0);
    for (size_t i = 0; i < MAX_WINDOWS; ++i) {
        if (windows[i].owner == handle && windows[i].handle != 0)
            nt_DestroyWindow(windows[i].handle);
    }
    nt_runtime_remove_controls(handle);
    window->proc(handle, NT_WM_NCDESTROY, 0, 0);
    release_window(window);
    return 1;
}

intptr_t NT_MS_ABI nt_SendMessageA(uintptr_t handle, uint32_t message, uintptr_t wparam, intptr_t lparam)
{
    if (nt_runtime_control_valid(handle))
        return nt_runtime_control_message(handle, message, wparam, lparam);
    struct window *window = require_window(handle);
    return window != NULL ? window->proc(handle, message, wparam, lparam) : 0;
}

static bool replace_title(struct window *window, char *utf8)
{
    if (utf8 == NULL) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    SDL_SetWindowTitle(window->native, utf8);
    free(window->title);
    window->title = utf8;
    return true;
}

static bool change_title(struct window *window, const char *title)
{
    if (title == NULL)
        title = "";
    const size_t length = strnlen(title, MAX_TITLE);
    if (length == MAX_TITLE) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return false;
    }
    return replace_title(window, nt_runtime_ansi_to_utf8(title, length));
}

static intptr_t get_title(const struct window *window, uintptr_t capacity, intptr_t output, bool unicode)
{
    uint16_t *wide = nt_runtime_utf8_to_wide(window->title);
    if (wide == NULL)
        return 0;
    size_t length = nt_runtime_wide_length(wide, MAX_TITLE * 3);
    if (unicode) {
        if (output != 0 && capacity != 0) {
            if (length >= capacity)
                length = capacity - 1;
            memcpy((void *)output, wide, length * sizeof(*wide));
            ((uint16_t *)output)[length] = 0;
        }
    } else {
        char ansi[MAX_TITLE * 3 + 1];
        const int32_t count = nt_WideCharToMultiByte(1252, 0, wide, -1, ansi, sizeof(ansi), NULL, NULL);
        length = count > 0 ? (size_t)count - 1 : 0;
        if (output != 0 && capacity != 0) {
            if (length >= capacity)
                length = capacity - 1;
            memcpy((void *)output, ansi, length);
            ((char *)output)[length] = '\0';
        }
    }
    free(wide);
    return (intptr_t)length;
}

intptr_t NT_MS_ABI nt_DefWindowProcA(uintptr_t handle, uint32_t message,
                                    uintptr_t wparam, intptr_t lparam)
{
    struct window *window = require_window(handle);
    if (window == NULL)
        return 0;
    switch (message) {
    case NT_WM_NCCREATE:
        return 1;
    case NT_WM_CLOSE:
        nt_DestroyWindow(handle);
        return 0;
    case NT_WM_SETTEXT:
        return change_title(window, (const char *)lparam);
    case NT_WM_GETTEXTLENGTH:
        return get_title(window, 0, 0, false);
    case NT_WM_GETTEXT:
        return wparam != 0 && lparam != 0 ? get_title(window, wparam, lparam, false) : 0;
    case NT_WM_ERASEBKGND:
        if (window->type->definition.background != 0 && wparam == window->dc) {
            const uintptr_t background = window->type->definition.background;
            const bool system_color = background <= 32;
            const uintptr_t brush = system_color ? nt_CreateSolidBrush(nt_GetSysColor((int32_t)background - 1)) : background;
            const uintptr_t previous = nt_SelectObject(window->dc, brush);
            int32_t result = 0;
            if (previous != 0) {
                result = nt_BitBlt(window->dc, 0, 0, window->width, window->height, 0, 0, 0, PATCOPY);
                nt_SelectObject(window->dc, previous);
            }
            if (system_color && brush != 0)
                nt_DeleteObject(brush);
            return result;
        }
        return 0;
    case NT_WM_PAINT: {
        struct nt_paint paint;
        if (nt_BeginPaint(handle, &paint) != 0)
            nt_EndPaint(handle, &paint);
        return 0;
    }
    default:
        return 0;
    }
}

intptr_t NT_MS_ABI nt_DefWindowProcW(uintptr_t handle, uint32_t message,
                                    uintptr_t wparam, intptr_t lparam)
{
    if (message != NT_WM_SETTEXT && message != NT_WM_GETTEXT && message != NT_WM_GETTEXTLENGTH)
        return nt_DefWindowProcA(handle, message, wparam, lparam);
    struct window *window = require_window(handle);
    if (window == NULL)
        return 0;
    if (message == NT_WM_SETTEXT) {
        char *text = lparam != 0 ? window_string((const uint16_t *)lparam) : strdup("");
        return text != NULL && replace_title(window, text);
    }
    if (message == NT_WM_GETTEXTLENGTH)
        return get_title(window, 0, 0, true);
    return wparam != 0 && lparam != 0 ? get_title(window, wparam, lparam, true) : 0;
}

intptr_t NT_MS_ABI nt_DefDlgProcA(uintptr_t handle, uint32_t message, uintptr_t wparam, intptr_t lparam)
{
    struct window *window = require_window(handle);
    if (window == NULL)
        return 0;
    window->dialog_window = window->type->definition.window_extra >= NT_DLGWINDOWEXTRA;
    nt_window_proc dialog = NULL;
    if (nt_runtime_dialog_window(handle))
        dialog = (nt_window_proc)nt_GetWindowLongPtrA(handle, NT_DWLP_DLGPROC);
    if (dialog != NULL) {
        nt_SetWindowLongPtrA(handle, NT_DWLP_MSGRESULT, 0);
        const intptr_t handled = dialog(handle, message, wparam, lparam);
        if (message == NT_WM_INITDIALOG || message == 0x39 ||
            message == 0x2e || message == 0x2f || (message >= 0x132 && message <= 0x138))
            return handled;
        if (window->handle != handle)
            return 0;
        if (handled != 0)
            return nt_GetWindowLongPtrA(handle, NT_DWLP_MSGRESULT);
    }
    switch (message) {
    case NT_WM_CLOSE:
        if (dialog != NULL)
            nt_SendMessageA(handle, NT_WM_COMMAND, NT_IDCANCEL, 0);
        else
            nt_DestroyWindow(handle);
        return 0;
    case NT_WM_NEXTDLGCTL:
        nt_runtime_focus_control(handle, lparam != 0 ? wparam : 0, lparam == 0 && wparam != 0);
        return 0;
    case 0x400: { /* DM_GETDEFID */
        const uint16_t id = window->default_id != 0 ? window->default_id :
                            nt_runtime_dialog_default_id(handle);
        return id != 0 ? (intptr_t)(0x534b0000u | id) : 0;
    }
    case 0x401: /* DM_SETDEFID */
        window->default_id = (uint16_t)wparam;
        return 1;
    default:
        return nt_DefWindowProcA(handle, message, wparam, lparam);
    }
}

int32_t NT_MS_ABI nt_GetClientRect(uintptr_t handle, struct nt_rect *rect)
{
    if (nt_runtime_control_valid(handle))
        return nt_runtime_control_rect(handle, rect, false);
    const struct window *window = require_window(handle);
    if (window == NULL || rect == NULL)
        return 0;
    *rect = (struct nt_rect){0, 0, window->width, window->height};
    return 1;
}

int32_t NT_MS_ABI nt_GetWindowRect(uintptr_t handle, struct nt_rect *rect)
{
    if (nt_runtime_control_valid(handle))
        return nt_runtime_control_rect(handle, rect, true);
    const struct window *window = require_window(handle);
    if (window == NULL || rect == NULL)
        return 0;
    int x, y, top = 0, left = 0, bottom = 0, right = 0;
    SDL_GetWindowPosition(window->native, &x, &y);
    SDL_GetWindowBordersSize(window->native, &top, &left, &bottom, &right);
    const int64_t l = (int64_t)x - left, t = (int64_t)y - top;
    const int64_t r = (int64_t)x + window->width + right;
    const int64_t b = (int64_t)y + window->height + menu_height(window) + bottom;
    if (l < INT32_MIN || t < INT32_MIN || r > INT32_MAX || b > INT32_MAX) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    *rect = (struct nt_rect){(int32_t)l, (int32_t)t, (int32_t)r, (int32_t)b};
    return 1;
}

int32_t NT_MS_ABI nt_AdjustWindowRectEx(struct nt_rect *rect, uint32_t style, int32_t menu,
                                       uint32_t exstyle)
{
    if (rect == NULL || rect->left > rect->right || rect->top > rect->bottom ||
        (menu != 0 && rect->top < INT32_MIN + NT_MENU_HEIGHT)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if ((style & (WS_CAPTION | WS_THICKFRAME | UINT32_C(0x40000000))) != 0 ||
        (exstyle & ~(WS_EX_TOPMOST | WS_EX_APPWINDOW)) != 0) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    if (menu != 0)
        rect->top -= NT_MENU_HEIGHT;
    return 1;
}

int32_t NT_MS_ABI nt_SetMenu(uintptr_t handle, uintptr_t menu)
{
    struct window *window = require_window(handle);
    if (window == NULL)
        return 0;
    if (window->menu == menu)
        return 1;
    if (menu != 0 && !nt_runtime_attach_menu(menu, handle))
        return 0;
    const int height = window->height + menu_height(window) - (menu != 0 ? NT_MENU_HEIGHT : 0);
    if (!resize_surface(window, window->width, height)) {
        nt_runtime_detach_menu(menu, handle, false);
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    nt_runtime_detach_menu(window->menu, handle, false);
    window->menu = menu;
    window->popup_count = 0;
    invalidate(window, NULL, true);
    return 1;
}

uintptr_t NT_MS_ABI nt_GetMenu(uintptr_t handle)
{
    const struct window *window = require_window(handle);
    return window != NULL ? window->menu : 0;
}

int32_t NT_MS_ABI nt_DrawMenuBar(uintptr_t handle)
{
    struct window *window = require_window(handle);
    if (window == NULL)
        return 0;
    if (!present(window)) {
        nt_SetLastError(NT_ERROR_GEN_FAILURE);
        return 0;
    }
    return 1;
}

int32_t NT_MS_ABI nt_GetMenuItemRect(uintptr_t handle, uintptr_t menu, uint32_t item,
                                     struct nt_rect *rect)
{
    const struct window *window = require_window(handle);
    if (window == NULL)
        return 0;
    const struct nt_menu_popup *position = NULL;
    for (size_t i = 0; i < window->popup_count; ++i) {
        if (window->popups[i].menu == menu)
            position = &window->popups[i];
    }
    if (rect == NULL || menu == 0 || (menu != window->menu && position == NULL)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    struct nt_menu_view_item entry;
    struct nt_rect row;
    if (!nt_runtime_menu_bounds(menu, position,
                               window->width, window->height + menu_height(window), item,
                               &row, &entry)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    int x, y;
    SDL_GetWindowPosition(window->native, &x, &y);
    if ((int64_t)x + row.right > INT32_MAX || (int64_t)y + row.bottom > INT32_MAX) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    *rect = (struct nt_rect){row.left + x, row.top + y, row.right + x, row.bottom + y};
    return 1;
}

int32_t NT_MS_ABI nt_InvalidateRect(uintptr_t handle, const struct nt_rect *rect, int32_t erase)
{
    struct window *window = require_window(handle);
    if (window == NULL)
        return 0;
    invalidate(window, rect, erase != 0);
    return 1;
}

uintptr_t NT_MS_ABI nt_BeginPaint(uintptr_t handle, struct nt_paint *paint)
{
    struct window *window = require_window(handle);
    if (window == NULL || paint == NULL || window->painting || window->dc_users != 0) {
        if (window != NULL)
            nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    *paint = (struct nt_paint){.dc = window->dc, .rect = window->dirty};
    if (!window->invalidated)
        paint->rect = (struct nt_rect){0};
    const bool erase = window->erase;
    window->invalidated = window->erase = false;
    window->painting = true;
    nt_SelectClipRgn(window->dc, 0);
    nt_IntersectClipRect(window->dc, paint->rect.left, paint->rect.top,
                         paint->rect.right, paint->rect.bottom);
    if (erase)
        paint->erase = window->proc(handle, NT_WM_ERASEBKGND, window->dc, 0) == 0;
    return window->handle == handle ? paint->dc : 0;
}

int32_t NT_MS_ABI nt_EndPaint(uintptr_t handle, const struct nt_paint *paint)
{
    struct window *window = require_window(handle);
    if (window == NULL || paint == NULL || !window->painting || paint->dc != window->dc) {
        if (window != NULL)
            nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    window->painting = false;
    nt_SelectClipRgn(window->dc, 0);
    if (!present(window)) {
        invalidate(window, NULL, false);
        nt_SetLastError(NT_ERROR_GEN_FAILURE);
        return 0;
    }
    return 1;
}

uintptr_t NT_MS_ABI nt_GetDC(uintptr_t handle)
{
    struct window *window = require_window(handle);
    if (window == NULL || window->painting || window->dc_users == UINT_MAX)
        return 0;
    ++window->dc_users;
    return window->dc;
}

int32_t NT_MS_ABI nt_ReleaseDC(uintptr_t handle, uintptr_t dc)
{
    struct window *window = require_window(handle);
    if (window == NULL || dc != window->dc || window->dc_users == 0) {
        if (window != NULL)
            nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (--window->dc_users == 0 && !present(window)) {
        invalidate(window, NULL, false);
        nt_SetLastError(NT_ERROR_GEN_FAILURE);
        return 0;
    }
    return 1;
}

int32_t NT_MS_ABI nt_ShowWindow(uintptr_t handle, int32_t show)
{
    if (nt_runtime_control_valid(handle))
        return nt_runtime_control_show(handle, show != 0);
    struct window *window = require_window(handle);
    if (window == NULL)
        return 0;
    if (show < 0 || show > 10) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    const bool was_visible = window->visible;
    if ((SDL_GetWindowFlags(window->native) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED)) == 0)
        nt_GetWindowRect(handle, &window->normal);
    window->visible = show != 0;
    if (window->visible) {
        window->style |= WS_VISIBLE;
        SDL_ShowWindow(window->native);
        if (SDL_GetMouseFocus() == window->native) {
            if (window->type->definition.cursor != 0)
                nt_SetCursor(window->type->definition.cursor);
            else
                SDL_SetCursor(SDL_GetDefaultCursor());
        }
        if (show == 2 || show == 6 || show == 7)
            SDL_MinimizeWindow(window->native);
        else if (show == 3)
            SDL_MaximizeWindow(window->native);
        else if (show == 9)
            SDL_RestoreWindow(window->native);
        invalidate(window, NULL, true);
    } else {
        window->style &= ~WS_VISIBLE;
        SDL_HideWindow(window->native);
        if (capture == handle)
            nt_ReleaseCapture();
    }
    if (window->handle == handle && was_visible != window->visible)
        window->proc(handle, NT_WM_SHOWWINDOW, window->visible, 0);
    return was_visible;
}

int32_t NT_MS_ABI nt_EnableWindow(uintptr_t handle, int32_t enable)
{
    if (nt_runtime_control_valid(handle))
        return nt_runtime_control_enable(handle, enable != 0);
    struct window *window = require_window(handle);
    if (window == NULL)
        return 0;
    const bool was_disabled = !window->enabled;
    window->enabled = enable != 0;
    if (window->enabled)
        window->style &= ~WS_DISABLED;
    else
        window->style |= WS_DISABLED;
    if (was_disabled == window->enabled) {
        if (!window->enabled)
            window->proc(handle, NT_WM_CANCELMODE, 0, 0);
        if (window->handle == handle && !window->enabled && capture == handle)
            nt_ReleaseCapture();
        if (window->handle == handle)
            window->proc(handle, NT_WM_ENABLE, window->enabled, 0);
    }
    return was_disabled;
}

bool nt_runtime_set_modal_owner(uintptr_t handle, uintptr_t owner)
{
    const struct window *dialog = find_window(handle), *parent = find_window(owner);
    if (dialog == NULL || parent == NULL || dialog->owner != owner)
        return false;
    SDL_SetWindowModalFor(dialog->native, parent->native);
    return true;
}

uintptr_t NT_MS_ABI nt_SetCapture(uintptr_t handle)
{
    struct window *window = require_window(handle);
    if (window == NULL || window->destroying || !window->enabled || !window->visible) {
        if (window != NULL)
            nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    const uintptr_t previous = capture;
    if (previous != handle) {
        capture = handle;
        SDL_CaptureMouse(SDL_GetKeyboardFocus() == window->native ? SDL_TRUE : SDL_FALSE);
        if (previous != 0)
            nt_SendMessageA(previous, NT_WM_CAPTURECHANGED, 0, (intptr_t)handle);
    }
    return previous;
}

int32_t NT_MS_ABI nt_ReleaseCapture(void)
{
    if (!desktop_thread) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    if (capture != 0) {
        const uintptr_t previous = capture;
        capture = 0;
        SDL_CaptureMouse(SDL_FALSE);
        nt_SendMessageA(previous, NT_WM_CAPTURECHANGED, 0, 0);
    }
    return 1;
}

int32_t NT_MS_ABI nt_SetForegroundWindow(uintptr_t handle)
{
    const struct window *window = require_window(handle);
    if (window == NULL)
        return 0;
    if (!window->visible || !window->enabled || window->destroying) {
        nt_SetLastError(NT_ERROR_ACCESS_DENIED);
        return 0;
    }
    SDL_RaiseWindow(window->native);
    if (SDL_GetKeyboardFocus() == window->native || SDL_SetWindowInputFocus(window->native) == 0)
        return 1;
    nt_SetLastError(NT_ERROR_ACCESS_DENIED);
    return 0;
}

int32_t NT_MS_ABI nt_SetWindowTextA(uintptr_t handle, const char *title)
{
    return nt_SendMessageA(handle, NT_WM_SETTEXT, 0, (intptr_t)title) != 0;
}

int32_t NT_MS_ABI nt_SetWindowTextW(uintptr_t handle, const uint16_t *title)
{
    const struct window *window = find_window(handle);
    if (window != NULL && window->type->unicode)
        return window->proc(handle, NT_WM_SETTEXT, 0, (intptr_t)title) != 0;
    if (title == NULL)
        return nt_SetWindowTextA(handle, NULL);
    char narrow[MAX_TITLE];
    return nt_WideCharToMultiByte(1252, 0, title, -1, narrow, sizeof(narrow), NULL, NULL) != 0 &&
           nt_SetWindowTextA(handle, narrow);
}

intptr_t NT_MS_ABI nt_GetWindowLongPtrA(uintptr_t handle, int32_t index)
{
    const struct window *window = require_window(handle);
    if (window == NULL)
        return 0;
    switch (index) {
    case -4: return (intptr_t)window->proc;
    case -6: return (intptr_t)window->instance;
    case -8: return (intptr_t)window->owner;
    case -16: return (int32_t)window->style;
    case -20: return (int32_t)window->exstyle;
    case -21: return window->userdata;
    default:
        break;
    }
    if (index >= 0) {
        const size_t size = (size_t)window->type->definition.window_extra;
        if ((size_t)index <= size && sizeof(intptr_t) <= size - (size_t)index) {
            intptr_t value;
            memcpy(&value, window->extra + index, sizeof(value));
            return value;
        }
        nt_SetLastError(NT_ERROR_INVALID_INDEX);
        return 0;
    }
    nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
    return 0;
}

intptr_t NT_MS_ABI nt_SetWindowLongPtrA(uintptr_t handle, int32_t index, intptr_t value)
{
    struct window *window = require_window(handle);
    if (window == NULL)
        return 0;
    if (index == -21) {
        const intptr_t previous = window->userdata;
        window->userdata = value;
        return previous;
    }
    if (index == -4 && value != 0) {
        const intptr_t previous = (intptr_t)window->proc;
        window->proc = (nt_window_proc)value;
        return previous;
    }
    if (index >= 0) {
        const size_t size = (size_t)window->type->definition.window_extra;
        if ((size_t)index <= size && sizeof(intptr_t) <= size - (size_t)index) {
            intptr_t previous;
            memcpy(&previous, window->extra + index, sizeof(previous));
            memcpy(window->extra + index, &value, sizeof(value));
            return previous;
        }
        nt_SetLastError(NT_ERROR_INVALID_INDEX);
        return 0;
    }
    nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
    return 0;
}

int32_t NT_MS_ABI nt_SetWindowPos(uintptr_t handle, uintptr_t after, int32_t x, int32_t y,
                                 int32_t width, int32_t height, uint32_t flags)
{
    enum { NOSIZE = 1, NOMOVE = 2, NOZORDER = 4, NOACTIVATE = 0x10, SHOW = 0x40, HIDE = 0x80 };
    struct window *window = require_window(handle);
    if (window == NULL)
        return 0;
    if ((flags & ~(NOSIZE | NOMOVE | NOZORDER | NOACTIVATE | SHOW | HIDE)) != 0 ||
        ((flags & NOZORDER) == 0 && after != 0) || (flags & (SHOW | HIDE)) == (SHOW | HIDE) ||
        ((flags & NOMOVE) == 0 && strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0)) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    if ((flags & NOSIZE) == 0 && (!valid_dimensions(width, height) ||
                                 !resize_surface(window, width, height - menu_height(window)))) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if ((flags & NOMOVE) == 0)
        SDL_SetWindowPosition(window->native, x, y);
    if ((flags & NOSIZE) == 0)
        SDL_SetWindowSize(window->native, width, height);
    if ((flags & (NOZORDER | NOACTIVATE)) == 0)
        SDL_RaiseWindow(window->native);
    if ((flags & (SHOW | HIDE)) != 0)
        nt_ShowWindow(handle, (flags & SHOW) != 0 ? 5 : 0);
    return 1;
}

static intptr_t pair(int x, int y)
{
    return (intptr_t)((uint32_t)(uint16_t)y << 16 | (uint16_t)x);
}

static void enqueue(struct window *window, uint32_t type, uintptr_t wparam, intptr_t lparam,
                    int x, int y, uint32_t private_data)
{
    struct nt_message value = {window->handle, type, wparam, lparam, nt_GetTickCount(),
                                x, y, private_data};
    if (type == NT_WM_MOUSEMOVE && message_count != 0 &&
        messages[message_count - 1].window == window->handle &&
        messages[message_count - 1].message == type) {
        messages[message_count - 1] = value;
    } else if (message_count == MAX_MESSAGES) {
        overflow = true;
    } else {
        messages[message_count++] = value;
    }
}

static unsigned int virtual_key(SDL_Keycode key)
{
    if (key >= SDLK_a && key <= SDLK_z)
        return (unsigned int)(key - SDLK_a + 'A');
    if (key >= SDLK_0 && key <= SDLK_9)
        return (unsigned int)key;
    if (key >= SDLK_F1 && key <= SDLK_F12)
        return (unsigned int)(key - SDLK_F1 + 0x70);
    switch (key) {
    case SDLK_BACKSPACE: case SDLK_TAB: case SDLK_RETURN: case SDLK_ESCAPE: case SDLK_SPACE:
        return (unsigned int)key;
    case SDLK_LEFT: return 0x25;
    case SDLK_UP: return 0x26;
    case SDLK_RIGHT: return 0x27;
    case SDLK_DOWN: return 0x28;
    case SDLK_INSERT: return 0x2d;
    case SDLK_DELETE: return 0x2e;
    case SDLK_HOME: return 0x24;
    case SDLK_END: return 0x23;
    case SDLK_PAGEUP: return 0x21;
    case SDLK_PAGEDOWN: return 0x22;
    case SDLK_LSHIFT: return 0xa0;
    case SDLK_RSHIFT: return 0xa1;
    case SDLK_LCTRL: return 0xa2;
    case SDLK_RCTRL: return 0xa3;
    case SDLK_LALT: return 0xa4;
    case SDLK_RALT: return 0xa5;
    case SDLK_CAPSLOCK: return 0x14;
    case SDLK_SEMICOLON: return 0xba;
    case SDLK_EQUALS: return 0xbb;
    case SDLK_COMMA: return 0xbc;
    case SDLK_MINUS: return 0xbd;
    case SDLK_PERIOD: return 0xbe;
    case SDLK_SLASH: return 0xbf;
    case SDLK_BACKQUOTE: return 0xc0;
    case SDLK_LEFTBRACKET: return 0xdb;
    case SDLK_BACKSLASH: return 0xdc;
    case SDLK_RIGHTBRACKET: return 0xdd;
    case SDLK_QUOTE: return 0xde;
    default: return 0;
    }
}

static uint32_t key_character(SDL_Keycode key, uint16_t mods)
{
    if ((mods & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) != 0)
        return 0;
    if (key >= SDLK_a && key <= SDLK_z) {
        const bool upper = ((mods & KMOD_SHIFT) != 0) != ((mods & KMOD_CAPS) != 0);
        return (uint32_t)(upper ? key - ('a' - 'A') : key);
    }
    if (key >= SDLK_0 && key <= SDLK_9 && (mods & KMOD_SHIFT) != 0)
        return (unsigned char)")!@#$%^&*("[key - SDLK_0];
    if (key >= 33 && key <= 126 && (mods & KMOD_SHIFT) != 0) {
        static const char normal[] = ";=,-./`[\\]'";
        static const char shifted[] = ":+<_>?~{|}\"";
        const char *symbol = strchr(normal, (int)key);
        if (symbol != NULL)
            return (unsigned char)shifted[symbol - normal];
    }
    if ((key >= SDLK_SPACE && key <= SDLK_z) || key == SDLK_RETURN ||
        key == SDLK_TAB || key == SDLK_BACKSPACE || key == SDLK_ESCAPE)
        return (uint32_t)key;
    return 0;
}

static uintptr_t mouse_keys(uint32_t state)
{
    const SDL_Keymod modifiers = SDL_GetModState();
    return ((state & SDL_BUTTON_LMASK) != 0 ? 1u : 0u) |
           ((state & SDL_BUTTON_RMASK) != 0 ? 2u : 0u) |
           ((state & SDL_BUTTON_MMASK) != 0 ? 16u : 0u) |
           ((modifiers & KMOD_SHIFT) != 0 ? 4u : 0u) |
           ((modifiers & KMOD_CTRL) != 0 ? 8u : 0u);
}

static void close_popups(struct window *window, size_t keep)
{
    const uintptr_t handle = window->handle;
    uintptr_t closed[NT_MENU_DEPTH];
    const size_t count = window->popup_count;
    for (size_t i = keep; i < count; ++i)
        closed[i] = window->popups[i].menu;
    window->popup_count = keep;
    for (size_t i = count; i > keep && window->handle == handle; --i)
        nt_SendMessageA(handle, NT_WM_UNINITMENUPOPUP, closed[i - 1], 0);
}

static bool open_popup(struct window *window, uintptr_t menu, uint32_t index,
                        int x, int y, size_t depth)
{
    if (depth >= NT_MENU_DEPTH) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return false;
    }
    const uintptr_t handle = window->handle, bar = window->menu;
    if (window->popup_count == 0)
        nt_SendMessageA(handle, NT_WM_INITMENU, bar, 0);
    if (window->handle != handle || window->menu != bar || depth > window->popup_count)
        return false;
    close_popups(window, depth);
    if (window->handle != handle || window->menu != bar || window->popup_count != depth)
        return false;
    nt_SendMessageA(handle, NT_WM_INITMENUPOPUP, menu, index);
    if (window->handle != handle || window->menu != bar || nt_GetMenuItemCount(menu) <= 0)
        return false;
    window->popups[depth] = (struct nt_menu_popup){.menu = menu, .x = x, .y = y};
    window->popup_count = depth + 1;
    return true;
}

static bool menu_pointer(struct window *window, int x, int y, bool released)
{
    if (window->menu == 0)
        return false;
    const uintptr_t handle = window->handle;
    if (y < NT_MENU_HEIGHT) {
        if (released) {
            bool opened = false;
            for (uint32_t i = 0; ; ++i) {
                struct nt_menu_view_item item;
                struct nt_rect row;
                if (!nt_runtime_menu_bounds(window->menu, NULL, window->width,
                                           window->height + NT_MENU_HEIGHT, i, &row, &item))
                    break;
                if (x < row.left || x >= row.right)
                    continue;
                if ((item.flags & (NT_MENU_DISABLED | NT_MENU_SEPARATOR)) == 0) {
                    if (item.submenu != 0) {
                        const bool same = window->popup_count != 0 &&
                                          window->popups[0].menu == item.submenu;
                        if (!same)
                            opened = open_popup(window, item.submenu, i, row.left, NT_MENU_HEIGHT, 0);
                    } else {
                        close_popups(window, 0);
                        if (window->handle == handle)
                            enqueue(window, NT_WM_COMMAND, item.id & UINT16_MAX, 0, 0, 0, 0);
                    }
                }
                break;
            }
            if (!opened && window->handle == handle)
                close_popups(window, 0);
            if (window->handle == handle)
                nt_DrawMenuBar(handle);
        }
        return true;
    }
    if (window->popup_count == 0)
        return false;
    if (!released)
        return true;
    for (size_t depth = window->popup_count; depth > 0; --depth) {
        const struct nt_menu_popup *popup = &window->popups[depth - 1];
        for (uint32_t i = popup->first; ; ++i) {
            struct nt_menu_view_item item;
            struct nt_rect row;
            if (!nt_runtime_menu_bounds(popup->menu, popup, window->width,
                                        window->height + NT_MENU_HEIGHT, i, &row, &item))
                break;
            if (x < row.left || x >= row.right || y < row.top || y >= row.bottom)
                continue;
            if ((item.flags & (NT_MENU_DISABLED | NT_MENU_SEPARATOR)) == 0) {
                if (item.submenu != 0) {
                    open_popup(window, item.submenu, i, row.right, row.top, depth);
                } else {
                    close_popups(window, 0);
                    if (window->handle == handle)
                        enqueue(window, NT_WM_COMMAND, item.id & UINT16_MAX, 0, 0, 0, 0);
                }
            }
            if (window->handle == handle)
                nt_DrawMenuBar(handle);
            return true;
        }
    }
    close_popups(window, 0);
    if (window->handle == handle)
        nt_DrawMenuBar(handle);
    return true;
}

static void scroll_popup(struct window *window, int delta)
{
    if (window->popup_count == 0)
        return;
    struct nt_menu_popup *popup = &window->popups[window->popup_count - 1];
    const int count = nt_GetMenuItemCount(popup->menu);
    const int capacity = window->height / NT_MENU_ROW;
    if (count <= 0 || capacity <= 0)
        return;
    const int64_t next = (int64_t)popup->first - delta;
    const int limit = count > capacity ? count - capacity : 0;
    popup->first = next < 0 ? 0 : next > limit ? (uint32_t)limit : (uint32_t)next;
    nt_DrawMenuBar(window->handle);
}

static struct window *pointer_window(uint32_t native_id, int *x, int *y)
{
    struct window *source = window_from_id(native_id);
    struct window *target = find_window(capture);
    int64_t px = *x, py = *y;
    if (target != NULL && source != NULL && source != target) {
        int source_x, source_y, target_x, target_y;
        SDL_GetWindowPosition(source->native, &source_x, &source_y);
        SDL_GetWindowPosition(target->native, &target_x, &target_y);
        px += (int64_t)source_x - target_x;
        py += (int64_t)source_y - target_y;
    }
    *x = px < INT16_MIN ? INT16_MIN : px > INT16_MAX ? INT16_MAX : (int)px;
    *y = py < INT16_MIN ? INT16_MIN : py > INT16_MAX ? INT16_MAX : (int)py;
    return target != NULL ? target : source;
}

static void translate_event(const SDL_Event *event)
{
    struct window *window;
    switch (event->type) {
    case SDL_QUIT:
        nt_PostQuitMessage(0);
        break;
    case SDL_WINDOWEVENT:
        window = window_from_id(event->window.windowID);
        if (window == NULL)
            break;
        if (event->window.event == SDL_WINDOWEVENT_CLOSE) {
            enqueue(window, NT_WM_CLOSE, 0, 0, 0, 0, 0);
        } else if (event->window.event == SDL_WINDOWEVENT_EXPOSED) {
            invalidate(window, NULL, false);
        } else if (event->window.event == SDL_WINDOWEVENT_MOVED) {
            enqueue(window, NT_WM_MOVE, 0, pair(event->window.data1, event->window.data2), 0, 0, 0);
        } else if (event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            const int width = event->window.data1, height = event->window.data2 - menu_height(window);
            if (resize_surface(window, width, height))
                enqueue(window, NT_WM_SIZE, 0, pair(width, height), 0, 0, 0);
        } else if (event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
            if (window->type->unicode) {
                SDL_SetTextInputRect(&window->input_rect);
                SDL_StartTextInput();
            }
        } else if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            if (window->type->unicode)
                SDL_StopTextInput();
            memset(keyboard, 0, sizeof(keyboard));
            memset(async_keys, 0, sizeof(async_keys));
        } else if (event->window.event == SDL_WINDOWEVENT_LEAVE) {
            if ((window->mouse_track & 2) != 0)
                enqueue(window, 0x2a3, 0, 0, 0, 0, 0); /* WM_MOUSELEAVE */
            window->mouse_track = 0;
        } else if (event->window.event == SDL_WINDOWEVENT_ENTER) {
            if (window->type->definition.cursor != 0)
                nt_SetCursor(window->type->definition.cursor);
            else
                SDL_SetCursor(SDL_GetDefaultCursor());
        }
        break;
    case SDL_KEYDOWN: case SDL_KEYUP:
        window = window_from_id(event->key.windowID);
        if (window != NULL && window->enabled) {
            const bool down = event->type == SDL_KEYDOWN;
            if (down && event->key.keysym.sym == SDLK_ESCAPE && window->popup_count != 0) {
                const uintptr_t handle = window->handle;
                close_popups(window, window->popup_count - 1);
                if (window->handle == handle)
                    nt_DrawMenuBar(handle);
                break;
            }
            const unsigned int key = virtual_key(event->key.keysym.sym);
            const uint32_t detail = UINT32_C(1) | (uint32_t)(event->key.keysym.scancode & 255) << 16 |
                                    (down ? (uint32_t)(event->key.repeat != 0) << 30 : UINT32_C(3) << 30);
            if (key != 0 && key < sizeof(async_keys)) {
                const bool suppress = nt_runtime_raw_key(window->handle, (uint16_t)event->key.keysym.scancode,
                                                         (uint16_t)key, down);
                const unsigned char old = async_keys[key];
                async_keys[key] = down ? (old & 0x80) == 0 ? 0x81 : old : old & 1;
                uint32_t character = down ? key_character(event->key.keysym.sym, event->key.keysym.mod) : 0;
                if (window->type->unicode && character >= 0x20)
                    character = 0; /* Composed printable text arrives through SDL_TEXTINPUT. */
                if (!suppress)
                    enqueue(window, down ? NT_WM_KEYDOWN : NT_WM_KEYUP, key, (intptr_t)(int32_t)detail,
                             0, 0, character);
            }
        }
        break;
    case SDL_TEXTINPUT:
        window = window_from_id(event->text.windowID);
        if (window != NULL && window->enabled && window->type->unicode) {
            uint16_t text[SDL_TEXTINPUTEVENT_TEXT_SIZE];
            const int count = nt_MultiByteToWideChar(65001, 8, event->text.text, -1, text,
                                                     sizeof(text) / sizeof(*text));
            for (int i = 0; i + 1 < count; ++i)
                enqueue(window, NT_WM_CHAR, text[i], 0, 0, 0, 0);
        }
        break;
    case SDL_MOUSEWHEEL:
        window = window_from_id(event->wheel.windowID);
        if (window != NULL && window->enabled) {
            int delta = event->wheel.y;
            if (event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
                delta = delta == INT_MIN ? INT_MAX : -delta;
            const int64_t step = (int64_t)delta * 120;
            const int16_t wheel = step < INT16_MIN ? INT16_MIN : step > INT16_MAX ? INT16_MAX : (int16_t)step;
            if (!nt_runtime_raw_mouse(window->handle, 0, 0, 0x400, (uint16_t)wheel))
                scroll_popup(window, delta);
        }
        break;
    case SDL_MOUSEMOTION: {
        int x = event->motion.x, y = event->motion.y;
        window = pointer_window(event->motion.windowID, &x, &y);
        if (window != NULL && window->enabled &&
            (capture != 0 || (y >= menu_height(window) && window->popup_count == 0))) {
            y -= menu_height(window);
            if ((window->mouse_track & 1) != 0 && (x != window->hover_x || y != window->hover_y)) {
                window->hover_x = x;
                window->hover_y = y;
                window->hover_due = nt_GetTickCount64() + window->hover_delay;
            }
            if (!nt_runtime_raw_mouse(window->handle, event->motion.xrel, event->motion.yrel, 0, 0))
                enqueue(window, NT_WM_MOUSEMOVE, mouse_keys(event->motion.state),
                         pair(x, y), x, y, 0);
        }
        break;
    }
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
        int x = event->button.x, y = event->button.y;
        window = pointer_window(event->button.windowID, &x, &y);
        if (window != NULL && window->enabled && event->button.button >= SDL_BUTTON_LEFT &&
            event->button.button <= SDL_BUTTON_RIGHT) {
            const uint32_t base = event->button.button == SDL_BUTTON_LEFT ? NT_WM_LBUTTONDOWN :
                                  (event->button.button == SDL_BUTTON_RIGHT ? NT_WM_RBUTTONDOWN : NT_WM_MBUTTONDOWN);
            const bool down = event->type == SDL_MOUSEBUTTONDOWN;
            const unsigned int shift = event->button.button == SDL_BUTTON_LEFT ? 0 :
                                       event->button.button == SDL_BUTTON_RIGHT ? 2 : 4;
            if (nt_runtime_raw_mouse(window->handle, 0, 0, (uint16_t)(1u << (shift + !down)), 0))
                break;
            if (capture == 0 && event->button.button == SDL_BUTTON_LEFT &&
                menu_pointer(window, x, y, !down))
                break;
            y -= menu_height(window);
            if (capture == 0 && (y < 0 || window->popup_count != 0))
                break;
            const uint32_t mask = SDL_BUTTON(event->button.button);
            const uint32_t current = SDL_GetMouseState(NULL, NULL);
            const bool twice = down && event->button.clicks == 2 &&
                               (window->type->definition.style & CS_DBLCLKS) != 0;
            enqueue(window, base + (down ? (twice ? 2u : 0u) : 1u),
                     mouse_keys(down ? current | mask : current & ~mask),
                     pair(x, y), x, y, 0);
        }
        break;
    }
    default:
        break;
    }
}

static bool matches(uintptr_t handle, uint32_t type, uintptr_t filter, uint32_t first, uint32_t last)
{
    return (filter == 0 || (filter == UINTPTR_MAX ? handle == 0 : filter == handle)) &&
           ((first == 0 && last == 0) || (type >= first && type <= last));
}

static void update_key(const struct nt_message *message)
{
    if (message->message != NT_WM_KEYDOWN && message->message != NT_WM_KEYUP)
        return;
    const unsigned int key = (unsigned int)message->wparam;
    if (key >= sizeof(keyboard))
        return;
    const bool down = message->message == NT_WM_KEYDOWN;
    const unsigned char before = keyboard[key];
    keyboard[key] = (before & 1) | (down ? 0x80 : 0);
    if (down && (before & 0x80) == 0 && key == 0x14)
        keyboard[key] ^= 1;
    if (key >= 0xa0 && key <= 0xa5) {
        const unsigned int pair = key & ~1u;
        keyboard[0x10 + (key - 0xa0) / 2] = (keyboard[pair] | keyboard[pair + 1]) & 0x80;
    }
}

int nt_runtime_peek_window_message(struct nt_message *message, uintptr_t filter,
                                   uint32_t first, uint32_t last, bool remove)
{
    if (!desktop_thread)
        return 0;
    SDL_Event event;
    for (size_t i = 0; i < MAX_MESSAGES && SDL_PollEvent(&event) != 0; ++i)
        translate_event(&event);
    const uint64_t now = nt_GetTickCount64();
    for (size_t i = 0; i < MAX_WINDOWS; ++i) {
        struct window *window = &windows[i];
        if ((window->mouse_track & 1) != 0 && now >= window->hover_due) {
            window->mouse_track &= ~1u;
            if (SDL_GetMouseFocus() == window->native)
                enqueue(window, 0x2a1, mouse_keys(SDL_GetMouseState(NULL, NULL)),
                        pair(window->hover_x, window->hover_y), window->hover_x, window->hover_y, 0);
        }
    }
    if (overflow) {
        overflow = false;
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return -1;
    }
    for (size_t i = 0; i < message_count; ++i) {
        if (matches(messages[i].window, messages[i].message, filter, first, last)) {
            *message = messages[i];
            if (remove) {
                --message_count;
                memmove(messages + i, messages + i + 1, (message_count - i) * sizeof(messages[0]));
                update_key(message);
            }
            return 1;
        }
    }
    for (size_t i = 0; i < MAX_WINDOWS; ++i) {
        const struct window *window = &windows[i];
        if (window->visible && window->invalidated && !window->painting &&
            matches(window->handle, NT_WM_PAINT, filter, first, last)) {
            *message = (struct nt_message){.window = window->handle, .message = NT_WM_PAINT,
                                           .time = nt_GetTickCount()};
            return 1;
        }
    }
    return 0;
}

int nt_runtime_take_window_message(struct nt_message *message, uintptr_t window,
                                   uint32_t first, uint32_t last)
{
    return nt_runtime_peek_window_message(message, window, first, last, true);
}

bool nt_runtime_post_window_message(uintptr_t handle, uint32_t message, uintptr_t wparam, intptr_t lparam)
{
    struct window *window = handle != 0 ? require_window(handle) : NULL;
    if (!desktop_thread || (handle != 0 && window == NULL))
        return false;
    if (message_count == MAX_MESSAGES) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    if (window != NULL)
        enqueue(window, message, wparam, lparam, 0, 0, 0);
    else
        messages[message_count++] = (struct nt_message){.message = message, .wparam = wparam,
                                                        .lparam = lparam, .time = nt_GetTickCount()};
    return true;
}

void nt_runtime_desktop_wait(int milliseconds)
{
    SDL_Event event;
    if (SDL_WaitEventTimeout(&event, milliseconds) != 0)
        translate_event(&event);
}

int32_t NT_MS_ABI nt_GetKeyboardState(unsigned char *keys)
{
    if (!desktop_thread || keys == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    memcpy(keys, keyboard, sizeof(keyboard));
    return 1;
}

int32_t NT_MS_ABI nt_TranslateMessage(const struct nt_message *message)
{
    if (message == NULL || message->message != NT_WM_KEYDOWN || message->private_data == 0 ||
        message->private_data > 0x7f)
        return 0;
    struct window *window = find_window(message->window);
    if (window == NULL)
        return 0;
    enqueue(window, NT_WM_CHAR, message->private_data, message->lparam, message->x, message->y, 0);
    return !overflow;
}

int32_t NT_MS_ABI nt_SystemParametersInfoA(uint32_t action, uint32_t parameter,
                                          void *result, uint32_t flags)
{
    if (!desktop_thread || action != 0x30 || parameter != 0 || flags != 0) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    SDL_Rect area;
    if (result == NULL || SDL_GetDisplayUsableBounds(0, &area) != 0) {
        nt_SetLastError(result == NULL ? NT_ERROR_INVALID_PARAMETER : NT_ERROR_GEN_FAILURE);
        return 0;
    }
    *(struct nt_rect *)result = (struct nt_rect){area.x, area.y, area.x + area.w, area.y + area.h};
    return 1;
}

_Static_assert(sizeof(struct nt_window_placement) == 44, "WINDOWPLACEMENT layout");
_Static_assert(sizeof(struct nt_monitor_info) == 40, "MONITORINFO layout");
_Static_assert(sizeof(struct nt_track_mouse) == 24, "TRACKMOUSEEVENT layout");

int32_t NT_MS_ABI nt_IsWindowUnicode(uintptr_t handle)
{
    const struct window *window = find_window(handle);
    return window != NULL && window->type->unicode;
}

int32_t NT_MS_ABI nt_IsWindowVisible(uintptr_t handle)
{
    const struct window *window = find_window(handle);
    return window != NULL && window->visible;
}

int32_t NT_MS_ABI nt_IsIconic(uintptr_t handle)
{
    const struct window *window = find_window(handle);
    return window != NULL && (SDL_GetWindowFlags(window->native) & SDL_WINDOW_MINIMIZED) != 0;
}

int32_t NT_MS_ABI nt_GetWindowPlacement(uintptr_t handle, struct nt_window_placement *placement)
{
    const struct window *window = require_window(handle);
    if (window == NULL || placement == NULL || placement->length != sizeof(*placement)) {
        if (window != NULL)
            nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const uint32_t flags = SDL_GetWindowFlags(window->native);
    struct nt_rect normal = window->normal;
    if ((flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED)) == 0)
        nt_GetWindowRect(handle, &normal);
    *placement = (struct nt_window_placement){.length = sizeof(*placement),
        .show = (flags & SDL_WINDOW_MINIMIZED) != 0 ? 2 : (flags & SDL_WINDOW_MAXIMIZED) != 0 ? 3 :
                window->visible ? 1 : 0,
        .minimum = {-1, -1}, .maximum = {-1, -1}, .normal = normal};
    return 1;
}

int32_t NT_MS_ABI nt_SetWindowPlacement(uintptr_t handle, const struct nt_window_placement *placement)
{
    struct window *window = require_window(handle);
    if (window == NULL)
        return 0;
    if (placement == NULL || placement->length != sizeof(*placement) ||
        placement->flags != 0 || placement->show > 10) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const struct nt_rect rect = placement->normal;
    int top = 0, left = 0, bottom = 0, right = 0;
    SDL_GetWindowBordersSize(window->native, &top, &left, &bottom, &right);
    const int64_t width = (int64_t)rect.right - rect.left - left - right;
    const int64_t height = (int64_t)rect.bottom - rect.top - top - bottom - menu_height(window);
    const int64_t x = (int64_t)rect.left + left, y = (int64_t)rect.top + top;
    if (width <= 0 || width > MAX_SIZE || height <= 0 || height > MAX_SIZE ||
        x > INT32_MAX || x < INT32_MIN || y > INT32_MAX || y < INT32_MIN) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    SDL_RestoreWindow(window->native);
    if (!nt_SetWindowPos(handle, 0, (int)x, (int)y, (int)width, (int)height, 0x14))
        return 0;
    window->normal = rect;
    nt_ShowWindow(handle, (int32_t)placement->show);
    return 1;
}

int32_t NT_MS_ABI nt_UpdateWindow(uintptr_t handle)
{
    struct window *window = require_window(handle);
    if (window == NULL)
        return 0;
    if (window->visible && window->invalidated && !window->painting)
        window->proc(handle, NT_WM_PAINT, 0, 0);
    return 1;
}

uintptr_t NT_MS_ABI nt_GetActiveWindow(void)
{
    SDL_Window *native = desktop_thread ? SDL_GetKeyboardFocus() : NULL;
    const struct window *window = native != NULL ? window_from_id(SDL_GetWindowID(native)) : NULL;
    return window != NULL ? window->handle : 0;
}

uintptr_t NT_MS_ABI nt_GetForegroundWindow(void) { return nt_GetActiveWindow(); }
uintptr_t NT_MS_ABI nt_GetCapture(void) { return desktop_thread ? capture : 0; }

uint32_t NT_MS_ABI nt_GetWindowThreadProcessId(uintptr_t handle, uint32_t *process)
{
    if (require_window(handle) == NULL)
        return 0;
    if (process != NULL)
        *process = (uint32_t)getpid();
    return (uint32_t)gettid();
}

static int32_t convert_point(uintptr_t handle, struct nt_point *point, int direction)
{
    if (point == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    int x, y;
    if (nt_runtime_control_valid(handle)) {
        struct nt_rect rect;
        if (!nt_runtime_control_rect(handle, &rect, true))
            return 0;
        x = rect.left;
        y = rect.top;
    } else {
        const struct window *window = require_window(handle);
        if (window == NULL)
            return 0;
        SDL_GetWindowPosition(window->native, &x, &y);
        y = (int32_t)((uint32_t)y + (uint32_t)menu_height(window));
    }
    const int64_t px = (int64_t)point->x + (int64_t)direction * x;
    const int64_t py = (int64_t)point->y + (int64_t)direction * y;
    if (px < INT32_MIN || px > INT32_MAX || py < INT32_MIN || py > INT32_MAX) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    *point = (struct nt_point){(int32_t)px, (int32_t)py};
    return 1;
}

int32_t NT_MS_ABI nt_ClientToScreen(uintptr_t window, struct nt_point *point) { return convert_point(window, point, 1); }
int32_t NT_MS_ABI nt_ScreenToClient(uintptr_t window, struct nt_point *point) { return convert_point(window, point, -1); }

int32_t NT_MS_ABI nt_GetCursorPos(struct nt_point *point)
{
    if (point == NULL || !desktop_thread) {
        nt_SetLastError(point == NULL ? NT_ERROR_INVALID_PARAMETER : NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    int x, y;
    SDL_GetGlobalMouseState(&x, &y);
    *point = (struct nt_point){x, y};
    return 1;
}

int32_t NT_MS_ABI nt_SetCursorPos(int32_t x, int32_t y)
{
    if (desktop_thread && SDL_WarpMouseGlobal(x, y) == 0)
        return 1;
    nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
    return 0;
}

uintptr_t NT_MS_ABI nt_SetCursor(uintptr_t handle)
{
    SDL_Cursor *native = nt_runtime_cursor(handle);
    if (!desktop_thread || (handle != 0 && native == NULL)) {
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    const uintptr_t previous = current_cursor;
    if (native != NULL)
        SDL_SetCursor(native);
    SDL_ShowCursor(native != NULL ? SDL_ENABLE : SDL_DISABLE);
    current_cursor = handle;
    return previous;
}

int32_t NT_MS_ABI nt_TrackMouseEvent(struct nt_track_mouse *track)
{
    const uint32_t query = 0x40000000u, cancel = 0x80000000u;
    if (track == NULL || track->size != sizeof(*track) || (track->flags & ~(query | cancel | 3)) != 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    struct window *window = require_window(track->window);
    if (window == NULL)
        return 0;
    if ((track->flags & query) != 0) {
        track->flags = window->mouse_track;
        track->milliseconds = window->hover_delay;
    } else if ((track->flags & cancel) != 0) {
        window->mouse_track &= ~(track->flags & 3);
    } else {
        window->mouse_track |= track->flags;
        if ((track->flags & 1) != 0) {
            window->hover_delay = track->milliseconds == UINT32_MAX ? 400 : track->milliseconds;
            window->hover_due = nt_GetTickCount64() + window->hover_delay;
            SDL_GetMouseState(&window->hover_x, &window->hover_y);
            window->hover_y -= menu_height(window);
        }
    }
    return 1;
}

int16_t NT_MS_ABI nt_GetKeyState(int32_t key)
{
    if (!desktop_thread || key < 0 || key >= (int32_t)sizeof(keyboard))
        return 0;
    return (int16_t)(((keyboard[key] & 0x80) != 0 ? 0x8000 : 0) | (keyboard[key] & 1));
}

int16_t NT_MS_ABI nt_GetAsyncKeyState(int32_t key)
{
    if (!desktop_thread || key < 0 || key >= (int32_t)sizeof(async_keys))
        return 0;
    SDL_Event event;
    for (size_t i = 0; i < MAX_MESSAGES && SDL_PollEvent(&event) != 0; ++i)
        translate_event(&event);
    const unsigned char value = async_keys[key];
    async_keys[key] &= 0x80;
    return (int16_t)(((value & 0x80) != 0 ? 0x8000 : 0) | (value & 1));
}

uintptr_t NT_MS_ABI nt_GetKeyboardLayout(uint32_t thread)
{
    if (thread != 0 && thread != (uint32_t)gettid()) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    /* The guest's virtual keyboard layout is English (US). */
    return 0x04090409;
}

enum { MONITOR_BASE = 0x79000001 };

uintptr_t NT_MS_ABI nt_MonitorFromPoint(struct nt_point point, uint32_t flags)
{
    if (!desktop_thread || flags > 2)
        return 0;
    const int count = SDL_GetNumVideoDisplays();
    int nearest = -1;
    double distance = 0;
    for (int i = 0; i < count; ++i) {
        SDL_Rect bounds;
        if (SDL_GetDisplayBounds(i, &bounds) != 0)
            continue;
        const int64_t right = (int64_t)bounds.x + bounds.w, bottom = (int64_t)bounds.y + bounds.h;
        const double dx = point.x < bounds.x ? (double)bounds.x - point.x :
                          point.x >= right ? (double)point.x - right + 1 : 0;
        const double dy = point.y < bounds.y ? (double)bounds.y - point.y :
                          point.y >= bottom ? (double)point.y - bottom + 1 : 0;
        const double squared = dx * dx + dy * dy;
        if (squared == 0)
            return (uintptr_t)MONITOR_BASE + (unsigned int)i;
        if (nearest < 0 || squared < distance) {
            nearest = i;
            distance = squared;
        }
    }
    if (nearest < 0 || flags == 0)
        return 0;
    return (uintptr_t)MONITOR_BASE + (flags == 1 ? 0u : (unsigned int)nearest);
}

uintptr_t NT_MS_ABI nt_MonitorFromWindow(uintptr_t handle, uint32_t flags)
{
    if (!desktop_thread || flags > 2)
        return 0;
    const struct window *window = find_window(handle);
    if (window == NULL)
        return flags != 0 && SDL_GetNumVideoDisplays() > 0 ? MONITOR_BASE : 0;
    const int index = SDL_GetWindowDisplayIndex(window->native);
    if (index >= 0)
        return (uintptr_t)MONITOR_BASE + (unsigned int)index;
    return flags != 0 && SDL_GetNumVideoDisplays() > 0 ? MONITOR_BASE : 0;
}

static int32_t offset_display(int32_t coordinate, int32_t extent)
{
    const int64_t result = (int64_t)coordinate + extent;
    return result > INT32_MAX ? INT32_MAX : result < INT32_MIN ? INT32_MIN : (int32_t)result;
}

static struct nt_rect display_rect(SDL_Rect rect)
{
    return (struct nt_rect){rect.x, rect.y, offset_display(rect.x, rect.w), offset_display(rect.y, rect.h)};
}

int32_t NT_MS_ABI nt_GetMonitorInfoW(uintptr_t monitor, struct nt_monitor_info *info)
{
    if (!desktop_thread || monitor < MONITOR_BASE || monitor - MONITOR_BASE > INT_MAX ||
        info == NULL || (info->size != sizeof(*info) && info->size != sizeof(*info) + 64)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const int index = (int)(monitor - MONITOR_BASE);
    SDL_Rect bounds, work;
    if (SDL_GetDisplayBounds(index, &bounds) != 0) {
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    if (SDL_GetDisplayUsableBounds(index, &work) != 0)
        work = bounds;
    info->bounds = display_rect(bounds);
    info->work = display_rect(work);
    info->flags = index == 0;
    if (info->size > sizeof(*info)) {
        char name[32];
        snprintf(name, sizeof(name), "\\\\.\\DISPLAY%d", index + 1);
        nt_MultiByteToWideChar(1252, 0, name, -1, (uint16_t *)(info + 1), 32);
    }
    return 1;
}

int32_t NT_MS_ABI nt_GetSystemMetrics(int32_t index)
{
    if (!desktop_thread)
        return 0;
    const int count = SDL_GetNumVideoDisplays();
    SDL_Rect bounds;
    if (index == 80)
        return count > 0 ? count : 0;
    if (index == 19)
        return 1; /* SDL supplies a mouse pointer. */
    if (index == 43)
        return 3;
    if (SDL_GetDisplayBounds(0, &bounds) != 0)
        return 0;
    if (index == 0 || index == 1)
        return index == 0 ? bounds.w : bounds.h;
    if (index >= 76 && index <= 79) {
        struct nt_rect total = display_rect(bounds);
        for (int i = 1; i < count; ++i) {
            if (SDL_GetDisplayBounds(i, &bounds) == 0) {
                const struct nt_rect other = display_rect(bounds);
                nt_UnionRect(&total, &total, &other);
            }
        }
        const int64_t width = (int64_t)total.right - total.left, height = (int64_t)total.bottom - total.top;
        if (index == 76 || index == 77)
            return index == 76 ? total.left : total.top;
        const int64_t result = index == 78 ? width : height;
        return result <= INT32_MAX ? (int32_t)result : INT32_MAX;
    }
    return 0;
}

int32_t NT_MS_ABI nt_SetProcessDPIAware(void)
{
    if (!desktop_thread) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    dpi_aware = true;
    return 1;
}

int32_t NT_MS_ABI nt_AdjustWindowRect(struct nt_rect *rect, uint32_t style, int32_t menu)
{
    return nt_AdjustWindowRectEx(rect, style, menu, 0);
}

uintptr_t NT_MS_ABI nt_ImmGetContext(uintptr_t handle)
{
    struct window *window = require_window(handle);
    if (window == NULL || window->input_refs == UINT_MAX)
        return 0;
    if (window->input_context == 0)
        window->input_context = handle ^ (uintptr_t)UINT32_C(0x5c000000);
    ++window->input_refs;
    return window->input_context;
}

int32_t NT_MS_ABI nt_ImmReleaseContext(uintptr_t handle, uintptr_t context)
{
    struct window *window = require_window(handle);
    if (window == NULL || window->input_context != context || window->input_refs == 0)
        return 0;
    --window->input_refs;
    return 1;
}

static int32_t set_ime_rectangle(uintptr_t context, uint32_t style, struct nt_point point,
                                 struct nt_rect area, bool candidate)
{
    struct window *window = NULL;
    if (desktop_thread && context != 0)
        window = find_window(context ^ (uintptr_t)UINT32_C(0x5c000000));
    if (window == NULL || window->input_context != context || window->input_refs == 0) {
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    const bool point_only = candidate ? style == 0x40 : style == 2 || style == 0x20;
    const bool rectangle = candidate ? style == 0x80 : style == 1;
    if (style != 0 && !point_only && !rectangle) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    SDL_Rect rect = {0, 0, 1, 1};
    if (point_only) {
        rect.x = point.x;
        rect.y = point.y;
    } else if (rectangle) {
        const int64_t width = (int64_t)area.right - area.left, height = (int64_t)area.bottom - area.top;
        if (width <= 0 || width > INT_MAX || height <= 0 || height > INT_MAX) {
            nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
            return 0;
        }
        rect = (SDL_Rect){area.left, area.top, (int)width, (int)height};
    }
    rect.y = offset_display(rect.y, menu_height(window));
    window->input_rect = rect;
    if (SDL_GetKeyboardFocus() == window->native)
        SDL_SetTextInputRect(&window->input_rect);
    return 1;
}

int32_t NT_MS_ABI nt_ImmSetCompositionWindow(uintptr_t context, const struct nt_composition_form *form)
{
    if (form == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return set_ime_rectangle(context, form->style, form->point, form->area, false);
}

int32_t NT_MS_ABI nt_ImmSetCandidateWindow(uintptr_t context, const struct nt_candidate_form *form)
{
    if (form == NULL || form->index != 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return set_ime_rectangle(context, form->style, form->point, form->area, true);
}
