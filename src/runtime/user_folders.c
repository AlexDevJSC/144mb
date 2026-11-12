/* SPDX-License-Identifier: MIT */
#include "user_folders.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

_Static_assert(sizeof(struct nt_folder_id) == 16, "Windows GUID layout");

#define FOLDER(name, a, b, c, ...) \
    {name, "Users/guest/" name, NT_GUEST_USER_PROFILE "\\" name, {a, b, c, {__VA_ARGS__}}}
const struct nt_user_folder nt_user_folders[NT_USER_FOLDER_COUNT] = {
    [NT_USER_FOLDER_DESKTOP] =
        FOLDER("Desktop",   0xb4bfcc3a, 0xdb2c, 0x424c, 0xb0, 0x29, 0x7f, 0xe9, 0x9a, 0x87, 0xc6, 0x41),
    [NT_USER_FOLDER_DOCUMENTS] =
        FOLDER("Documents", 0xfdd39ad0, 0x238f, 0x46af, 0xad, 0xb4, 0x6c, 0x85, 0x48, 0x03, 0x69, 0xc7),
    [NT_USER_FOLDER_DOWNLOADS] =
        FOLDER("Downloads", 0x374de290, 0x123f, 0x4565, 0x91, 0x64, 0x39, 0xc4, 0x92, 0x5e, 0x46, 0x7b),
    [NT_USER_FOLDER_MUSIC] =
        FOLDER("Music",     0x4bd8d571, 0x6d19, 0x48d3, 0xbe, 0x97, 0x42, 0x22, 0x20, 0x08, 0x0e, 0x43),
    [NT_USER_FOLDER_PICTURES] =
        FOLDER("Pictures",  0x33e28130, 0x4e1e, 0x4676, 0x83, 0x5a, 0x98, 0x39, 0x5c, 0x3b, 0xc3, 0xbb),
    [NT_USER_FOLDER_VIDEOS] =
        FOLDER("Videos",    0x18989b1d, 0x99b5, 0x455b, 0x84, 0x1c, 0xab, 0x7c, 0x74, 0xe4, 0xdd, 0xfc)
};
#undef FOLDER

static const struct {
    struct nt_folder_id id;
    const char *path;
} profile_folders[] = {
    {{0x5e6c858f, 0x0e22, 0x4760, {0x9a, 0xfe, 0xea, 0x33, 0x17, 0xb6, 0x71, 0x73}}, NT_GUEST_USER_PROFILE},
    {{0x3eb685db, 0x65f9, 0x4cf6, {0xa0, 0x3a, 0xe3, 0xef, 0x65, 0x72, 0x9f, 0x3d}}, NT_GUEST_USER_PROFILE "\\AppData\\Roaming"},
    {{0xf1b32785, 0x6fba, 0x4fcf, {0x9d, 0x55, 0x7b, 0x8e, 0x7f, 0x15, 0x70, 0x91}}, NT_GUEST_USER_PROFILE "\\AppData\\Local"}
};

int32_t NT_MS_ABI nt_SHGetKnownFolderPath(const struct nt_folder_id *id, uint32_t flags,
                                         uintptr_t token, uint16_t **output)
{
    const uint32_t dont_verify = 0x4000;
    const int32_t invalid_argument = (int32_t)UINT32_C(0x80070057);
    if (output == NULL)
        return (int32_t)UINT32_C(0x80004003);
    *output = NULL;
    if (id == NULL)
        return invalid_argument;
    if (token != 0 || (flags & ~dont_verify) != 0)
        return (int32_t)UINT32_C(0x80004001);
    const char *path = NULL;
    for (size_t i = 0; i < NT_USER_FOLDER_COUNT; ++i) {
        if (memcmp(id, &nt_user_folders[i].id, sizeof(*id)) == 0)
            path = nt_user_folders[i].dos_path;
    }
    for (size_t i = 0; i < sizeof(profile_folders) / sizeof(profile_folders[0]); ++i) {
        if (memcmp(id, &profile_folders[i].id, sizeof(*id)) == 0)
            path = profile_folders[i].path;
    }
    if (path == NULL)
        return invalid_argument;
    if ((flags & dont_verify) == 0) {
        char canonical[PATH_MAX];
        const int directory = nt_runtime_open_guest_directory(path, canonical, sizeof(canonical));
        if (directory < 0) {
            const uint32_t error = errno == ENOENT || errno == ENOTDIR ? NT_ERROR_PATH_NOT_FOUND : NT_ERROR_ACCESS_DENIED;
            return (int32_t)(UINT32_C(0x80070000) | error);
        }
        close(directory);
    }
    const size_t length = strlen(path) + 1;
    uint16_t *copy = malloc(length * sizeof(*copy));
    if (copy == NULL)
        return (int32_t)UINT32_C(0x8007000e);
    for (size_t i = 0; i < length; ++i)
        copy[i] = (unsigned char)path[i];
    *output = copy;
    return 0;
}
