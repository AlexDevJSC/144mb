/* SPDX-License-Identifier: MIT */
#include "desktop.h"

#include <SDL.h>
#include <stdlib.h>
#include <string.h>

enum { CF_TEXT = 1, CF_UNICODETEXT = 13, CLIPBOARD_NOT_OPEN = 1418 };

/* The desktop thread owns the selection and is the only guest clipboard caller. */
static bool opened, emptied;
static uintptr_t window_owner, selection;

int32_t NT_MS_ABI nt_OpenClipboard(uintptr_t window)
{
    if (!nt_runtime_desktop_active() || (window != 0 && !nt_runtime_window_valid(window))) {
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    if (opened) {
        nt_SetLastError(NT_ERROR_ACCESS_DENIED);
        return 0;
    }
    opened = true;
    emptied = false;
    window_owner = window;
    return 1;
}

int32_t NT_MS_ABI nt_CloseClipboard(void)
{
    if (!nt_runtime_desktop_active() || !opened) {
        nt_SetLastError(CLIPBOARD_NOT_OPEN);
        return 0;
    }
    opened = emptied = false;
    window_owner = 0;
    return 1;
}

int32_t NT_MS_ABI nt_EmptyClipboard(void)
{
    if (!nt_runtime_desktop_active() || !opened) {
        nt_SetLastError(CLIPBOARD_NOT_OPEN);
        return 0;
    }
    if (SDL_SetClipboardText("") != 0) {
        nt_SetLastError(NT_ERROR_GEN_FAILURE);
        return 0;
    }
    nt_runtime_release_clipboard_memory(selection, true);
    selection = 0;
    emptied = true;
    return 1;
}

static char *decode_text(uint32_t format, const unsigned char *bytes, size_t size)
{
    if (format == CF_TEXT) {
        const unsigned char *end = memchr(bytes, '\0', size);
        return end != NULL ? nt_runtime_ansi_to_utf8((const char *)bytes, (size_t)(end - bytes)) : NULL;
    }
    const uint16_t *wide = (const uint16_t *)bytes;
    for (size_t i = 0; i < size / sizeof(*wide); ++i) {
        if (wide[i] == 0)
            return nt_runtime_utf16_to_utf8(wide, i);
    }
    return NULL;
}

uintptr_t NT_MS_ABI nt_SetClipboardData(uint32_t format, uintptr_t memory)
{
    if (!nt_runtime_desktop_active() || !opened) {
        nt_SetLastError(CLIPBOARD_NOT_OPEN);
        return 0;
    }
    if (!emptied || window_owner == 0) {
        nt_SetLastError(NT_ERROR_ACCESS_DENIED);
        return 0;
    }
    if ((format != CF_TEXT && format != CF_UNICODETEXT) || memory == 0 || selection != 0) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    unsigned char *bytes;
    size_t size;
    if (!nt_runtime_claim_clipboard_memory(memory, &bytes, &size))
        return 0;
    char *text = decode_text(format, bytes, size);
    free(bytes);
    const uint32_t error = text == NULL ? NT_ERROR_INVALID_PARAMETER :
                           (SDL_SetClipboardText(text) == 0 ? NT_ERROR_SUCCESS : NT_ERROR_GEN_FAILURE);
    free(text);
    if (error != NT_ERROR_SUCCESS) {
        nt_runtime_release_clipboard_memory(memory, false);
        nt_SetLastError(error);
        return 0;
    }
    selection = memory;
    return memory;
}

void nt_runtime_clipboard_window_closed(uintptr_t window)
{
    if (opened && window_owner == window) {
        opened = emptied = false;
        window_owner = 0;
    }
}

void nt_runtime_close_clipboard(void)
{
    nt_runtime_release_clipboard_memory(selection, true);
    selection = window_owner = 0;
    opened = emptied = false;
}
