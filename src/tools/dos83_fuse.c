/* SPDX-License-Identifier: MIT */
/* P2.3: FUSE3 pre-operation mediation for POSIX clients of an opt-in view.
 *
 * The Win32 runtime owns the BACKING directory. All POSIX clients MUST use
 * the VIEW, with the backing path made read-only/inaccessible to them by a
 * separate mount namespace or service-UID permission boundary. Otherwise no
 * global short-name coherence guarantee is possible.
 *
 * No callbacks dereference untrusted host symlinks or cross lower mounts.
 * FUSE metadata is forwarded from the granted root; all mutations that
 * affect entry names use the same Win32 broker as the PE runtime.
 */
#define _GNU_SOURCE
#define FUSE_USE_VERSION 35
#include "compat.h"
#include "winfile.h"

#include <fuse3/fuse.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <linux/openat2.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

/* All FUSE callbacks operate in the backing namespace, NOT through VIEW.
 * Otherwise a FUSE request would recurse into itself and deadlock. */
static int lower_root = -1;
static pthread_mutex_t namespace_lock = PTHREAD_MUTEX_INITIALIZER;

/* NT-compatible files opt into the existing CreateFile OFD lease registry.
 * A separate live lease for each FUSE FD preserves independent lifetimes. */
struct view_file_lease {
    int fd, lease;
    struct view_file_lease *next;
};
static struct view_file_lease *view_leases;
static pthread_mutex_t view_lease_lock = PTHREAD_MUTEX_INITIALIZER;

static int view_lease_add(int fd, int lease)
{
    struct view_file_lease *item = malloc(sizeof(*item));
    if (!item)
        return -ENOMEM;
    *item = (struct view_file_lease){.fd = fd, .lease = lease};
    pthread_mutex_lock(&view_lease_lock);
    item->next = view_leases;
    view_leases = item;
    pthread_mutex_unlock(&view_lease_lock);
    return 0;
}

static int view_lease_take(int fd)
{
    int lease = -1;
    pthread_mutex_lock(&view_lease_lock);
    struct view_file_lease **cursor = &view_leases;
    while (*cursor) {
        if ((*cursor)->fd == fd) {
            struct view_file_lease *item = *cursor;
            *cursor = item->next;
            lease = item->lease;
            free(item);
            break;
        }
        cursor = &(*cursor)->next;
    }
    pthread_mutex_unlock(&view_lease_lock);
    return lease;
}

static bool view_lease_active(int fd)
{
    pthread_mutex_lock(&view_lease_lock);
    bool active = false;
    for (struct view_file_lease *item = view_leases; item; item = item->next)
        if (item->fd == fd) {
            active = true;
            break;
        }
    pthread_mutex_unlock(&view_lease_lock);
    return active;
}

/* Marked files observe the same kernel OFD byte-range locks as LockFileEx. */
static int view_range_access(int fd, off_t offset, size_t size, bool writing)
{
    if (!view_lease_active(fd) || size == 0)
        return 0;
    if (offset < 0 || size > (size_t)INT64_MAX ||
        (uint64_t)offset > (uint64_t)INT64_MAX - (uint64_t)size)
        return -EINVAL;
    struct flock request = {
        .l_type = writing ? F_WRLCK : F_RDLCK,
        .l_whence = SEEK_SET, .l_start = offset, .l_len = (off_t)size
    };
    if (fcntl(fd, F_OFD_GETLK, &request) < 0)
        return -errno;
    return request.l_type == F_UNLCK ? 0 : -EAGAIN;
}


static const char *relpath(const char *path)
{
    if (!path || path[0] != '/') {
        errno = EINVAL;
        return NULL;
    }
    /* Clients must not see or alter the authoritative share-lease files. */
    static const char hidden[] = "/:ntnative-file-leases";
    const size_t n = sizeof(hidden) - 1;
    if (!strncmp(path, hidden, n) && (path[n] == '/' || path[n] == '\0')) {
        errno = EACCES;
        return NULL;
    }
    return path[1] ? path + 1 : ".";
}

static int lower_open(const char *path, int flags, mode_t mode)
{
    const char *relative = relpath(path);
    if (!relative)
        return -1;
    const struct open_how how = {
        .flags = (uint64_t)(flags | O_CLOEXEC | O_NOFOLLOW),
        .mode = (uint64_t)((flags & O_CREAT) ? mode : 0),
        .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV
    };
    return (int)syscall(SYS_openat2, lower_root, relative, &how, sizeof(how));
}

/* Obtain a pinned parent; never construct an unrestricted absolute host path
 * for lower rename, link, create or delete operations. */
static int lower_parent(const char *path, char leaf[NAME_MAX + 1])
{
    const char *relative = relpath(path);
    if (!relative)
        return -1;
    if (relative[0] == '.' && relative[1] == '\0') {
        errno = EINVAL;
        return -1;
    }
    const char *slash = strrchr(relative, '/');
    const char *name = slash ? slash + 1 : relative;
    size_t length = strlen(name);
    if (!length || length > NAME_MAX ||
        !strcmp(name, ".") || !strcmp(name, "..")) {
        errno = EINVAL;
        return -1;
    }
    memcpy(leaf, name, length + 1);
    char parent[PATH_MAX];
    if (!slash)
        strcpy(parent, "/");
    else {
        size_t count = (size_t)(slash - relative);
        if (count + 2 > sizeof(parent)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        parent[0] = '/';
        memcpy(parent + 1, relative, count);
        parent[count + 1] = 0;
    }
    return lower_open(parent, O_PATH | O_DIRECTORY, 0);
}

static int guest_wide(const char *path, uint16_t wide[PATH_MAX])
{
    if (!relpath(path)) {
        errno = EINVAL;
        return -EINVAL;
    }
    char dos[PATH_MAX];
    int n = snprintf(dos, sizeof(dos), "C:%s", path);
    if (n <= 0 || n >= (int)sizeof(dos)) {
        errno = ENAMETOOLONG;
        return -ENAMETOOLONG;
    }
    if (!nt_MultiByteToWideChar(65001, 8, dos, -1, wide, PATH_MAX)) {
        errno = EILSEQ;
        return -EILSEQ;
    }
    return 0;
}

static int guest_refresh(const char *path)
{
    uint16_t wide[PATH_MAX];
    int error = guest_wide(path, wide);
    if (error)
        return error;
    /* Entry from Linux create/mkdir/link is known only after the real
     * syscall succeeds. Assign an alias before replying success to FUSE. */
    char dos[PATH_MAX];
    int n = snprintf(dos, sizeof(dos), "C:%s", path);
    if (n <= 0 || n >= (int)sizeof(dos))
        return -ENAMETOOLONG;
    if (!nt_runtime_refresh_dos83_path(dos))
        return -(errno ? errno : EIO);
    return 0;
}

static int guest_error(void)
{
    switch (nt_GetLastError()) {
    case NT_ERROR_FILE_NOT_FOUND: return -ENOENT;
    case NT_ERROR_PATH_NOT_FOUND: return -ENOENT;
    case NT_ERROR_FILE_EXISTS: return -EEXIST;
    case NT_ERROR_ALREADY_EXISTS: return -EEXIST;
    case NT_ERROR_ACCESS_DENIED: return -EACCES;
    case NT_ERROR_SHARING_VIOLATION: return -EBUSY;
    case NT_ERROR_NOT_SAME_DEVICE: return -EXDEV;
    case NT_ERROR_NOT_SUPPORTED: return -ENOTSUP;
    default: return -EIO;
    }
}

static int fs_getattr(const char *path, struct stat *st,
                      struct fuse_file_info *fi)
{
    int fd = fi && fi->fh ? (int)fi->fh : lower_open(path, O_PATH, 0);
    if (fd < 0)
        return -errno;
    int result = fstat(fd, st);
    int error = errno;
    if (!fi || !fi->fh)
        close(fd);
    return result == 0 ? 0 : -error;
}

static int fs_opendir(const char *path, struct fuse_file_info *fi)
{
    int fd = lower_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return -errno;
    fi->fh = (uint64_t)fd;
    return 0;
}

static int fs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi,
                      enum fuse_readdir_flags flags)
{
    (void)flags;
    if (offset < 0)
        return -EINVAL;
    int dupfd = dup((int)fi->fh);
    if (dupfd < 0)
        return -errno;
    DIR *dir = fdopendir(dupfd);
    if (!dir) {
        int error = errno;
        close(dupfd);
        return -error;
    }
    int error = 0;
    if (offset)
        seekdir(dir, (long)offset);
    else
        rewinddir(dir);
    for (;;) {
        errno = 0;
        const struct dirent *entry = readdir(dir);
        if (!entry) {
            error = errno;
            break;
        }
        if (!strcmp(path, "/") &&
            !strcmp(entry->d_name, ":ntnative-file-leases"))
            continue;
        const long cookie = telldir(dir);
        if (cookie < 0) {
            error = errno ? errno : EIO;
            break;
        }
        if (filler(buffer, entry->d_name, NULL, (off_t)cookie,
                   (enum fuse_fill_dir_flags)0))
            break;
    }
    closedir(dir);
    return error ? -error : 0;
}

static int fs_releasedir(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    return close((int)fi->fh) == 0 ? 0 : -errno;
}

static int fs_open(const char *path, struct fuse_file_info *fi)
{
    /* Never truncate before share admission: a Windows handle can reject
     * this write, and a rejected FUSE open must not mutate the file. */
    const bool truncate = (fi->flags & O_TRUNC) != 0;
    int fd = lower_open(path, fi->flags & ~(O_CREAT | O_EXCL | O_TRUNC), 0);
    if (fd < 0)
        return -errno;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        int error = errno ? errno : EACCES;
        close(fd);
        return -error;
    }
    int marked = nt_runtime_nt_file_marker(fd);
    if (marked < 0) {
        int error = errno;
        close(fd);
        return -error;
    }
    int lease = -1;
    if (marked) {
        const int mode = fi->flags & O_ACCMODE;
        const uint32_t access = mode == O_RDONLY ? NT_FS_READ :
            mode == O_WRONLY ? NT_FS_WRITE : NT_FS_READ | NT_FS_WRITE;
        lease = nt_runtime_acquire_linux_nt_lease(fd, access,
                                                  NT_FS_SHARE_ALL);
        if (lease < 0) {
            int error = errno;
            close(fd);
            return -error;
        }
    }
    int error = 0;
    if (truncate && ftruncate(fd, 0) != 0)
        error = -errno;
    if (!error && lease >= 0)
        error = view_lease_add(fd, lease);
    if (error) {
        if (lease >= 0)
            nt_runtime_release_linux_nt_lease(&lease);
        close(fd);
        return error;
    }
    fi->fh = (uint64_t)fd;
    return 0;
}

static int fs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    pthread_mutex_lock(&namespace_lock);
    /* FUSE create receives an inode mode (S_IFREG | permissions).
     * openat2 rejects file-type bits in open_how.mode with EINVAL. */
    int fd = lower_open(path, fi->flags | O_CREAT | O_EXCL, mode & 07777);
    int error = fd < 0 ? -errno : guest_refresh(path);
    if (error && fd >= 0) {
        close(fd);
        char name[NAME_MAX + 1];
        int parent = lower_parent(path, name);
        if (parent >= 0) {
            (void)unlinkat(parent, name, 0);
            close(parent);
        }
    }
    if (!error)
        fi->fh = (uint64_t)fd;
    pthread_mutex_unlock(&namespace_lock);
    return error;
}

static int fs_read(const char *path, char *buffer, size_t size,
                   off_t offset, struct fuse_file_info *fi)
{
    (void)path;
    int denied = view_range_access((int)fi->fh, offset, size, false);
    if (denied)
        return denied;
    ssize_t got = pread((int)fi->fh, buffer, size, offset);
    return got < 0 ? -errno : (int)got;
}

static int fs_write(const char *path, const char *buffer, size_t size,
                    off_t offset, struct fuse_file_info *fi)
{
    (void)path;
    int denied = view_range_access((int)fi->fh, offset, size, true);
    if (denied)
        return denied;
    ssize_t count = pwrite((int)fi->fh, buffer, size, offset);
    return count < 0 ? -errno : (int)count;
}

static int fs_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    int lease = view_lease_take((int)fi->fh);
    int result = close((int)fi->fh);
    int error = errno;
    nt_runtime_release_linux_nt_lease(&lease);
    return result == 0 ? 0 : -error;
}

static int fs_flush(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    (void)fi;
    return 0;
}

static int fs_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
    (void)path;
    int result = datasync ? fdatasync((int)fi->fh) : fsync((int)fi->fh);
    return result == 0 ? 0 : -errno;
}

static int fs_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    bool borrowed = fi != NULL;
    int fd = borrowed ? (int)fi->fh : lower_open(path, O_WRONLY, 0);
    if (fd < 0)
        return -errno;
    int result = ftruncate(fd, size);
    int error = errno;
    if (!borrowed)
        close(fd);
    return result == 0 ? 0 : -error;
}

static int fs_mkdir(const char *path, mode_t mode)
{
    pthread_mutex_lock(&namespace_lock);
    char name[NAME_MAX + 1];
    int parent = lower_parent(path, name);
    int error = parent < 0 ? -errno : 0;
    if (!error && mkdirat(parent, name, mode) != 0)
        error = -errno;
    if (!error) {
        error = guest_refresh(path);
        if (error)
            (void)unlinkat(parent, name, AT_REMOVEDIR);
    }
    if (parent >= 0)
        close(parent);
    pthread_mutex_unlock(&namespace_lock);
    return error;
}

static int fs_mknod(const char *path, mode_t mode, dev_t dev)
{
    (void)dev;
    if (!S_ISREG(mode))
        return -ENOTSUP;
    struct fuse_file_info fi = { .flags = O_WRONLY };
    int result = fs_create(path, mode & 0777, &fi);
    if (!result)
        (void)close((int)fi.fh);
    return result;
}

static int fs_unlink(const char *path)
{
    uint16_t wide[PATH_MAX];
    int error = guest_wide(path, wide);
    if (error)
        return error;
    pthread_mutex_lock(&namespace_lock);
    int result = nt_DeleteFileW(wide) ? 0 : guest_error();
    pthread_mutex_unlock(&namespace_lock);
    return result;
}

static int fs_rmdir(const char *path)
{
    uint16_t wide[PATH_MAX];
    int error = guest_wide(path, wide);
    if (error)
        return error;
    pthread_mutex_lock(&namespace_lock);
    int result = nt_RemoveDirectoryW(wide) ? 0 : guest_error();
    pthread_mutex_unlock(&namespace_lock);
    return result;
}

static int fs_rename(const char *from, const char *to, unsigned int flags)
{
    if (flags & ~(unsigned int)RENAME_NOREPLACE)
        return -ENOTSUP;
    uint16_t old[PATH_MAX], replacement[PATH_MAX];
    int error = guest_wide(from, old);
    if (!error)
        error = guest_wide(to, replacement);
    if (error)
        return error;
    pthread_mutex_lock(&namespace_lock);
    /* POSIX rename replaces existing entries by default; when callers
     * explicitly ask NOREPLACE, preserve that kernel semantic. */
    int result = nt_MoveFileExW(old, replacement,
                                flags & RENAME_NOREPLACE ? 0u : 1u) ? 0 : guest_error();
    pthread_mutex_unlock(&namespace_lock);
    return result;
}

static int fs_link(const char *from, const char *to)
{
    pthread_mutex_lock(&namespace_lock);
    char old_name[NAME_MAX + 1], new_name[NAME_MAX + 1];
    int old_parent = lower_parent(from, old_name);
    int new_parent = lower_parent(to, new_name);
    int error = old_parent < 0 || new_parent < 0 ? -errno : 0;
    if (!error && linkat(old_parent, old_name, new_parent, new_name, 0) != 0)
        error = -errno;
    if (!error) {
        error = guest_refresh(to);
        if (error)
            (void)unlinkat(new_parent, new_name, 0);
    }
    if (old_parent >= 0)
        close(old_parent);
    if (new_parent >= 0)
        close(new_parent);
    pthread_mutex_unlock(&namespace_lock);
    return error;
}

static int fs_statfs(const char *path, struct statvfs *st)
{
    (void)path;
    return fstatvfs(lower_root, st) == 0 ? 0 : -errno;
}

static int fs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    int fd = fi ? (int)fi->fh : lower_open(path, O_RDONLY, 0);
    if (fd < 0)
        return -errno;
    int result = fchmod(fd, mode);
    int error = errno;
    if (!fi)
        close(fd);
    return result == 0 ? 0 : -error;
}

static int fs_utimens(const char *path, const struct timespec times[2],
                      struct fuse_file_info *fi)
{
    int fd = fi ? (int)fi->fh : lower_open(path, O_RDONLY, 0);
    if (fd < 0)
        return -errno;
    int result = futimens(fd, times);
    int error = errno;
    if (!fi)
        close(fd);
    return result == 0 ? 0 : -error;
}

/* Guests cannot alter the runtime's DOS/reparse bookkeeping via arbitrary
 * POSIX setxattr, even if the lower filesystem permits user xattrs. */
static bool reserved_xattr(const char *name)
{
    return strncmp(name, "user.ntnative.", strlen("user.ntnative.")) == 0;
}

static int fs_getxattr(const char *path, const char *name, char *value,
                       size_t size)
{
    if (reserved_xattr(name))
        return -ENODATA;
    int fd = lower_open(path, O_RDONLY, 0);
    if (fd < 0)
        return -errno;
    ssize_t result = fgetxattr(fd, name, value, size);
    int error = errno;
    close(fd);
    return result < 0 ? -error : (int)result;
}

static int fs_setxattr(const char *path, const char *name, const char *value,
                       size_t size, int flags)
{
    if (reserved_xattr(name))
        return -EACCES;
    int fd = lower_open(path, O_RDONLY, 0);
    if (fd < 0)
        return -errno;
    int result = fsetxattr(fd, name, value, size, flags);
    int error = errno;
    close(fd);
    return result == 0 ? 0 : -error;
}

static int fs_removexattr(const char *path, const char *name)
{
    if (reserved_xattr(name))
        return -EACCES;
    int fd = lower_open(path, O_RDONLY, 0);
    if (fd < 0)
        return -errno;
    int result = fremovexattr(fd, name);
    int error = errno;
    close(fd);
    return result == 0 ? 0 : -error;
}

static int fs_listxattr(const char *path, char *buffer, size_t capacity)
{
    int fd = lower_open(path, O_RDONLY, 0);
    if (fd < 0)
        return -errno;
    ssize_t bytes = flistxattr(fd, NULL, 0);
    int error = errno;
    if (bytes <= 0) {
        close(fd);
        return bytes < 0 ? -error : 0;
    }
    char *names = malloc((size_t)bytes);
    if (!names) {
        close(fd);
        return -ENOMEM;
    }
    ssize_t actual = flistxattr(fd, names, (size_t)bytes);
    error = errno;
    close(fd);
    if (actual < 0) {
        free(names);
        return -error;
    }
    size_t needed = 0;
    for (size_t p = 0; p < (size_t)actual; ) {
        size_t n = strnlen(names + p, (size_t)actual - p);
        if (n == (size_t)actual - p) {
            free(names);
            return -EIO;
        }
        if (!reserved_xattr(names + p)) {
            if (capacity && needed + n + 1 <= capacity)
                memcpy(buffer + needed, names + p, n + 1);
            needed += n + 1;
        }
        p += n + 1;
    }
    free(names);
    return capacity && needed > capacity ? -ERANGE : (int)needed;
}

/* The runtime also opens the lower tree directly; do not leave a cached
 * obsolete FUSE directory entry after a Win32 rename performed elsewhere. */
static void *fs_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn;
    cfg->use_ino = 1;
    cfg->attr_timeout = 0.0;
    cfg->entry_timeout = 0.0;
    cfg->negative_timeout = 0.0;
    return NULL;
}

static const struct fuse_operations ops = {
    .init = fs_init,
    .getattr = fs_getattr,
    .opendir = fs_opendir,
    .readdir = fs_readdir,
    .releasedir = fs_releasedir,
    .open = fs_open,
    .create = fs_create,
    .read = fs_read,
    .write = fs_write,
    .release = fs_release,
    .flush = fs_flush,
    .fsync = fs_fsync,
    .truncate = fs_truncate,
    .mkdir = fs_mkdir,
    .mknod = fs_mknod,
    .unlink = fs_unlink,
    .rmdir = fs_rmdir,
    .rename = fs_rename,
    .link = fs_link,
    .statfs = fs_statfs,
    .chmod = fs_chmod,
    .utimens = fs_utimens,
    .getxattr = fs_getxattr,
    .setxattr = fs_setxattr,
    .listxattr = fs_listxattr,
    .removexattr = fs_removexattr
};

/* Deterministic smoke of lower descriptor confinement even on CI runners
 * with /dev/fuse unavailable. This does NOT validate kernel FUSE mounting. */
static int backend_selftest(void)
{
    int outside = lower_open("/../../etc/passwd", O_RDONLY, 0);
    if (outside >= 0) {
        close(outside);
        fprintf(stderr, "FAIL: traversal escaped backing grant\n");
        return 1;
    }
    if (lower_open("not-an-absolute-fuse-path", O_RDONLY, 0) >= 0) {
        fprintf(stderr, "FAIL: accepted malformed FUSE path\n");
        return 1;
    }
    if (!reserved_xattr("user.ntnative.dos83.123") ||
        reserved_xattr("user.mime_type")) {
        fprintf(stderr, "FAIL: metadata xattr not isolated\n");
        return 1;
    }
    /* Exercise the exact FUSE callbacks without a kernel mount. This
     * proves the broker and alias persistence even when CI has no /dev/fuse. */
    struct fuse_file_info fi = { .flags = O_WRONLY };
    const char *oldpath = "/P2FuseOriginalLongName.dat";
    const char *newpath = "/P2FuseRenamedLongName.dat";
    if (fs_create(oldpath, 0600, &fi) != 0) {
        fprintf(stderr, "FAIL: selftest create callback\n");
        return 1;
    }
    if (fs_write(oldpath, "A", 1, 0, &fi) != 1 ||
        fs_release(oldpath, &fi) != 0) {
        fprintf(stderr, "FAIL: selftest write callback\n");
        return 1;
    }
    uint16_t old_alias[PATH_MAX], new_alias[PATH_MAX];
    uint32_t short_count = nt_GetShortPathNameW(
        u"C:\\P2FuseOriginalLongName.dat", old_alias, PATH_MAX);
    if (!short_count || short_count >= PATH_MAX ||
        fs_rename(oldpath, newpath, RENAME_NOREPLACE) != 0) {
        fprintf(stderr, "FAIL: selftest short-name rename callback\n");
        return 1;
    }
    uint32_t next_count = nt_GetShortPathNameW(
        u"C:\\P2FuseRenamedLongName.dat", new_alias, PATH_MAX);
    if (next_count != short_count ||
        memcmp(old_alias, new_alias, ((size_t)short_count + 1) * sizeof(uint16_t)) != 0 ||
        lower_open(oldpath, O_RDONLY, 0) >= 0 ||
        fs_unlink(newpath) != 0) {
        fprintf(stderr, "FAIL: selftest alias persistence/cleanup\n");
        return 1;
    }
    /* Deterministic compatibility matrix, even without /dev/fuse:
     * marked Linux opens must use the exact Win32 OFD registry; failed
     * O_TRUNC opens must not alter bytes. Unmarked files remain POSIX. */
    const char *marked_path = "/NTCompatibleSharedFile.dat";
    struct fuse_file_info new_file = {.flags = O_WRONLY};
    if (fs_create(marked_path, 0600, &new_file) != 0 ||
        fs_write(marked_path, "XYZ", 3, 0, &new_file) != 3 ||
        fs_release(marked_path, &new_file) != 0) {
        fprintf(stderr, "FAIL: NT compatibility fixture create\n");
        return 1;
    }
    int mark_fd = lower_open(marked_path, O_RDONLY, 0);
    if (mark_fd < 0 || fsetxattr(mark_fd, NT_FS_COMPAT_MARKER, "1", 1, 0) != 0) {
        fprintf(stderr, "FAIL: NT compatibility fixture marker\n");
        if (mark_fd >= 0)
            close(mark_fd);
        return 1;
    }
    close(mark_fd);
    if (fs_getxattr(marked_path, NT_FS_COMPAT_MARKER, NULL, 0) != -ENODATA ||
        fs_setxattr(marked_path, NT_FS_COMPAT_MARKER, "0", 1, 0) != -EACCES ||
        lower_open("/:ntnative-file-leases", O_RDONLY, 0) >= 0) {
        fprintf(stderr, "FAIL: trusted NT metadata exposed by FUSE\n");
        return 1;
    }
    const uint32_t nt_both = UINT32_C(0x80000000) | UINT32_C(0x40000000);
    uintptr_t nt_handle = nt_CreateFileW(u"C:\\NTCompatibleSharedFile.dat",
                                          nt_both, NT_FS_READ, NULL, 3, 0, 0);
    if (nt_handle == UINTPTR_MAX) {
        fprintf(stderr, "FAIL: NT fixture handle open\n");
        return 1;
    }
    struct fuse_file_info denied = {.flags = O_WRONLY | O_TRUNC};
    struct fuse_file_info accepted = {.flags = O_RDONLY};
    if (fs_open(marked_path, &denied) != -EBUSY ||
        fs_open(marked_path, &accepted) != 0) {
        fprintf(stderr, "FAIL: Win32 FILE_SHARE rejected wrong FUSE access\n");
        return 1;
    }
    char contents[4] = {0};
    if (fs_read(marked_path, contents, 3, 0, &accepted) != 3 ||
        memcmp(contents, "XYZ", 3) != 0 ||
        !nt_LockFile(nt_handle, 0, 0, 1, 0) ||
        fs_read(marked_path, contents, 1, 0, &accepted) != -EAGAIN ||
        !nt_UnlockFile(nt_handle, 0, 0, 1, 0) ||
        fs_read(marked_path, contents, 1, 0, &accepted) != 1 ||
        fs_release(marked_path, &accepted) != 0 ||
        !nt_CloseHandle(nt_handle)) {
        fprintf(stderr, "FAIL: marked file I/O locking or lifecycle\n");
        return 1;
    }
    struct fuse_file_info linux_writer = {.flags = O_WRONLY};
    if (fs_open(marked_path, &linux_writer) != 0) {
        fprintf(stderr, "FAIL: Linux writer lease not acquired\n");
        return 1;
    }
    nt_handle = nt_CreateFileW(u"C:\\NTCompatibleSharedFile.dat",
                               UINT32_C(0x80000000), NT_FS_READ, NULL, 3, 0, 0);
    if (nt_handle != UINTPTR_MAX) {
        (void)nt_CloseHandle(nt_handle);
        fprintf(stderr, "FAIL: NT open bypassed Linux FUSE writer lease\n");
        return 1;
    }
    if (fs_release(marked_path, &linux_writer) != 0) {
        fprintf(stderr, "FAIL: Linux writer release\n");
        return 1;
    }
    /* A forked Linux writer dies without fs_release: its independent OFD
     * lease must be dropped by the kernel, not by the parent supervisor. */
    pid_t child = fork();
    if (child == 0) {
        struct fuse_file_info crashed = {.flags = O_WRONLY};
        _exit(fs_open(marked_path, &crashed) == 0 ? 0 : 21);
    }
    int child_status = 0;
    if (child < 0 || waitpid(child, &child_status, 0) != child ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        fprintf(stderr, "FAIL: forked Linux writer lease setup\n");
        return 1;
    }
    nt_handle = nt_CreateFileW(u"C:\\NTCompatibleSharedFile.dat",
                               UINT32_C(0x80000000), NT_FS_READ, NULL, 3, 0, 0);
    if (nt_handle == UINTPTR_MAX || !nt_CloseHandle(nt_handle)) {
        fprintf(stderr, "FAIL: crashed Linux writer leaked OFD lease\n");
        return 1;
    }
    if (fs_open(marked_path, &denied) != 0 ||
        fs_release(marked_path, &denied) != 0 ||
        fs_unlink(marked_path) != 0) {
        fprintf(stderr, "FAIL: marked lease release or delete\n");
        return 1;
    }
    printf("PASS NT opt-in share leases, pre-truncate denial, range locks, metadata isolation\n");
    printf("PASS FUSE callbacks, broker rename, alias persistence and confinement (mount not tested)\n");
    return 0;
}

int main(int argc, char **argv)
{
    if ((argc == 3 && !strcmp(argv[1], "--selftest")) ||
        (argc >= 3 && strcmp(argv[1], "--selftest"))) {
        /* Valid invocations: --selftest BACKING or BACKING MOUNT [-f]. */
    } else {
        fprintf(stderr,
                "Usage: %s --selftest BACKING | BACKING MOUNT [-f]\n",
                argv[0]);
        return 2;
    }
    const char *backing = !strcmp(argv[1], "--selftest") ? argv[2] : argv[1];
    lower_root = open(backing, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (lower_root < 0 || !nt_runtime_set_file_root(backing)) {
        perror("nt-dos83-fuse: backing");
        return 2;
    }
    if (!strcmp(argv[1], "--selftest"))
        return backend_selftest();
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Expected BACKING and MOUNT\n");
        return 2;
    }
    /* Refuse an obvious double-access configuration (view under backing).
     * Mandatory write isolation of the backing path is imposed by the
     * external sandbox/service UID, not by this unprivileged daemon. */
    char lower[PATH_MAX], view[PATH_MAX];
    if (!realpath(backing, lower) || !realpath(argv[2], view)) {
        perror("nt-dos83-fuse: mountpoint");
        return 2;
    }
    const size_t n = strlen(lower);
    if (!strcmp(lower, view) ||
        (!strncmp(lower, view, n) && view[n] == '/')) {
        fprintf(stderr, "Refusing a view inside its backing root\n");
        return 2;
    }
    char *fargv[] = { argv[0], view, "-f", "-o",
                      "default_permissions,fsname=nt-dos83,subtype=nt-dos83", NULL };
    return fuse_main(5, fargv, &ops, NULL);
}
