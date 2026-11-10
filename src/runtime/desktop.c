/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "desktop.h"
#include "desktop_resources.h"
#include "dialogs.h"
#include "menu_view.h"

#include <SDL.h>
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
};

struct window {
    uintptr_t handle, dc, instance, owner;
    uintptr_t menu;
    struct nt_menu_popup popups[NT_MENU_DEPTH];
    size_t popup_count;
    struct window_class *type;
    nt_window_proc proc;
    SDL_Window *native;
    char *title;
    unsigned char *extra;
    struct nt_rect dirty;
    intptr_t userdata;
    uint16_t default_id;
    uint32_t style, exstyle;
    int width, height;
    unsigned int dc_users;
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
static unsigned char keyboard[256];
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
            classes[value - CLASS_ATOM].definition.instance == instance)
            return &classes[value - CLASS_ATOM];
        return NULL;
    }
    for (size_t i = 0; i < class_count; ++i) {
        if (classes[i].definition.instance == instance &&
            nt_module_name_equal(classes[i].name, name))
            return &classes[i];
    }
    return NULL;
}

uint16_t NT_MS_ABI nt_RegisterClassA(const struct nt_window_class *definition)
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
    if (length == sizeof(classes[0].name) || class_count == MAX_CLASSES) {
        nt_SetLastError(length == sizeof(classes[0].name) ? NT_ERROR_INVALID_PARAMETER :
                       NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    struct window_class *type = &classes[class_count];
    type->definition = *definition;
    memcpy(type->name, definition->name, length + 1);
    type->definition.name = type->name;
    return (uint16_t)(CLASS_ATOM + class_count++);
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

uintptr_t NT_MS_ABI nt_CreateWindowExA(uint32_t exstyle, const char *class_name, const char *title,
                                       uint32_t style, int32_t x, int32_t y, int32_t width,
                                       int32_t height, uintptr_t parent, uintptr_t menu,
                                       uintptr_t instance, void *parameter)
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
    char *utf8 = nt_runtime_ansi_to_utf8(title, title_length);
    char *copy = strdup(title);
    const size_t extra_size = (size_t)type->definition.window_extra;
    unsigned char *extra = extra_size != 0 ? calloc(1, extra_size) : NULL;
    if (utf8 == NULL || copy == NULL || (extra_size != 0 && extra == NULL)) {
        free(utf8);
        free(copy);
        free(extra);
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    uint32_t flags = SDL_WINDOW_HIDDEN;
    if ((style & WS_CAPTION) != WS_CAPTION)
        flags |= SDL_WINDOW_BORDERLESS;
    if ((style & WS_THICKFRAME) != 0)
        flags |= SDL_WINDOW_RESIZABLE;
    if ((exstyle & WS_EX_TOPMOST) != 0)
        flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    const int native_x = x == INT32_MIN ? (int)SDL_WINDOWPOS_UNDEFINED : x;
    const int native_y = y == INT32_MIN ? (int)SDL_WINDOWPOS_UNDEFINED : y;
    SDL_Window *native = SDL_CreateWindow(utf8, native_x, native_y, width, height, flags);
    free(utf8);
    const uintptr_t dc = native != NULL ? nt_runtime_create_surface(width, height) : 0;
    if (native == NULL || dc == 0) {
        if (native != NULL)
            SDL_DestroyWindow(native);
        free(copy);
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
        .proc = type->definition.proc, .native = native, .title = copy, .extra = extra,
        .style = style & ~WS_VISIBLE, .exstyle = exstyle, .width = width, .height = height,
        .enabled = (style & WS_DISABLED) == 0
    };
    struct nt_create_window create = {parameter, instance, menu, parent, height, width, y, x,
                                       (int32_t)style, title, type->name, exstyle};
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

static bool change_title(struct window *window, const char *title)
{
    if (title == NULL)
        title = "";
    const size_t length = strnlen(title, MAX_TITLE);
    if (length == MAX_TITLE) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return false;
    }
    char *utf8 = nt_runtime_ansi_to_utf8(title, length);
    char *copy = strdup(title);
    if (utf8 == NULL || copy == NULL) {
        free(utf8);
        free(copy);
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    SDL_SetWindowTitle(window->native, utf8);
    free(utf8);
    free(window->title);
    window->title = copy;
    return true;
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
        return (intptr_t)strlen(window->title);
    case NT_WM_GETTEXT:
        if (wparam != 0 && lparam != 0) {
            size_t length = strlen(window->title);
            if (length >= wparam)
                length = wparam - 1;
            char *output = (char *)lparam;
            memcpy(output, window->title, length);
            output[length] = '\0';
            return (intptr_t)length;
        }
        return 0;
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
    window->visible = show != 0;
    if (window->visible) {
        window->style |= WS_VISIBLE;
        SDL_ShowWindow(window->native);
        if (SDL_GetMouseFocus() == window->native) {
            SDL_Cursor *cursor = nt_runtime_cursor(window->type->definition.cursor);
            SDL_SetCursor(cursor != NULL ? cursor : SDL_GetDefaultCursor());
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
        } else if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            memset(keyboard, 0, sizeof(keyboard));
        } else if (event->window.event == SDL_WINDOWEVENT_ENTER) {
            SDL_Cursor *cursor = nt_runtime_cursor(window->type->definition.cursor);
            SDL_SetCursor(cursor != NULL ? cursor : SDL_GetDefaultCursor());
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
            if (key != 0)
                enqueue(window, down ? NT_WM_KEYDOWN : NT_WM_KEYUP, key, (intptr_t)(int32_t)detail,
                         0, 0, down ? key_character(event->key.keysym.sym, event->key.keysym.mod) : 0);
        }
        break;
    case SDL_MOUSEWHEEL:
        window = window_from_id(event->wheel.windowID);
        if (window != NULL && window->enabled) {
            int delta = event->wheel.y;
            if (event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
                delta = delta == INT_MIN ? INT_MAX : -delta;
            scroll_popup(window, delta);
        }
        break;
    case SDL_MOUSEMOTION: {
        int x = event->motion.x, y = event->motion.y;
        window = pointer_window(event->motion.windowID, &x, &y);
        if (window != NULL && window->enabled &&
            (capture != 0 || (y >= menu_height(window) && window->popup_count == 0))) {
            y -= menu_height(window);
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
    return (filter == 0 || filter == handle) &&
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

int nt_runtime_take_window_message(struct nt_message *message, uintptr_t filter,
                                   uint32_t first, uint32_t last)
{
    if (!desktop_thread)
        return 0;
    SDL_Event event;
    for (size_t i = 0; i < MAX_MESSAGES && SDL_PollEvent(&event) != 0; ++i)
        translate_event(&event);
    if (overflow) {
        overflow = false;
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return -1;
    }
    for (size_t i = 0; i < message_count; ++i) {
        if (matches(messages[i].window, messages[i].message, filter, first, last)) {
            *message = messages[i];
            --message_count;
            memmove(messages + i, messages + i + 1, (message_count - i) * sizeof(messages[0]));
            update_key(message);
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
