/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_USER_FOLDERS_H
#define NTNATIVE_USER_FOLDERS_H

#include "compat.h"

#define NT_GUEST_USER_PROFILE "C:\\Users\\guest"
#define NT_GUEST_FOLDER_MOUNTS "/run/ntnative/folders"

enum {
    NT_USER_FOLDER_DESKTOP, NT_USER_FOLDER_DOCUMENTS, NT_USER_FOLDER_DOWNLOADS,
    NT_USER_FOLDER_MUSIC, NT_USER_FOLDER_PICTURES, NT_USER_FOLDER_VIDEOS,
    NT_USER_FOLDER_COUNT
};

struct nt_folder_id {
    uint32_t first;
    uint16_t second, third;
    uint8_t rest[8];
};

struct nt_user_folder {
    const char *name, *relative, *dos_path;
    struct nt_folder_id id;
};

extern const struct nt_user_folder nt_user_folders[NT_USER_FOLDER_COUNT];

bool nt_runtime_set_user_folder_root(size_t folder, const char *directory);
unsigned int nt_runtime_shared_user_folders(void);
int32_t NT_MS_ABI nt_SHGetKnownFolderPath(const struct nt_folder_id *id, uint32_t flags,
                                         uintptr_t token, uint16_t **output);

#endif
