/* SPDX-License-Identifier: MIT */
#include "shell.h"
#include "child_process.h"
#include "dialogs.h"
#include "winbase.h"
#include "winfile.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { ITEM_SIGNATURE = 0x4e54, COMMAND_LIMIT = 32768,
       SFGAO_FILESYSTEM = 0x40000000u, SFGAO_FOLDER = 0x20000000u,
       SFGAO_FILESYSANCESTOR = 0x10000000u };
struct path_item { uint16_t bytes, signature, text[]; };

static char *narrow_path(const uint16_t *path)
{
    size_t length = path ? nt_runtime_wide_length(path, PATH_MAX) : 0;
    return length && length < PATH_MAX ? nt_runtime_utf16_to_utf8(path, length) : NULL;
}
static bool locate_path(const char *path, char dos[PATH_MAX], bool *folder)
{
    const int fd = nt_runtime_open_guest_directory(path, dos, PATH_MAX);
    if (fd >= 0) {
        close(fd);
        *folder = true;
        return true;
    }
    bool exists = false;
    *folder = false;
    return nt_runtime_check_guest_file(path, dos, PATH_MAX, &exists) && exists;
}

int32_t NT_MS_ABI nt_SHParseDisplayName(const uint16_t *name, void *context, void **pidl,
                                      uint32_t requested, uint32_t *attributes)
{
    if (attributes)
        *attributes = 0;
    if (!pidl)
        return (int32_t)0x80004003;
    *pidl = NULL;
    if (context)
        return (int32_t)0x80004001;
    char *input = narrow_path(name);
    if (!input)
        return (int32_t)0x80070057;
    char dos[PATH_MAX];
    bool folder;
    const bool found = locate_path(input, dos, &folder);
    free(input);
    if (!found)
        return (int32_t)0x80070002;
    uint16_t *wide = nt_runtime_utf8_to_wide(dos);
    if (!wide)
        return (int32_t)0x8007000e;
    size_t text_bytes = (nt_runtime_wide_length(wide, PATH_MAX) + 1) * sizeof(*wide);
    size_t item_bytes = sizeof(struct path_item) + text_bytes;
    struct path_item *item = calloc(1, item_bytes + sizeof(uint16_t));
    if (!item) {
        free(wide);
        return (int32_t)0x8007000e;
    }
    item->bytes = (uint16_t)item_bytes;
    item->signature = ITEM_SIGNATURE;
    memcpy(item->text, wide, text_bytes);
    free(wide);
    *pidl = item;
    if (attributes)
        *attributes = requested & (SFGAO_FILESYSTEM | (folder ? SFGAO_FOLDER | SFGAO_FILESYSANCESTOR : 0));
    return 0;
}

static char *pidl_path(const struct path_item *item)
{
    if (!item || item->bytes < sizeof(*item) + 2 * sizeof(uint16_t) ||
        item->bytes > sizeof(*item) + PATH_MAX * sizeof(uint16_t) ||
        item->bytes % sizeof(uint16_t) || item->signature != ITEM_SIGNATURE)
        return NULL;
    size_t units = (item->bytes - sizeof(*item)) / sizeof(uint16_t);
    if (nt_runtime_wide_length(item->text, units) != units - 1)
        return NULL;
    return nt_runtime_utf16_to_utf8(item->text, units - 1);
}

int32_t NT_MS_ABI nt_SHOpenFolderAndSelectItems(const void *folder, uint32_t count,
                                              const void *const *items, uint32_t flags)
{
    if (count > 1 || flags)
        return (int32_t)0x80004001;
    if (count && !items)
        return (int32_t)0x80070057;
    char *input = pidl_path(folder);
    if (!input)
        return (int32_t)0x80070057;
    char dos[PATH_MAX], parent[PATH_MAX];
    bool is_directory;
    bool found = locate_path(input, dos, &is_directory);
    free(input);
    if (!found)
        return (int32_t)0x80070002;
    const char *selection = NULL;
    if (count) {
        input = pidl_path(items[0]);
        if (!input || !is_directory) {
            free(input);
            return (int32_t)0x80070057;
        }
        bool child_directory;
        found = locate_path(input, parent, &child_directory);
        free(input);
        if (!found)
            return (int32_t)0x80070002;
        size_t length = strlen(dos);
        const char *after = parent + length;
        if (strncmp(parent, dos, length) ||
            (dos[length - 1] != '\\' && *after++ != '\\') || !*after || strchr(after, '\\'))
            return (int32_t)0x80070057;
        selection = after;
    } else if (!is_directory) {
        char *leaf = strrchr(dos, '\\');
        if (!leaf)
            return (int32_t)0x80070057;
        strcpy(parent, leaf + 1);
        selection = parent;
        if (leaf == dos + 2)
            leaf[1] = 0;
        else
            *leaf = 0;
    }
    return nt_runtime_show_folder(dos, selection, 0) ? 0 : (int32_t)0x80004005;
}

static bool wide_verb(const uint16_t *verb, const char *expected)
{
    while (*expected) {
        uint16_t ch = *verb++;
        if (ch >= 'A' && ch <= 'Z')
            ch += 'a' - 'A';
        if (ch != (unsigned char)*expected++)
            return false;
    }
    return *verb == 0;
}

uintptr_t NT_MS_ABI nt_ShellExecuteW(uintptr_t owner, const uint16_t *operation,
        const uint16_t *file, const uint16_t *parameters, const uint16_t *directory, int32_t show)
{
    bool open = !operation || !*operation || wide_verb(operation, "open");
    if (!open && !wide_verb(operation, "explore"))
        return 31; /* SE_ERR_NOASSOC */
    char *input = narrow_path(file);
    if (!input)
        return 2;
    char dos[PATH_MAX];
    bool folder;
    bool found = locate_path(input, dos, &folder);
    free(input);
    if (!found)
        return 2;
    if (folder)
        return nt_runtime_show_folder(dos, NULL, owner) ? 33 : 5;
    const char *extension = strrchr(dos, '.');
    if (!open || !extension || !nt_module_name_equal(extension, ".exe"))
        return 31;
    size_t filename_length = nt_runtime_wide_length(file, PATH_MAX);
    size_t args_length = parameters ? nt_runtime_wide_length(parameters, COMMAND_LIMIT) : 0;
    if (filename_length + args_length + 4 > COMMAND_LIMIT)
        return 31;
    uint16_t *command = malloc((filename_length + args_length + 4) * sizeof(*command));
    if (!command)
        return 8;
    size_t length = 0;
    command[length++] = '"';
    memcpy(command + length, file, filename_length * sizeof(*command));
    length += filename_length;
    command[length++] = '"';
    if (args_length) {
        command[length++] = ' ';
        memcpy(command + length, parameters, args_length * sizeof(*command));
        length += args_length;
    }
    command[length] = 0;
    struct nt_process_information info;
    const struct nt_startup_info startup = {.size = sizeof(startup), .flags = 1, .show_window = (uint16_t)show};
    bool started = nt_CreateProcessW(file, command, NULL, NULL, 0, 0, NULL, directory, &startup, &info);
    free(command);
    if (!started) {
        uint32_t error = nt_GetLastError();
        return error == 2 || error == 3 || error == 8 ? error : 5;
    }
    nt_CloseHandle(info.process);
    nt_CloseHandle(info.thread);
    return 33;
}
