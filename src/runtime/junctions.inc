/* SPDX-License-Identifier: MIT */
/* Filesystem2: directory junction descriptors.
 *
 * A junction is an ordinary existing Linux directory with a user xattr.
 * The Windows guest sees a mount-point reparse tag and its contents are
 * opened from another existing directory under exactly the same granted
 * root. No host mount, sudo, temporary Windows tree or symlink traversal.
 *
 * The authoritative host root is pinned by a descriptor and checked by
 * st_dev/st_ino. Targets are opened with RESOLVE_NO_SYMLINKS|NO_XDEV.
 * A target cannot itself pass through another junction; this prevents
 * recursive cycles and excludes ambiguous traversal from the first ABI.
 */
#define NT_JUNCTION_XATTR "user.ntnative.junction.v1"
#define NT_JUNCTION_MAGIC UINT32_C(0x4a554e43)
struct nt_junction_metadata {
    uint32_t magic, version;
    uint64_t root_dev, root_inode;
    char target[PATH_MAX + 16]; /* canonical guest DOS path, UTF-8 */
};
_Static_assert(sizeof(struct nt_junction_metadata) <= 8192,
               "Junction xattr remains bounded");

static bool nt_junction_abs_guest_name(const char *path)
{
    if (!path || !*path)
        return false;
    if (path[0] == '\\' && path[1] == '\\')
        return true;
    return ((path[0] >= 'A' && path[0] <= 'Z') ||
            (path[0] >= 'a' && path[0] <= 'z')) &&
           path[1] == ':' && (path[2] == '\\' || path[2] == '/');
}

static bool nt_junction_metadata_valid(const struct nt_junction_metadata *meta,
                                       ssize_t size)
{
    return size == (ssize_t)sizeof(*meta) && meta->magic == NT_JUNCTION_MAGIC &&
           meta->version == 1 &&
           memchr(meta->target, 0, sizeof(meta->target)) != NULL &&
           nt_junction_abs_guest_name(meta->target);
}

static int nt_junction_open_physical(int root, const char *relative)
{
    int directory = fcntl(root, F_DUPFD_CLOEXEC, 3);
    if (directory < 0)
        return -1;
    if (strcmp(relative, ".") == 0 || relative[0] == 0)
        return directory;
    const char *cursor = relative;
    for (;;) {
        const char *slash = strchr(cursor, '/');
        const size_t length = slash ? (size_t)(slash - cursor) : strlen(cursor);
        if (length == 0 || length > NAME_MAX) {
            close(directory);
            errno = EINVAL;
            return -1;
        }
        char asked[NAME_MAX + 1], actual[NAME_MAX + 1];
        memcpy(asked, cursor, length);
        asked[length] = 0;
        const int found = resolve_name(directory, asked, actual);
        if (found <= 0) {
            const int error = found < 0 ? errno : ENOENT;
            close(directory);
            errno = error;
            return -1;
        }
        const int next = confined_open_raw(directory, actual,
                                           O_PATH | O_DIRECTORY, 0);
        const int opened_error = errno;
        close(directory);
        if (next < 0) {
            errno = opened_error;
            return -1;
        }
        /* Do not follow or target a junction chain: all underlying
         * directory entries must be physical, regular Linux directories. */
        struct nt_junction_metadata marker;
        const ssize_t n = read_guest_xattr(next, NT_JUNCTION_XATTR,
                                            &marker, sizeof(marker));
        if (n >= 0 || (errno != ENODATA && errno != ENOTSUP)) {
            const int error = n >= 0 ? ELOOP : errno;
            close(next);
            errno = error;
            return -1;
        }
        directory = next;
        if (!slash)
            return directory;
        cursor = slash + 1;
    }
}

/* Consumes directory FD on successful redirection or on error. */
static int nt_junction_follow(int granted_root, int directory)
{
    struct nt_junction_metadata meta;
    const ssize_t n = read_guest_xattr(directory, NT_JUNCTION_XATTR,
                                       &meta, sizeof(meta));
    if (n < 0 && (errno == ENODATA || errno == ENOTSUP))
        return directory;
    if (!nt_junction_metadata_valid(&meta, n)) {
        close(directory);
        errno = EIO;
        return -1;
    }
    struct stat granted;
    if (fstat(granted_root, &granted) != 0 ||
        (uint64_t)granted.st_dev != meta.root_dev ||
        (uint64_t)granted.st_ino != meta.root_inode) {
        close(directory);
        errno = EACCES;
        return -1;
    }
    char normalized[PATH_MAX];
    if (!normalize_path(meta.target, normalized, true)) {
        close(directory);
        errno = EACCES;
        return -1;
    }
    const char *relative = normalized[0] ? normalized : ".";
    int target_root = duplicate_root(&relative);
    struct stat target_grant;
    const bool same_grant = target_root >= 0 &&
                            fstat(target_root, &target_grant) == 0 &&
                            granted.st_dev == target_grant.st_dev &&
                            granted.st_ino == target_grant.st_ino;
    if (target_root >= 0)
        close(target_root);
    if (!same_grant) {
        close(directory);
        errno = EACCES;
        return -1;
    }
    const int target = nt_junction_open_physical(granted_root, relative);
    const int error = errno;
    close(directory);
    errno = error;
    return target;
}
