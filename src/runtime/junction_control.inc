/* SPDX-License-Identifier: MIT */
/* FSCTL_SET/GET/DELETE_REPARSE_POINT for a scoped MOUNT_POINT directory.
 * This is a native Linux directory with an xattr descriptor: not a kernel
 * mount, a host symlink or an emulated Windows volume. It is intentionally
 * restricted to an already granted root with a physical target directory.
 */
enum { NT_FSCTL_SET_REPARSE = 0x000900a4, NT_FSCTL_GET_REPARSE = 0x000900a8,
       NT_FSCTL_DELETE_REPARSE = 0x000900ac };
#define NT_REPARSE_TAG_MOUNT UINT32_C(0xa0000003)

static uint16_t nt_reparse_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static uint32_t nt_reparse_u32(const uint8_t *p)
{
    return (uint32_t)nt_reparse_u16(p) |
           (uint32_t)nt_reparse_u16(p + 2) << 16;
}

static bool nt_junction_unchanged_inode(int root, const char *path, int opened)
{
    char leaf[NAME_MAX + 1];
    int parent = open_parent(root, path, leaf);
    if (parent < 0)
        return false;
    int current = confined_open_raw(parent, leaf,
                                    O_PATH | O_NOFOLLOW | O_DIRECTORY, 0);
    int error = errno;
    close(parent);
    if (current < 0) {
        errno = error;
        return false;
    }
    struct stat before, after;
    const bool same = fstat(current, &before) == 0 &&
                      fstat(opened, &after) == 0 &&
                      before.st_dev == after.st_dev &&
                      before.st_ino == after.st_ino;
    error = errno;
    close(current);
    if (!same)
        errno = error ? error : EBUSY;
    return same;
}

static bool nt_junction_dir_empty(int pinned)
{
    char proc[64];
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", pinned);
    const int fd = open(proc, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return false;
    DIR *dir = fdopendir(fd);
    if (!dir) {
        const int error = errno;
        close(fd);
        errno = error;
        return false;
    }
    bool empty = true;
    for (;;) {
        errno = 0;
        const struct dirent *entry = readdir(dir);
        if (!entry) {
            if (errno)
                empty = false;
            break;
        }
        if (strcmp(entry->d_name, ".") != 0 &&
            strcmp(entry->d_name, "..") != 0) {
            errno = ENOTEMPTY;
            empty = false;
            break;
        }
    }
    const int error = errno;
    closedir(dir);
    if (!empty)
        errno = error;
    return empty;
}

static bool nt_junction_parse_set(const void *input, uint32_t size,
                                  char dest[PATH_MAX])
{
    if (!input || size < 16 || size > 32768) {
        errno = EINVAL;
        return false;
    }
    const uint8_t *p = input;
    const uint32_t tag = nt_reparse_u32(p);
    const uint16_t data_length = nt_reparse_u16(p + 4);
    if (tag != NT_REPARSE_TAG_MOUNT || data_length < 8 ||
        (uint32_t)data_length + 8 != size || nt_reparse_u16(p + 6) != 0) {
        errno = EINVAL;
        return false;
    }
    const uint16_t offset = nt_reparse_u16(p + 8);
    const uint16_t length = nt_reparse_u16(p + 10);
    const uint16_t print_offset = nt_reparse_u16(p + 12);
    const uint16_t print_length = nt_reparse_u16(p + 14);
    const uint32_t capacity = size - 16;
    if ((offset | length | print_offset | print_length) & 1u ||
        length < 10 || length / 2 >= PATH_MAX ||
        offset > capacity || length > capacity - offset ||
        print_offset > capacity || print_length > capacity - print_offset) {
        errno = EINVAL;
        return false;
    }
    uint16_t wide[PATH_MAX];
    const size_t count = length / 2;
    for (size_t i = 0; i < count; ++i) {
        wide[i] = nt_reparse_u16(p + 16 + offset + 2 * i);
        if (wide[i] == 0) {
            errno = EINVAL;
            return false;
        }
    }
    wide[count] = 0;
    /* Typical NTFS mount-point substitution is \??\C:\Path;
     * accept only a fully qualified guest namespace, never a host path. */
    if (wide[0] != '\\' || wide[1] != '?' ||
        wide[2] != '?' || wide[3] != '\\') {
        errno = EINVAL;
        return false;
    }
    if (nt_WideCharToMultiByte(65001, 0x80, wide + 4, -1,
                               dest, PATH_MAX, NULL, NULL) == 0)
        return false;
    if (!nt_junction_abs_guest_name(dest)) {
        errno = EINVAL;
        return false;
    }
    return true;
}

static int32_t nt_junction_control(int pinned, uint32_t rights,
                                   const char *guest_name, uint32_t control,
                                   const void *input, uint32_t input_size,
                                   void *output, uint32_t output_size,
                                   uint32_t *returned, void *overlapped)
{
    if (!returned || overlapped || !guest_name) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    *returned = 0;
    if ((control == NT_FSCTL_SET_REPARSE ||
         control == NT_FSCTL_DELETE_REPARSE) &&
        (rights & FILE_WRITE) == 0) {
        nt_SetLastError(NT_ERROR_ACCESS_DENIED);
        return 0;
    }
    /* file->path is already the internal canonical path captured when the
     * HANDLE was opened; normalizing it again would make relative names
     * depend on the process CWD after the handle was created. */
    const char *source_relative = guest_name[0] ? guest_name : ".";
    if (strnlen(source_relative, PATH_MAX) >= PATH_MAX) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    int root = duplicate_root(&source_relative);
    if (root < 0) {
        nt_SetLastError(path_error(errno, true));
        return 0;
    }
    struct stat root_info;
    if (fstat(root, &root_info) != 0 ||
        !nt_junction_unchanged_inode(root, source_relative, pinned)) {
        const int error = errno;
        close(root);
        nt_SetLastError(path_error(error, false));
        return 0;
    }

    if (control == NT_FSCTL_GET_REPARSE) {
        if (input || input_size || !output) {
            close(root);
            nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
            return 0;
        }
        struct nt_junction_metadata meta;
        const ssize_t count = read_guest_xattr(pinned, NT_JUNCTION_XATTR,
                                                &meta, sizeof(meta));
        if (!nt_junction_metadata_valid(&meta, count)) {
            close(root);
            nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
            return 0;
        }
        if (meta.root_dev != (uint64_t)root_info.st_dev ||
            meta.root_inode != (uint64_t)root_info.st_ino) {
            close(root);
            nt_SetLastError(NT_ERROR_ACCESS_DENIED);
            return 0;
        }
        uint16_t wide[PATH_MAX + 16];
        const int32_t converted = nt_MultiByteToWideChar(65001, 8, meta.target,
                          -1, wide, PATH_MAX + 16);
        if (converted <= 1) {
            close(root);
            nt_SetLastError(NT_ERROR_NO_UNICODE_TRANSLATION);
            return 0;
        }
        const size_t count_chars = (size_t)converted - 1;
        const size_t sub_bytes = (count_chars + 4) * 2u;
        const size_t print_bytes = count_chars * 2u;
        const size_t total = 16u + sub_bytes + print_bytes;
        if (total > UINT16_MAX + 8u || sub_bytes > UINT16_MAX) {
            close(root);
            nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
            return 0;
        }
        if (output_size < total) {
            close(root);
            nt_SetLastError(NT_ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        uint8_t *out = output;
        memset(out, 0, total);
        security_le32(out, NT_REPARSE_TAG_MOUNT);
        security_le16(out + 4, (uint16_t)(total - 8u));
        security_le16(out + 8, 0);
        security_le16(out + 10, (uint16_t)sub_bytes);
        security_le16(out + 12, (uint16_t)sub_bytes);
        security_le16(out + 14, (uint16_t)print_bytes);
        const uint16_t prefix[4] = {'\\', '?', '?', '\\'};
        for (size_t i = 0; i < 4; ++i)
            security_le16(out + 16 + 2 * i, prefix[i]);
        for (size_t i = 0; i < count_chars; ++i) {
            security_le16(out + 16 + 8 + 2 * i, wide[i]);
            security_le16(out + 16 + sub_bytes + 2 * i, wide[i]);
        }
        close(root);
        *returned = (uint32_t)total;
        return 1;
    }

    if (control == NT_FSCTL_DELETE_REPARSE) {
        if (!input || input_size != 8 || output || output_size ||
            nt_reparse_u32(input) != NT_REPARSE_TAG_MOUNT ||
            nt_reparse_u16((const uint8_t *)input + 4) != 0) {
            close(root);
            nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
            return 0;
        }
        const int removed = clear_guest_xattr(pinned, NT_JUNCTION_XATTR);
        const int error = errno;
        close(root);
        if (removed < 0)
            nt_SetLastError(path_error(error, false));
        return removed == 0;
    }

    if (control != NT_FSCTL_SET_REPARSE || output || output_size) {
        close(root);
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    char target_dos[PATH_MAX];
    if (!nt_junction_parse_set(input, input_size, target_dos)) {
        const int error = errno;
        close(root);
        nt_SetLastError(path_error(error, false));
        return 0;
    }
    char target_normal[PATH_MAX];
    if (!normalize_path(target_dos, target_normal, true)) {
        const int error = errno;
        close(root);
        nt_SetLastError(path_error(error, false));
        return 0;
    }
    const char *target_relative = target_normal[0] ? target_normal : ".";
    int target_grant = duplicate_root(&target_relative);
    struct stat target_root_info;
    const bool same_grant = target_grant >= 0 &&
                            fstat(target_grant, &target_root_info) == 0 &&
                            root_info.st_dev == target_root_info.st_dev &&
                            root_info.st_ino == target_root_info.st_ino;
    if (target_grant >= 0)
        close(target_grant);
    if (!same_grant) {
        close(root);
        nt_SetLastError(NT_ERROR_ACCESS_DENIED);
        return 0;
    }
    const int dest_fd = nt_junction_open_physical(root, target_relative);
    const int dest_error = errno;
    if (dest_fd < 0) {
        close(root);
        nt_SetLastError(path_error(dest_error, false));
        return 0;
    }
    struct stat source_st, dest_st;
    const bool different = fstat(pinned, &source_st) == 0 &&
                           fstat(dest_fd, &dest_st) == 0 &&
                           (source_st.st_dev != dest_st.st_dev ||
                            source_st.st_ino != dest_st.st_ino);
    close(dest_fd);
    if (!different || !nt_junction_dir_empty(pinned)) {
        close(root);
        nt_SetLastError(NT_ERROR_ACCESS_DENIED);
        return 0;
    }
    /* Retain the v1 xattr name and header, but store the compact layout.
     * The reader accepts both compact and historical PATH_MAX layouts. */
    struct nt_junction_compact_metadata meta = {
        .magic = NT_JUNCTION_MAGIC, .version = 1,
        .root_dev = (uint64_t)root_info.st_dev,
        .root_inode = (uint64_t)root_info.st_ino
    };
    char canonical[PATH_MAX + 16];
    if (!copy_dos_path(target_normal, canonical, sizeof(canonical))) {
        const int error = errno;
        close(root);
        nt_SetLastError(path_error(error, false));
        return 0;
    }
    /* An overlong Windows target is unsupported, never an unchecked
     * memcpy into the compact on-disk Linux xattr record. */
    const size_t target_bytes = strlen(canonical) + 1;
    if (target_bytes > sizeof(meta.target)) {
        close(root);
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    memcpy(meta.target, canonical, target_bytes);
    const int written = write_guest_xattr(pinned, NT_JUNCTION_XATTR,
                                          &meta, sizeof(meta));
    const int error = errno;
    close(root);
    if (written < 0)
        nt_SetLastError(path_error(error, false));
    return written == 0;
}
