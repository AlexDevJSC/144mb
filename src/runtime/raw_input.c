/* SPDX-License-Identifier: MIT */
#include "raw_input.h"

#include <string.h>

enum { MOUSE = 0, KEYBOARD = 1, WM_INPUT = 0xff, RAW_SLOTS = 512, REMOVE = 1, NO_LEGACY = 0x30,
       RID_INPUT = 0x10000003, RID_HEADER = 0x10000005 };
struct raw_header { uint32_t type, size; uintptr_t device, param; };
struct raw_mouse { uint16_t flags, padding, buttons, wheel; uint32_t raw; int32_t x, y; uint32_t extra; };
struct raw_keyboard { uint16_t scan, flags, reserved, key; uint32_t message, extra; };
struct raw_record {
    uintptr_t handle;
    struct raw_header header;
    union { struct raw_mouse mouse; struct raw_keyboard key; } data;
};
_Static_assert(sizeof(struct nt_raw_input_device) == 16, "RAWINPUTDEVICE layout");
_Static_assert(sizeof(struct raw_header) == 24, "RAWINPUTHEADER layout");
_Static_assert(sizeof(struct raw_mouse) == 24 && sizeof(struct raw_keyboard) == 16, "RAWINPUT payloads");

static struct raw_record packets[RAW_SLOTS];
static uintptr_t next_handle = 0x7b000001;
static struct { uintptr_t target; bool active, no_legacy; } registration[2];

int32_t NT_MS_ABI nt_RegisterRawInputDevices(const struct nt_raw_input_device *devices,
                                            uint32_t count, uint32_t size)
{
    if (!nt_runtime_desktop_active() || devices == NULL || count == 0 || count > 128 ||
        size != sizeof(*devices)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const struct nt_raw_input_device *device = &devices[i];
        if (device->page != 1 || (device->usage != 2 && device->usage != 6) ||
            (device->flags != 0 && device->flags != REMOVE && device->flags != NO_LEGACY) ||
            (device->flags == REMOVE && device->window != 0) ||
            (device->window != 0 && !nt_runtime_window_valid(device->window))) {
            nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
            return 0;
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        const struct nt_raw_input_device *device = &devices[i];
        const size_t index = device->usage == 2 ? MOUSE : KEYBOARD;
        registration[index].target = device->window;
        registration[index].active = device->flags != REMOVE;
        registration[index].no_legacy = device->flags == NO_LEGACY;
    }
    return 1;
}

void nt_runtime_close_raw_input(void)
{
    memset(registration, 0, sizeof(registration));
    memset(packets, 0, sizeof(packets));
}

void nt_runtime_raw_window_closed(uintptr_t window)
{
    for (size_t i = 0; i < sizeof(registration) / sizeof(registration[0]); ++i) {
        if (registration[i].target == window) {
            registration[i].target = 0;
            registration[i].active = false;
        }
    }
}

static struct raw_record *prepare_packet(uintptr_t window, uint32_t type)
{
    if (!registration[type].active || (registration[type].target != 0 && registration[type].target != window) ||
        next_handle == UINTPTR_MAX)
        return NULL;
    const uintptr_t handle = next_handle;
    struct raw_record *packet = &packets[handle % RAW_SLOTS];
    if (!nt_runtime_post_window_message(window, WM_INPUT, 0, (intptr_t)handle))
        return NULL;
    ++next_handle;
    *packet = (struct raw_record){.handle = handle, .header = {
        type, (uint32_t)(sizeof(struct raw_header) +
                         (type == MOUSE ? sizeof(struct raw_mouse) : sizeof(struct raw_keyboard))),
        UINT32_C(0x7b100000) + type, 0
    }};
    return packet;
}

bool nt_runtime_raw_mouse(uintptr_t window, int32_t dx, int32_t dy, uint16_t flags, uint16_t wheel)
{
    struct raw_record *packet = prepare_packet(window, MOUSE);
    if (packet == NULL)
        return false;
    packet->data.mouse = (struct raw_mouse){.buttons = flags, .wheel = wheel, .x = dx, .y = dy};
    return registration[MOUSE].no_legacy;
}

bool nt_runtime_raw_key(uintptr_t window, uint16_t scan, uint16_t key, bool pressed)
{
    struct raw_record *packet = prepare_packet(window, KEYBOARD);
    if (packet == NULL)
        return false;
    packet->data.key = (struct raw_keyboard){.scan = scan, .flags = !pressed, .key = key,
        .message = pressed ? NT_WM_KEYDOWN : NT_WM_KEYUP};
    return registration[KEYBOARD].no_legacy;
}

uint32_t NT_MS_ABI nt_GetRawInputData(uintptr_t handle, uint32_t command, void *data, uint32_t *size,
                                     uint32_t header_size)
{
    if (!nt_runtime_desktop_active() || size == NULL || header_size != sizeof(struct raw_header) ||
        (command != RID_INPUT && command != RID_HEADER)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return UINT32_MAX;
    }
    const struct raw_record *packet = &packets[handle % RAW_SLOTS];
    if (handle == 0 || packet->handle != handle) {
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        return UINT32_MAX;
    }
    const uint32_t needed = command == RID_HEADER ? sizeof(struct raw_header) : packet->header.size;
    if (data == NULL) {
        *size = needed;
        return 0;
    }
    if (*size < needed) {
        *size = needed;
        nt_SetLastError(NT_ERROR_INSUFFICIENT_BUFFER);
        return UINT32_MAX;
    }
    memcpy(data, &packet->header, sizeof(packet->header));
    if (command == RID_INPUT)
        memcpy((char *)data + sizeof(packet->header), &packet->data, needed - sizeof(packet->header));
    *size = needed;
    return needed;
}
