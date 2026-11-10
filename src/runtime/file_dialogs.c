/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "dialogs.h"

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

enum {
    OFN_READONLY = 0x1, OFN_OVERWRITEPROMPT = 0x2, OFN_HIDEREADONLY = 0x4,
    OFN_NOCHANGEDIR = 0x8, OFN_EXTENSIONDIFFERENT = 0x400, OFN_PATHMUSTEXIST = 0x800,
    OFN_FILEMUSTEXIST = 0x1000, OFN_CREATEPROMPT = 0x2000, OFN_EXPLORER = 0x80000,
    OFN_NODEREFERENCELINKS = 0x100000, OFN_DONTADDTORECENT = 0x2000000,
    OFN_FLAGS = OFN_READONLY | OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_NOCHANGEDIR |
                OFN_EXTENSIONDIFFERENT | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_CREATEPROMPT |
                OFN_EXPLORER | OFN_NODEREFERENCELINKS | OFN_DONTADDTORECENT,
    CDERR_STRUCTSIZE = 1, CDERR_INITIALIZATION = 2,
    FNERR_INVALIDFILENAME = 0x3002, FNERR_BUFFERTOOSMALL = 0x3003,
    ID_PATH = 100, ID_LIST, ID_EDIT, ID_UP, ID_FILTER, ID_READONLY, ID_STATUS,
    MAX_FILTERS = 16, MAX_FILTER_BYTES = 4096, MAX_ENTRIES = 256, MAX_SCAN = 4096, MAX_PATH_BYTES = 4096,
    CB_ADDSTRING = 0x143, CB_GETCURSEL = 0x147, CB_SETCURSEL = 0x14e
};

_Static_assert(offsetof(struct nt_open_file_name, reserved) == 136, "OPENFILENAME_SIZE_VERSION_400A");
_Static_assert(sizeof(struct nt_open_file_name) == 152, "OPENFILENAMEA layout");

static _Thread_local uint32_t dialog_error;

struct file_dialog {
    struct nt_open_file_name *request;
    bool save, read_only;
    int result;
    uint32_t error, filter_count, filter_index;
    char directory[MAX_PATH_BYTES], path[MAX_PATH_BYTES], filters[MAX_FILTER_BYTES];
    const char *names[MAX_FILTERS], *patterns[MAX_FILTERS];
};

struct file_entry {
    char name[256];
    bool directory;
};

static bool parse_filters(struct file_dialog *dialog, const char *filters)
{
    if (filters == NULL) {
        dialog->names[0] = "All files";
        dialog->patterns[0] = "*";
        dialog->filter_count = 1;
        return true;
    }
    size_t used = 0;
    while (used < sizeof(dialog->filters) && filters[used] != '\0') {
        if (dialog->filter_count == MAX_FILTERS)
            return false;
        const size_t first = used;
        const size_t name_length = strnlen(filters + used, sizeof(dialog->filters) - used);
        used += name_length + 1;
        if (used >= sizeof(dialog->filters))
            return false;
        const size_t pattern = used;
        const size_t pattern_length = strnlen(filters + used, sizeof(dialog->filters) - used);
        used += pattern_length + 1;
        if (pattern_length == 0 || used >= sizeof(dialog->filters))
            return false;
        memcpy(dialog->filters + first, filters + first, used - first);
        const uint32_t index = dialog->filter_count++;
        dialog->names[index] = dialog->filters + first;
        dialog->patterns[index] = dialog->filters + pattern;
    }
    return dialog->filter_count != 0 && used < sizeof(dialog->filters);
}

static bool matches_filter(const char *name, const char *filter)
{
    while (*filter != '\0') {
        char pattern[256];
        const size_t length = strcspn(filter, ";");
        if (length > 0 && length < sizeof(pattern)) {
            memcpy(pattern, filter, length);
            pattern[length] = '\0';
            if (strcmp(pattern, "*.*") == 0 || fnmatch(pattern, name, FNM_CASEFOLD) == 0)
                return true;
        }
        filter += length;
        if (*filter == ';')
            ++filter;
    }
    return false;
}

static int compare_entry(const void *left, const void *right)
{
    const struct file_entry *a = left, *b = right;
    if (a->directory != b->directory)
        return a->directory ? -1 : 1;
    const int folded = strcasecmp(a->name, b->name);
    return folded != 0 ? folded : strcmp(a->name, b->name);
}

static void set_control_text(uintptr_t window, int32_t id, const char *text)
{
    nt_SendDlgItemMessageA(window, id, NT_WM_SETTEXT, 0, (intptr_t)text);
}

static bool refresh_list(uintptr_t window, struct file_dialog *dialog, int fd)
{
    DIR *directory = fdopendir(fd);
    if (directory == NULL) {
        close(fd);
        return false;
    }
    struct file_entry *entries = calloc(MAX_ENTRIES, sizeof(*entries));
    if (entries == NULL) {
        closedir(directory);
        return false;
    }
    size_t count = 0;
    bool ok = true;
    for (size_t scanned = 0; scanned < MAX_SCAN && count < MAX_ENTRIES; ++scanned) {
        errno = 0;
        const struct dirent *entry = readdir(directory);
        if (entry == NULL) {
            ok = errno == 0;
            break;
        }
        bool is_directory;
        if (!nt_runtime_guest_directory_entry(dirfd(directory), entry->d_name, &is_directory) ||
            (!is_directory && !matches_filter(entry->d_name, dialog->patterns[dialog->filter_index])))
            continue;
        const size_t length = strlen(entry->d_name);
        if (length >= sizeof(entries[count].name))
            continue;
        memcpy(entries[count].name, entry->d_name, length + 1);
        entries[count++].directory = is_directory;
    }
    closedir(directory);
    if (ok) {
        qsort(entries, count, sizeof(*entries), compare_entry);
        nt_SendDlgItemMessageA(window, ID_LIST, NT_LB_RESETCONTENT, 0, 0);
        for (size_t i = 0; i < count; ++i) {
            char display[sizeof(entries[i].name) + 1];
            snprintf(display, sizeof(display), "%s%s", entries[i].name, entries[i].directory ? "/" : "");
            if (nt_SendDlgItemMessageA(window, ID_LIST, NT_LB_ADDSTRING, 0, (intptr_t)display) < 0) {
                ok = false;
                break;
            }
        }
    }
    free(entries);
    return ok;
}

static bool visit(uintptr_t window, struct file_dialog *dialog, const char *name)
{
    char path[MAX_PATH_BYTES];
    const int directory = nt_runtime_open_guest_directory(name, path, sizeof(path));
    if (directory < 0)
        return false;
    if (!refresh_list(window, dialog, directory)) {
        dialog->error = CDERR_INITIALIZATION;
        dialog->result = -1;
        return false;
    }
    strcpy(dialog->directory, path);
    set_control_text(window, ID_PATH, path);
    set_control_text(window, ID_EDIT, "");
    set_control_text(window, ID_STATUS, "");
    return true;
}

static void visit_parent(uintptr_t window, struct file_dialog *dialog)
{
    char path[MAX_PATH_BYTES];
    strcpy(path, dialog->directory);
    char *slash = strrchr(path, '\\');
    if (slash == path + 2)
        slash[1] = '\0';
    else
        *slash = '\0';
    visit(window, dialog, path);
}

static void select_list_entry(uintptr_t window)
{
    const intptr_t index = nt_SendDlgItemMessageA(window, ID_LIST, NT_LB_GETCURSEL, 0, 0);
    if (index >= 0) {
        char name[MAX_PATH_BYTES];
        if (nt_SendDlgItemMessageA(window, ID_LIST, NT_LB_GETTEXT, (uintptr_t)index, (intptr_t)name) >= 0)
            set_control_text(window, ID_EDIT, name);
    }
}

static const char *filename(const char *path)
{
    const char *separator = strrchr(path, '\\');
    return separator != NULL ? separator + 1 : path;
}

static bool apply_default_extension(char *path, const char *extension)
{
    const char *leaf = path;
    for (const char *next = path; *next != '\0'; ++next) {
        if (*next == '/' || *next == '\\')
            leaf = next + 1;
    }
    const size_t length = strlen(path), suffix = extension != NULL ? strlen(extension) : 0;
    if (suffix == 0 || strchr(leaf, '.') != NULL || *leaf == '\0')
        return true;
    if (length + suffix + 2 > MAX_PATH_BYTES)
        return false;
    path[length] = '.';
    memcpy(path + length + 1, extension, suffix + 1);
    return true;
}

static void accept_file(uintptr_t window, struct file_dialog *dialog)
{
    char name[MAX_PATH_BYTES], requested[MAX_PATH_BYTES];
    if (nt_GetDlgItemTextA(window, ID_EDIT, name, sizeof(name)) == 0) {
        set_control_text(window, ID_STATUS, "Enter or select a file name.");
        return;
    }
    const bool absolute = name[0] == '\\' || name[0] == '/' || name[1] == ':';
    const char *separator = dialog->directory[strlen(dialog->directory) - 1] == '\\' ? "" : "\\";
    const int length = absolute ? snprintf(requested, sizeof(requested), "%s", name) :
                       snprintf(requested, sizeof(requested), "%s%s%s", dialog->directory, separator, name);
    if (length < 0 || length >= (int)sizeof(requested)) {
        set_control_text(window, ID_STATUS, "The file path is too long.");
        return;
    }
    if (visit(window, dialog, requested) || dialog->result != 0)
        return;

    bool exists;
    if (!apply_default_extension(requested, dialog->request->default_extension) ||
        !nt_runtime_check_guest_file(requested, dialog->path, sizeof(dialog->path), &exists)) {
        set_control_text(window, ID_STATUS, "Select a regular file in the guest C: drive.");
        return;
    }
    const uint32_t flags = dialog->request->flags;
    if (!exists && (flags & OFN_FILEMUSTEXIST) != 0 && (flags & OFN_CREATEPROMPT) == 0) {
        set_control_text(window, ID_STATUS, "The selected file does not exist.");
        return;
    }
    if (exists && dialog->save && (flags & OFN_OVERWRITEPROMPT) != 0) {
        if (nt_MessageBoxA(window, "Replace the existing file?", "Confirm save", 0x124) != 6)
            return;
    } else if (!exists && !dialog->save && (flags & OFN_CREATEPROMPT) != 0) {
        if (nt_MessageBoxA(window, "Use this new file name?", "Confirm open", 0x124) != 6)
            return;
    }
    if (!dialog->save && (flags & OFN_HIDEREADONLY) == 0)
        dialog->read_only = nt_IsDlgButtonChecked(window, ID_READONLY) != 0;
    dialog->result = NT_IDOK;
}

static intptr_t NT_MS_ABI file_dialog_proc(uintptr_t window, uint32_t message, uintptr_t wparam, intptr_t lparam)
{
    (void)lparam;
    struct file_dialog *dialog = (void *)nt_GetWindowLongPtrA(window, NT_DWLP_USER);
    if (message == NT_WM_INITDIALOG)
        return 1;
    if (message == NT_WM_CLOSE || (message == NT_WM_COMMAND && (uint16_t)wparam == NT_IDCANCEL)) {
        dialog->result = NT_IDCANCEL;
        return 1;
    }
    if (message != NT_WM_COMMAND)
        return 0;
    const uintptr_t notification = wparam >> 16;
    switch ((uint16_t)wparam) {
    case NT_IDOK: accept_file(window, dialog); break;
    case ID_UP: visit_parent(window, dialog); break;
    case ID_LIST: if (notification == 1) select_list_entry(window); break;
    case ID_FILTER:
        if (notification == 1) {
            const intptr_t selected = nt_SendDlgItemMessageA(window, ID_FILTER, CB_GETCURSEL, 0, 0);
            if (selected >= 0 && selected < dialog->filter_count) {
                dialog->filter_index = (uint32_t)selected;
                visit(window, dialog, dialog->directory);
            }
        }
        break;
    default: return 0;
    }
    return 1;
}

static bool add_file_controls(uintptr_t window, const struct file_dialog *dialog)
{
    const bool read_only = !dialog->save && (dialog->request->flags & OFN_HIDEREADONLY) == 0;
    if (nt_runtime_add_dialog_control(window, "STATIC", "", ID_PATH, 0, 12, 12, 458, 22) == 0 ||
        nt_runtime_add_dialog_control(window, "BUTTON", "Up", ID_UP, NT_WS_TABSTOP, 482, 8, 66, 24) == 0 ||
        nt_runtime_add_dialog_control(window, "LISTBOX", "", ID_LIST, NT_WS_TABSTOP, 12, 40, 536, 216) == 0 ||
        nt_runtime_add_dialog_control(window, "STATIC", "File name", -1, 0, 12, 264, 90, 22) == 0 ||
        nt_runtime_add_dialog_control(window, "EDIT", "", ID_EDIT, NT_WS_TABSTOP, 106, 263, 442, 24) == 0 ||
        nt_runtime_add_dialog_control(window, "STATIC", "File type", -1, 0, 12, 298, 90, 22) == 0 ||
        nt_runtime_add_dialog_control(window, "COMBOBOX", "", ID_FILTER, NT_WS_TABSTOP | 3u,
                                      106, 296, 442, 110) == 0 ||
        nt_runtime_add_dialog_control(window, "STATIC", "", ID_STATUS, 0, 12, 326, 536, 24) == 0 ||
        nt_runtime_add_dialog_control(window, "BUTTON", dialog->save ? "Save" : "Open", NT_IDOK,
                                      NT_WS_TABSTOP | 1u, 346, 361, 96, 28) == 0 ||
        nt_runtime_add_dialog_control(window, "BUTTON", "Cancel", NT_IDCANCEL,
                                      NT_WS_TABSTOP, 452, 361, 96, 28) == 0 ||
        (read_only && nt_runtime_add_dialog_control(window, "BUTTON", "Read-only", ID_READONLY,
                                      NT_WS_TABSTOP | 3u, 12, 361, 140, 28) == 0))
        return false;
    for (uint32_t i = 0; i < dialog->filter_count; ++i) {
        if (nt_SendDlgItemMessageA(window, ID_FILTER, CB_ADDSTRING, 0, (intptr_t)dialog->names[i]) < 0)
            return false;
    }
    nt_SendDlgItemMessageA(window, ID_FILTER, CB_SETCURSEL, dialog->filter_index, 0);
    if (read_only)
        nt_CheckDlgButton(window, ID_READONLY, dialog->read_only);
    return true;
}

static bool file_result(struct file_dialog *dialog)
{
    struct nt_open_file_name *request = dialog->request;
    const size_t length = strlen(dialog->path) + 1;
    if (length > request->max_file) {
        const uint16_t required = (uint16_t)length;
        if (request->max_file >= sizeof(required))
            memcpy(request->file, &required, sizeof(required));
        dialog_error = FNERR_BUFFERTOOSMALL;
        return false;
    }
    const char *leaf = filename(dialog->path);
    const char *extension = strrchr(leaf, '.');
    memcpy(request->file, dialog->path, length);
    request->file_offset = (uint16_t)(leaf - dialog->path);
    request->extension_offset = extension != NULL ? (uint16_t)(extension + 1 - dialog->path) :
                                (uint16_t)(length - 1);
    request->flags &= ~(OFN_READONLY | OFN_EXTENSIONDIFFERENT);
    if (dialog->read_only)
        request->flags |= OFN_READONLY;
    if (request->default_extension != NULL && request->default_extension[0] != '\0' &&
        extension != NULL && strcasecmp(request->default_extension, extension + 1) != 0)
        request->flags |= OFN_EXTENSIONDIFFERENT;
    request->filter_index = dialog->filter_index + 1;
    if (request->file_title != NULL && request->max_file_title > 0) {
        size_t count = strlen(leaf);
        if (count >= request->max_file_title)
            count = request->max_file_title - 1;
        memcpy(request->file_title, leaf, count);
        request->file_title[count] = '\0';
    }
    return true;
}

static int32_t file_dialog(struct nt_open_file_name *request, bool save)
{
    dialog_error = 0;
    if (request == NULL || (request->size != offsetof(struct nt_open_file_name, reserved) &&
                            request->size != sizeof(*request))) {
        dialog_error = CDERR_STRUCTSIZE;
        return 0;
    }
    if (request->file == NULL || request->max_file == 0 ||
        strnlen(request->file, request->max_file < MAX_PATH_BYTES ? request->max_file : MAX_PATH_BYTES) >=
        (request->max_file < MAX_PATH_BYTES ? request->max_file : MAX_PATH_BYTES)) {
        dialog_error = FNERR_INVALIDFILENAME;
        return 0;
    }
    struct file_dialog dialog = {.request = request, .save = save, .read_only = (request->flags & OFN_READONLY) != 0};
    if ((request->flags & ~OFN_FLAGS) != 0 || request->custom_filter != NULL ||
        (request->default_extension != NULL && strnlen(request->default_extension, 256) == 256) ||
        (request->size == sizeof(*request) &&
         (request->reserved != NULL || request->reserved_count != 0 || request->extended_flags != 0)) ||
        !parse_filters(&dialog, request->filter)) {
        dialog_error = CDERR_INITIALIZATION;
        return 0;
    }
    dialog.filter_index = request->filter_index > 0 && request->filter_index <= dialog.filter_count ?
                          request->filter_index - 1 : 0;
    const char *title = request->title != NULL ? request->title : save ? "Save as" : "Open";
    const uintptr_t window = nt_runtime_create_dialog(title, request->owner, 560, 402, file_dialog_proc, (intptr_t)&dialog);
    if (window == 0) {
        dialog_error = CDERR_INITIALIZATION;
        return 0;
    }
    if (!add_file_controls(window, &dialog) ||
        (!visit(window, &dialog, request->initial_directory) &&
         (dialog.result != 0 || !visit(window, &dialog, NULL)))) {
        nt_DestroyWindow(window);
        dialog_error = dialog.error != 0 ? dialog.error : CDERR_INITIALIZATION;
        return 0;
    }
    set_control_text(window, ID_EDIT, request->file);
    nt_runtime_focus_control(window, nt_runtime_dialog_item(window, ID_EDIT), false);
    nt_SendDlgItemMessageA(window, ID_EDIT, 0xb1, 0, -1); /* EM_SETSEL */
    const int result = nt_runtime_run_modal(window, request->owner, &dialog.result);
    dialog_error = result < 0 ? (dialog.error != 0 ? dialog.error : CDERR_INITIALIZATION) : 0;
    return result == NT_IDOK && file_result(&dialog);
}

int32_t NT_MS_ABI nt_GetOpenFileNameA(struct nt_open_file_name *request)
{
    return file_dialog(request, false);
}

int32_t NT_MS_ABI nt_GetSaveFileNameA(struct nt_open_file_name *request)
{
    return file_dialog(request, true);
}

uint32_t NT_MS_ABI nt_CommDlgExtendedError(void)
{
    return dialog_error;
}
