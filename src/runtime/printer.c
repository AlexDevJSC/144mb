/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "compat.h"

#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { PD_RETURNDC = 0x100, PD_RETURNDEFAULT = 0x400, MAX_SIZE = 4096 };

struct device_mode {
    char name[32];
    uint16_t spec_version, driver_version, size, driver_extra;
    uint32_t fields;
    int16_t orientation, paper_size, length, width, scale, copies, source, quality;
    int16_t color, duplex, y_resolution, tt_option, collate;
    char form[32];
    uint16_t dpi;
    uint32_t display[13];
};

struct device_names {
    uint16_t driver, device, output, default_device;
    char text[64];
};

_Static_assert(sizeof(struct nt_print_dialog) == 120, "PRINTDLGA layout");
_Static_assert(sizeof(struct device_mode) == 156, "DEVMODEA layout");
_Static_assert(offsetof(struct device_mode, form) == 70, "DEVMODEA form offset");

static pthread_mutex_t printer_lock = PTHREAD_MUTEX_INITIALIZER;
static int default_output = -1;
static int32_t default_width, default_height;

bool nt_runtime_set_pdf_printer(int fd, int32_t width, int32_t height)
{
    int duplicate = -1;
    if (fd >= 0) {
        struct stat info;
        const int flags = fcntl(fd, F_GETFL);
        if (width < 1 || height < 1 || width > MAX_SIZE || height > MAX_SIZE ||
            flags < 0 || (flags & O_ACCMODE) == O_RDONLY || (flags & O_APPEND) != 0 ||
            fstat(fd, &info) != 0 || !S_ISREG(info.st_mode))
            return false;
        duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (duplicate < 0)
            return false;
    }
    pthread_mutex_lock(&printer_lock);
    if (default_output >= 0)
        close(default_output);
    default_output = duplicate;
    default_width = width;
    default_height = height;
    pthread_mutex_unlock(&printer_lock);
    return true;
}

static uintptr_t copy_global(const void *data, size_t size)
{
    const uintptr_t handle = nt_GlobalAlloc(2, size);
    if (handle == 0)
        return 0;
    void *target = nt_GlobalLock(handle);
    if (target == NULL) {
        nt_GlobalFree(handle);
        return 0;
    }
    memcpy(target, data, size);
    nt_GlobalUnlock(handle);
    return handle;
}

int32_t NT_MS_ABI nt_PrintDlgA(struct nt_print_dialog *dialog)
{
    if (dialog == NULL || dialog->size != sizeof(*dialog) ||
        dialog->device_mode != 0 || dialog->device_names != 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const uint32_t flags = dialog->flags;
    if ((flags & PD_RETURNDEFAULT) == 0 || (flags & ~(PD_RETURNDEFAULT | PD_RETURNDC)) != 0) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    pthread_mutex_lock(&printer_lock);
    if (default_output < 0) {
        pthread_mutex_unlock(&printer_lock);
        nt_SetLastError(NT_ERROR_FILE_NOT_FOUND);
        return 0;
    }
    const int32_t width = default_width, height = default_height;
    const uintptr_t dc = (flags & PD_RETURNDC) != 0 ?
                         nt_runtime_create_pdf_printer(default_output, width, height) : 0;
    pthread_mutex_unlock(&printer_lock);
    if ((flags & PD_RETURNDC) != 0 && dc == 0) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    const struct device_mode mode = {
        .name = "Linux PDF", .spec_version = 0x0401, .driver_version = 1, .size = sizeof(mode),
        .fields = 1 | 2 | 4 | 8 | 0x100, .orientation = 1, .paper_size = 256,
        .length = (int16_t)((height * 254 + 48) / 96), .width = (int16_t)((width * 254 + 48) / 96),
        .copies = 1, .dpi = 96, .form = "Custom"
    };
    const struct device_names names = {
        .driver = 8, .device = 8 + sizeof("Cairo"), .output = 8 + sizeof("Cairo") + sizeof("Linux PDF"),
        .default_device = 1, .text = "Cairo\0Linux PDF\0FILE:"
    };
    const uintptr_t settings = copy_global(&mode, sizeof(mode));
    const uintptr_t descriptor = settings != 0 ? copy_global(&names, sizeof(names)) : 0;
    if (settings == 0 || descriptor == 0) {
        nt_GlobalFree(settings);
        if (dc != 0)
            nt_DeleteDC(dc);
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    dialog->device_mode = settings;
    dialog->device_names = descriptor;
    dialog->dc = dc;
    dialog->copies = 1;
    return 1;
}
