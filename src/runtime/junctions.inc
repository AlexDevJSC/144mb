/* SPDX-License-Identifier: MIT */
/* Native directory junctions are persistent metadata on a real Linux
 * directory. No Linux symlink/mount is followed or created by this bridge:
 * a validated same-grant target is interpreted at the NT path boundary.
 * The host retains final authority through openat2, SELinux and the
 * explicitly granted O_PATH root. */
#define WIN_JUNCTION_XATTR "user.ntnative.junction"
enum { WIN_JUNCTION_MAX_HOPS = 16 };
struct nt_junction_record {
    char signature[4]; /* JNC1 */
    uint64_t root_device, root_inode;
    char target_relative[PATH_MAX];
    char target_dos[PATH_MAX];
};

static bool junction_relative_safe(const char *path)
{
    if (!path || !path[0] || path[0] == '/' ||
        strnlen(path, PATH_MAX) >= PATH_MAX)
        return false;
    const char *cursor = path;
    while (*cursor) {
        const char *slash = strchr(cursor, '/');
        const size_t count = slash ? (size_t)(slash - cursor) : strlen(cursor);
        if (!count || count > NAME_MAX ||
            (count == 1 && cursor[0] == '.') ||
            (count == 2 && cursor[0] == '.' && cursor[1] == '.'))
            return false;
        for (size_t i = 0; i < count; ++i) {
            const unsigned char c = (unsigned char)cursor[i];
            if (c < 0x20 || c == 0x7f || c == '\\' || c == ':' ||
                c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
                c == '|')
                return false;
        }
        if (!slash)
            break;
        cursor = slash + 1;
        if (!*cursor)
            return false;
    }
    return true;
}

/* Return 0 when the directory is ordinary, 1 when it is a validated
 * junction and -1 for hostile/corrupt metadata or a revoked grant. */
static int junction_read(int directory, int root, struct nt_junction_record *record)
{
    const ssize_t count = read_guest_xattr(directory, WIN_JUNCTION_XATTR,
                                           record, sizeof(*record));
    if (count < 0 && (errno == ENODATA || errno == ENOTSUP))
        return 0;
    if (count != (ssize_t)sizeof(*record) ||
        memcmp(record->signature, "JNC1", 4) != 0 ||
        !junction_relative_safe(record->target_relative) ||
        strnlen(record->target_dos, sizeof(record->target_dos)) >=
            sizeof(record->target_dos)) {
        errno = EACCES;
        return -1;
    }
    struct stat grant;
    if (fstat(root, &grant) != 0)
        return -1;
    if (record->root_device != (uint64_t)grant.st_dev ||
        record->root_inode != (uint64_t)grant.st_ino) {
        errno = EACCES;
        return -1;
    }
    return 1;
}
