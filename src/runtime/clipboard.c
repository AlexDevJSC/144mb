/* SPDX-License-Identifier: MIT */
#include "windesktop.h"

#include <SDL.h>
#include <stdlib.h>
#include <string.h>

enum { CF_TEXT = 1, CF_UNICODETEXT = 13, CLIPBOARD_NOT_OPEN = 1418 };

/* The desktop thread owns the selection and is the only guest clipboard caller. */
static bool opened, emptied;
static uintptr_t window_owner, selection, received[2];

static void release_received(void)
{
    for (size_t i = 0; i < sizeof(received) / sizeof(received[0]); ++i) {
        nt_runtime_release_clipboard_memory(received[i], true);
        received[i] = 0;
    }
}

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
    release_received();
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
    release_received();
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
        release_received();
        opened = emptied = false;
        window_owner = 0;
    }
}

void nt_runtime_close_clipboard(void)
{
    release_received();
    nt_runtime_release_clipboard_memory(selection, true);
    selection = window_owner = 0;
    opened = emptied = false;
}

uintptr_t NT_MS_ABI nt_GetClipboardData(uint32_t format)
{
    if (!nt_runtime_desktop_active() || !opened) {
        nt_SetLastError(CLIPBOARD_NOT_OPEN);
        return 0;
    }
    if (format != CF_TEXT && format != CF_UNICODETEXT) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    const size_t index = format == CF_TEXT ? 0 : 1;
    if (received[index] != 0)
        return received[index];
    if (selection == 0 && !SDL_HasClipboardText())
        return 0;
    char *utf8 = SDL_GetClipboardText();
    uint16_t *wide = utf8 != NULL ? nt_runtime_utf8_to_wide(utf8) : NULL;
    SDL_free(utf8);
    if (wide == NULL) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    const int32_t narrow_size = format == CF_TEXT ?
        nt_WideCharToMultiByte(1252, 0, wide, -1, NULL, 0, NULL, NULL) : 0;
    const size_t bytes = format == CF_TEXT ? (size_t)narrow_size :
                         (nt_runtime_wide_length(wide, INT32_MAX) + 1) * sizeof(*wide);
    const uintptr_t handle = bytes != 0 ? nt_GlobalAlloc(2, bytes) : 0;
    if (handle != 0) {
        void *output = nt_GlobalLock(handle);
        if (output == NULL) {
            nt_GlobalFree(handle);
        } else {
            if (format == CF_TEXT)
                nt_WideCharToMultiByte(1252, 0, wide, -1, output, narrow_size, NULL, NULL);
            else
                memcpy(output, wide, bytes);
            nt_GlobalUnlock(handle);
            unsigned char *copy;
            size_t copied;
            if (nt_runtime_claim_clipboard_memory(handle, &copy, &copied)) {
                free(copy);
                received[index] = handle;
            } else {
                nt_GlobalFree(handle);
            }
        }
    }
    free(wide);
    return received[index];
}
