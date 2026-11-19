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
#include "nt_acl_policy.h"
#include "nt_acl_storage.h"

#include <fuse3/fuse.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <linux/openat2.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/xattr.h>
#include <unistd.h>

/* All FUSE callbacks operate in the backing namespace, NOT through VIEW.
 * Otherwise a FUSE request would recurse into itself and deadlock. */
static int lower_root = -1;
/* Private service-only source directory for atomic ACL-protected mkdir. */
static int acl_staging_root = -1;
static pthread_mutex_t namespace_lock = PTHREAD_MUTEX_INITIALIZER;
/* Enable fault injection exclusively for --selftest. Production service
 * mounting never arms these SIGKILL hooks, regardless of environment. */
static bool fs4_staging_selftest;
static void fs4_stage_test_crash(const char *phase)
{
    if (!fs4_staging_selftest)
        return;
    const char *fault = getenv("NT_FS4_FUSE_TEST_CRASH");
    if (fault && strcmp(fault, phase) == 0)
        (void)raise(SIGKILL);
}

/* Service-UID mode: the service owns backing; FUSE permits only the
 * explicitly configured client UID. Kernel-supplied credentials (not paths
 * or caller-provided metadata) are checked at every entry point. */
static bool service_mode;
static bool metadata_selftest; /* only --selftest-metadata, NEVER production */
static bool managed_acl_selftest; /* only --selftest-acl, NEVER production */
static uid_t service_client_uid;
static gid_t service_client_gid;
static bool service_authorized(void)
{
    if (!service_mode || metadata_selftest || managed_acl_selftest)
        return true;
    const struct fuse_context *ctx = fuse_get_context();
    return ctx && (ctx->uid == service_client_uid || ctx->uid == geteuid());
}


/* FUSE uid/gid/pid come from the kernel request context; getgroups()
 * is resolved by libfuse for that request. Never derive groups from an
 * application-controlled xattr, memory block or filename. */
static int view_acl_token(struct nt_acl_peer_token *token)
{
    if (managed_acl_selftest) {
        const int count=getgroups(0,NULL);
        if (count<0 || count>29) return -EACCES;
        gid_t groups[29]={0};
        if (count && getgroups(count,groups)!=count) return -EACCES;
        return nt_acl_peer_token_init(token,geteuid(),getegid(),
                           groups,(size_t)count,NULL)==0 ? 0 : -EACCES;
    }
    const struct fuse_context *ctx=fuse_get_context();
    if (!ctx) return -EACCES;
    const int count=fuse_getgroups(0,NULL);
    if (count<0 || count>29) return -EACCES;
    gid_t groups[29]={0};
    if (count>0 && fuse_getgroups(count,groups)!=count)
        return -EACCES;
    return nt_acl_peer_token_init(token,ctx->uid,ctx->gid,
                                  groups,(size_t)count,NULL)==0 ? 0 : -EACCES;
}
static int view_acl_check(const char *path,uint32_t rights,bool parent)
{
    /* Never authorize a process through a fake FUSE context during the
     * no-mount selftests. Production service mode is the only gate. */
    if (!service_mode || metadata_selftest)
        return 0;
    struct nt_acl_peer_token token={0};
    const int peer=view_acl_token(&token);
    if (peer) return peer;
    int status=nt_acl_policy_check_path(lower_root,path,parent,&token,
                       rights,parent ? &nt_acl_directory_generic_mapping :
                                       &nt_acl_file_generic_mapping);
    if (status==NT_ACL_POLICY_ALLOWED || status==NT_ACL_POLICY_ABSENT)
        return 0;
    return status==NT_ACL_POLICY_DENIED ? -EACCES :
            -(errno ? errno : EACCES);
}
/* When replacing a destination, ENOENT means there is no existing inode
 * whose DACL can be bypassed; every other read error is fail-closed. */
static int view_acl_replace_target(const char *path)
{
    if (!service_mode || metadata_selftest)
        return 0;
    struct nt_acl_peer_token token={0};
    const int peer=view_acl_token(&token);
    if (peer) return peer;
    int state=nt_acl_policy_check_path(lower_root,path,false,&token,
                         NT_ACL_DELETE,&nt_acl_file_generic_mapping);
    if (state==NT_ACL_POLICY_ALLOWED || state==NT_ACL_POLICY_ABSENT)
        return 0;
    if (state==NT_ACL_POLICY_ERROR && errno==ENOENT)
        return 0;
    return -EACCES;
}

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


/* Size changes on a marked file cannot bypass existing NT byte-range locks.
 * l_len=0 queries [0, EOF) and any lock beyond the current end. */
static int view_truncate_lock(int fd, bool marked)
{
    if (!marked)
        return 0;
    struct flock request = {
        .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0
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
    static const char acl_hidden[] = "/:ntnative-acl-stage";
    const size_t n = sizeof(hidden) - 1;
    const size_t a = sizeof(acl_hidden) - 1;
    if ((!strncmp(path, hidden, n) && (path[n] == '/' || path[n] == '\0')) ||
        (!strncmp(path,acl_hidden,a) && (path[a]=='/' || path[a]=='\0'))) {
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

/* Preflight is deliberately before lower_open/mkdirat/linkat/ftruncate,
 * never only an alias refresh after the mutation. */
static int guest_mutation_begin(const char *path)
{
    if (!relpath(path))
        return -(errno ? errno : EINVAL);
    char dos[PATH_MAX];
    int n = snprintf(dos, sizeof(dos), "C:%s", path);
    if (n <= 0 || n >= (int)sizeof(dos))
        return -ENAMETOOLONG;
    int guard = nt_runtime_begin_namespace_mutation(dos);
    if (guard < 0)
        return -(errno ? errno : EUCLEAN);
    return guard;
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
    if (!service_authorized())
        return -EACCES;
    int acl=view_acl_check(path,NT_ACL_READ_ATTR,false);
    if (acl) return acl;
    int fd = fi && fi->fh ? (int)fi->fh : lower_open(path, O_PATH, 0);
    if (fd < 0)
        return -errno;
    int result = fstat(fd, st);
    int error = errno;
    if (result == 0 && service_mode) {
        st->st_uid = service_client_uid;
        st->st_gid = service_client_gid;
    }
    if (!fi || !fi->fh)
        close(fd);
    return result == 0 ? 0 : -error;
}

static int fs_opendir(const char *path, struct fuse_file_info *fi)
{
    if (!service_authorized())
        return -EACCES;
    int acl=view_acl_check(path,NT_ACL_DATA_READ,false);
    if (acl) return acl;
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
    if (!service_authorized())
        return -EACCES;
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
            (!strcmp(entry->d_name, ":ntnative-file-leases") ||
             !strcmp(entry->d_name, ":ntnative-acl-stage")))
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
    if (!service_authorized())
        return -EACCES;
    (void)path;
    return close((int)fi->fh) == 0 ? 0 : -errno;
}

static int fs_open(const char *path, struct fuse_file_info *fi)
{
    if (!service_authorized())
        return -EACCES;
    const int mode=fi->flags&O_ACCMODE;
    uint32_t rights=mode==O_RDONLY ? NT_ACL_DATA_READ :
                    mode==O_WRONLY ? NT_ACL_DATA_WRITE :
                    NT_ACL_DATA_READ|NT_ACL_DATA_WRITE;
    if (fi->flags&O_TRUNC) rights|=NT_ACL_DATA_WRITE;
    int acl=view_acl_check(path,rights,false);
    if (acl) return acl;
    /* Never truncate before share admission: a Windows handle can reject
     * this write, and a rejected FUSE open must not mutate the file. */
    const bool truncate = (fi->flags & O_TRUNC) != 0;
    int guard = truncate ? guest_mutation_begin(path) : -1;
    if (truncate && guard < 0)
        return guard;
    int fd = lower_open(path, fi->flags & ~(O_CREAT | O_EXCL | O_TRUNC), 0);
    if (fd < 0) {
        int error = errno;
        nt_runtime_end_namespace_mutation(guard);
        return -error;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        int error = errno ? errno : EACCES;
        close(fd);
        nt_runtime_end_namespace_mutation(guard);
        return -error;
    }
    int marked = nt_runtime_nt_file_marker(fd);
    if (marked < 0) {
        int error = errno;
        close(fd);
        nt_runtime_end_namespace_mutation(guard);
        return -error;
    }
    /* In dedicated-UID service mode EVERY FUSE file shares the PE OFD
     * registry, including files without the legacy opt-in xattr. Otherwise
     * a POSIX open on an unmarked inode could bypass a PE FILE_SHARE lease. */
    if (service_mode)
        marked = 1;
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
            nt_runtime_end_namespace_mutation(guard);
            return -error;
        }
    }
    int error = truncate ? view_truncate_lock(fd, marked != 0) : 0;
    if (!error && truncate && ftruncate(fd, 0) != 0)
        error = -errno;
    if (!error && lease >= 0)
        error = view_lease_add(fd, lease);
    if (error) {
        if (lease >= 0)
            nt_runtime_release_linux_nt_lease(&lease);
        close(fd);
        nt_runtime_end_namespace_mutation(guard);
        return error;
    }
    /* Marked NT files must bypass the FUSE page cache: otherwise a
     * buffered read can skip fs_read and the Win32 range-lock check. */
    if (marked) {
        fi->direct_io = 1;
        fi->keep_cache = 0;
    }
    fi->fh = (uint64_t)fd;
    nt_runtime_end_namespace_mutation(guard);
    return 0;
}

/* If a parent has a managed DACL, the new regular inode must receive
 * an inherited SD while still unnamed. FUSE may not publish a file with
 * only the parent's admission check and a missing child ACL. */
static int fs4_inherit_unpublished_inode(int parent,int child,
                                         bool child_directory,
                                         uint32_t requested)
{
    if (!service_mode || metadata_selftest)
        return 0;
    uint8_t *source=malloc(NT_ACL_SD_MAX_BYTES);
    if (!source)
        return -ENOMEM;
    size_t size=0;
    int result=nt_acl_storage_read_fd(parent,source,NT_ACL_SD_MAX_BYTES,&size);
    if (result==NT_ACL_STORAGE_ABSENT) {
        free(source);
        return 0;
    }
    if (result!=NT_ACL_STORAGE_PRESENT) {
        int error=errno ? errno : EUCLEAN;
        free(source);
        return -error;
    }
    struct nt_acl_peer_token token={0};
    int authorized=view_acl_token(&token);
    if (authorized) {
        free(source);
        return authorized;
    }
    size_t needed=0;
    const uint8_t *owner=token.token.sid[0];
    const uint8_t *group=token.token.sid[1];
    int state=nt_acl_inherit_descriptor(source,size,child_directory,
                                owner,token.token.length[0],
                                group,token.token.length[1],
                                NULL,0,&needed);
    if (state!=NT_ACL_DENIED || !needed ||
        needed>NT_ACL_SD_MAX_BYTES) {
        free(source);
        return -ENOTSUP;
    }
    uint8_t *child_sd=malloc(needed);
    if (!child_sd) {free(source);return -ENOMEM;}
    state=nt_acl_inherit_descriptor(source,size,child_directory,
                                owner,token.token.length[0],
                                group,token.token.length[1],
                                child_sd,needed,&needed);
    free(source);
    if (state!=NT_ACL_ALLOWED) {
        free(child_sd);
        return -ENOTSUP;
    }
    /* The child is not yet visible. If the inherited DACL does not grant
     * the requested initial open rights, do not publish the inode. */
    uint32_t granted=0;
    state=nt_acl_access_check_mapped(child_sd,needed,&token.token,requested,
                                     &nt_acl_file_generic_mapping,&granted);
    if (state!=NT_ACL_ALLOWED || (granted&requested)!=requested) {
        free(child_sd);
        return -EACCES;
    }
    const bool stored=nt_acl_storage_write_trusted_fd(child,child_sd,needed);
    const int error=errno;
    free(child_sd);
    return stored ? 0 : -(error ? error : EIO);
}

/* Dedicated storage UID uses staged inode publication. */
/* Publish the inode only AFTER it holds a compatible OFD FILE_SHARE
 * lease. A normal O_CREAT makes the inode visible before share admission:
 * a concurrent PE CreateFile can open it during that interval. O_TMPFILE
 * requires backing filesystem support; there is NO unsafe fallback.
 * The WAL guard is owned by the caller through the final alias refresh. */
static int fs_create_staged_service(const char *path, mode_t mode,
                                    struct fuse_file_info *fi)
{
    char name[NAME_MAX + 1];
    int parent = lower_parent(path, name);
    if (parent < 0)
        return -errno;
    const int access_mode = fi->flags & O_ACCMODE;
    if (access_mode != O_WRONLY && access_mode != O_RDWR) {
        close(parent);
        return -EINVAL;
    }
    int fd = openat(parent, ".", O_TMPFILE | O_CLOEXEC | access_mode,
                    mode & 07777);
    if (fd < 0) {
        int error = -errno;
        close(parent);
        return error;
    }
    const uint32_t access = access_mode == O_RDWR ?
                            NT_FS_READ | NT_FS_WRITE : NT_FS_WRITE;
    int lease = nt_runtime_acquire_linux_nt_lease(fd, access, NT_FS_SHARE_ALL);
    int error = lease < 0 ? -errno : 0;
    if (!error) {
        error = view_lease_add(fd, lease);
        if (!error)
            lease = -1; /* lifecycle transferred to FUSE view registry */
    }
    if (!error)
        error=fs4_inherit_unpublished_inode(parent,fd,false,
                        access_mode==O_RDWR ? NT_ACL_DATA_READ|NT_ACL_DATA_WRITE :
                                             NT_ACL_DATA_WRITE);
    if (!error)
        fs4_stage_test_crash("post-lease");
    bool published = false;
    if (!error) {
        if (linkat(fd, "", parent, name, AT_EMPTY_PATH) < 0)
            error = -errno;
        else
            published = true;
    }
    if (published)
        fs4_stage_test_crash("post-link");
    if (!error && fsync(fd) != 0)
        error = -errno;
    if (!error) {
        /* lower_parent returns an O_PATH directory FD: fsync(O_PATH)
         * fails with EBADF. Reopen the pinned directory, not a host path. */
        int directory_sync = openat(parent, ".", O_RDONLY | O_DIRECTORY |
                                   O_NOFOLLOW | O_CLOEXEC);
        if (directory_sync < 0)
            error = -errno;
        else {
            if (fsync(directory_sync) != 0)
                error = -errno;
            close(directory_sync);
        }
    }
    if (!error)
        fs4_stage_test_crash("post-fsync");
    if (!error)
        error = guest_refresh(path);
    if (!error)
        fs4_stage_test_crash("post-alias");
    if (error) {
        if (published) {
            /* Refuse to remove another inode if an external writer changed
             * the name while the alias/WAL transaction was in progress. */
            struct stat expected, current;
            if (fstat(fd, &expected) == 0 &&
                fstatat(parent, name, &current, AT_SYMLINK_NOFOLLOW) == 0 &&
                expected.st_dev == current.st_dev &&
                expected.st_ino == current.st_ino)
                (void)unlinkat(parent, name, 0);
        }
        if (lease >= 0)
            nt_runtime_release_linux_nt_lease(&lease);
        int registry_lease = view_lease_take(fd);
        nt_runtime_release_linux_nt_lease(&registry_lease);
        close(fd);
    } else {
        fi->fh = (uint64_t)fd;
        fi->direct_io = 1;
        fi->keep_cache = 0;
    }
    close(parent);
    return error;
}

static int fs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    if (!service_authorized())
        return -EACCES;
    int acl=view_acl_check(path,NT_ACL_DATA_WRITE,true);
    if (acl) return acl;
    /* Acquire process-local namespace_lock before physical WAL, matching
     * unlink/rmdir/rename. The previous reverse order could ABBA-deadlock
     * two concurrent FUSE callbacks during namespace mutations. */
    pthread_mutex_lock(&namespace_lock);
    int guard = guest_mutation_begin(path);
    if (guard < 0) {
        pthread_mutex_unlock(&namespace_lock);
        return guard;
    }
    if (service_mode) {
        int result = fs_create_staged_service(path, mode, fi);
        pthread_mutex_unlock(&namespace_lock);
        nt_runtime_end_namespace_mutation(guard);
        return result;
    }
    /* FUSE create receives an inode mode (S_IFREG | permissions).
     * openat2 rejects file-type bits in open_how.mode with EINVAL. */
    int fd = lower_open(path, fi->flags | O_CREAT | O_EXCL, mode & 07777);
    int error = fd < 0 ? -errno : guest_refresh(path);
    int lease = -1;
    if (!error && service_mode) {
        const int access_mode = fi->flags & O_ACCMODE;
        const uint32_t access = access_mode == O_RDONLY ? NT_FS_READ :
            access_mode == O_WRONLY ? NT_FS_WRITE : NT_FS_READ | NT_FS_WRITE;
        lease = nt_runtime_acquire_linux_nt_lease(fd, access, NT_FS_SHARE_ALL);
        if (lease < 0) {
            error = -errno;
        } else {
            error = view_lease_add(fd, lease);
            if (!error) {
                fi->direct_io = 1;
                fi->keep_cache = 0;
            }
        }
    }
    if (error && fd >= 0) {
        if (lease >= 0)
            nt_runtime_release_linux_nt_lease(&lease);
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
    nt_runtime_end_namespace_mutation(guard);
    return error;
}

static int fs_read(const char *path, char *buffer, size_t size,
                   off_t offset, struct fuse_file_info *fi)
{
    if (!service_authorized())
        return -EACCES;
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
    if (!service_authorized())
        return -EACCES;
    (void)path;
    int denied = view_range_access((int)fi->fh, offset, size, true);
    if (denied)
        return denied;
    ssize_t count = pwrite((int)fi->fh, buffer, size, offset);
    return count < 0 ? -errno : (int)count;
}

static int fs_release(const char *path, struct fuse_file_info *fi)
{
    if (!service_authorized())
        return -EACCES;
    (void)path;
    int lease = view_lease_take((int)fi->fh);
    int result = close((int)fi->fh);
    int error = errno;
    nt_runtime_release_linux_nt_lease(&lease);
    return result == 0 ? 0 : -error;
}

static int fs_flush(const char *path, struct fuse_file_info *fi)
{
    if (!service_authorized())
        return -EACCES;
    (void)path;
    (void)fi;
    return 0;
}

static int fs_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
    if (!service_authorized())
        return -EACCES;
    (void)path;
    int result = datasync ? fdatasync((int)fi->fh) : fsync((int)fi->fh);
    return result == 0 ? 0 : -errno;
}

static int fs_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    if (!service_authorized())
        return -EACCES;
    /* FUSE fi is an already-authorized open capability; a path-only
     * truncate must obtain WRITE_DATA permission before any effect. */
    if (!fi) {
        int acl=view_acl_check(path,NT_ACL_DATA_WRITE,false);
        if (acl) return acl;
    }
    if (size < 0)
        return -EINVAL;
    int guard = guest_mutation_begin(path);
    if (guard < 0)
        return guard;
    const bool borrowed = fi != NULL;
    int fd = borrowed ? (int)fi->fh : lower_open(path, O_WRONLY, 0);
    if (fd < 0) {
        int cause = errno;
        nt_runtime_end_namespace_mutation(guard);
        return -cause;
    }
    int lease = -1;
    int marked = borrowed ? (view_lease_active(fd) ? 1 : 0) :
                            nt_runtime_nt_file_marker(fd);
    int error = marked < 0 ? -errno : 0;
    if (!error && service_mode)
        marked = 1;
    if (!borrowed && marked > 0) {
        lease = nt_runtime_acquire_linux_nt_lease(fd, NT_FS_WRITE,
                                                  NT_FS_SHARE_ALL);
        if (lease < 0)
            error = -errno;
    }
    if (!error)
        error = view_truncate_lock(fd, marked > 0);
    if (!error && ftruncate(fd, size) != 0)
        error = -errno;
    nt_runtime_release_linux_nt_lease(&lease);
    if (!borrowed)
        close(fd);
    nt_runtime_end_namespace_mutation(guard);
    return error;
}

/* A hidden, service-owned staging directory makes a new directory's
 * inherited SD durable BEFORE its final name is visible. Recovery removes
 * only empty orphaned stages, never follows symlinks or trusts a guest
 * staging path. No public fallback to mkdirat under a managed DACL. */
static int acl_stage_prepare(void)
{
    if (!service_mode)
        return 0;
    if (mkdirat(lower_root,":ntnative-acl-stage",0700)!=0 &&
        errno!=EEXIST)
        return -errno;
    acl_staging_root=openat(lower_root,":ntnative-acl-stage",
                     O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (acl_staging_root<0)
        return -errno;
    struct stat st;
    if (fstat(acl_staging_root,&st)!=0 ||
        !S_ISDIR(st.st_mode) || st.st_uid!=geteuid() ||
        (st.st_mode&077u)!=0)
        return -EACCES;
    DIR *dir=fdopendir(dup(acl_staging_root));
    if (!dir)
        return -errno;
    int err=0;
    for (;;) {
        errno=0;
        const struct dirent *entry=readdir(dir);
        if (!entry) {
            if (errno) err=-errno;
            break;
        }
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))
            continue;
        if (strncmp(entry->d_name,"dir-",4)!=0) {
            err=-EUCLEAN;
            break;
        }
        struct stat item;
        if (fstatat(acl_staging_root,entry->d_name,
                    &item,AT_SYMLINK_NOFOLLOW)!=0 ||
            !S_ISDIR(item.st_mode) || item.st_uid!=geteuid() ||
            (item.st_mode&077u)!=0 ||
            unlinkat(acl_staging_root,entry->d_name,
                     AT_REMOVEDIR)!=0) {
            err=-EUCLEAN; /* Never remove nonempty or foreign entries. */
            break;
        }
    }
    closedir(dir);
    if (!err && fsync(acl_staging_root)!=0)
        err=-errno;
    return err;
}

static int acl_mkdir_staged(const char *path,int parent,const char *name)
{
    if (acl_staging_root<0)
        return -EACCES;
    uint64_t nonce=0;
    char stage_name[32];
    bool stage_created=false;
    for (int i=0;i<8;i++) {
        if (getrandom(&nonce,sizeof(nonce),0)!=(ssize_t)sizeof(nonce))
            return -EIO;
        const int n=snprintf(stage_name,sizeof(stage_name),
                              "dir-%016" PRIx64,nonce);
        if (n<0 || (size_t)n>=sizeof(stage_name))
            return -EINVAL;
        if (mkdirat(acl_staging_root,stage_name,0700)==0) {
            stage_created=true;
            break;
        }
        if (errno!=EEXIST)
            return -errno;
    }
    if (!stage_created)
        return -EEXIST;
    int fd=openat(acl_staging_root,stage_name,
                  O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int err=fd<0 ? -errno : 0;
    if (!err)
        err=fs4_inherit_unpublished_inode(parent,fd,true,
                         NT_ACL_READ_ATTR|NT_ACL_DATA_READ);
    struct stat original={0};
    if (!err && fstat(fd,&original)!=0)
        err=-errno;
    bool visible=false;
    if (!err) {
        if (renameat2(acl_staging_root,stage_name,parent,name,
                      RENAME_NOREPLACE)!=0)
            err=-errno;
        else
            visible=true;
    }
    if (!err) {
        int destination=openat(parent,".",
               O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if (destination<0)
            err=-errno;
        else {
            if (fsync(destination)!=0)
                err=-errno;
            close(destination);
        }
    }
    if (!err && fsync(acl_staging_root)!=0)
        err=-errno;
    if (!err)
        err=guest_refresh(path);
    if (err && visible) {
        struct stat current;
        if (fstatat(parent,name,&current,AT_SYMLINK_NOFOLLOW)==0 &&
            current.st_dev==original.st_dev &&
            current.st_ino==original.st_ino)
            (void)unlinkat(parent,name,AT_REMOVEDIR);
    }
    if (!visible)
        (void)unlinkat(acl_staging_root,stage_name,AT_REMOVEDIR);
    if (fd>=0)
        close(fd);
    return err;
}

static int fs_mkdir(const char *path, mode_t mode)
{
    if (!service_authorized())
        return -EACCES;
    int acl=view_acl_check(path,NT_ACL_DIR_ADD_SUBDIR,true);
    if (acl) return acl;
    /* Acquire process-local namespace_lock before physical WAL, matching
     * unlink/rmdir/rename. The previous reverse order could ABBA-deadlock
     * two concurrent FUSE callbacks during namespace mutations. */
    pthread_mutex_lock(&namespace_lock);
    int guard = guest_mutation_begin(path);
    if (guard < 0) {
        pthread_mutex_unlock(&namespace_lock);
        return guard;
    }
    char name[NAME_MAX + 1];
    int parent = lower_parent(path, name);
    int error = parent < 0 ? -errno : 0;
    bool managed_parent=false;
    if (!error && service_mode && !metadata_selftest) {
        size_t needed=0;
        const int state=nt_acl_storage_read_fd(parent,NULL,0,&needed);
        if (state==NT_ACL_STORAGE_SHORT_BUFFER ||
            state==NT_ACL_STORAGE_PRESENT)
            managed_parent=true;
        else if (state==NT_ACL_STORAGE_ERROR)
            error=-(errno ? errno : EUCLEAN);
    }
    if (!error && managed_parent) {
        /* Physical DAC remains service owner-only (0700). The inherited
         * SD, not chmod, authorizes FUSE/PE clients after publication. */
        error=acl_mkdir_staged(path,parent,name);
    } else if (!error) {
        if (mkdirat(parent,name,mode)!=0)
            error=-errno;
        if (!error) {
            error=guest_refresh(path);
            if (error)
                (void)unlinkat(parent,name,AT_REMOVEDIR);
        }
    }
    if (parent >= 0)
        close(parent);
    pthread_mutex_unlock(&namespace_lock);
    nt_runtime_end_namespace_mutation(guard);
    return error;
}

static int fs_mknod(const char *path, mode_t mode, dev_t dev)
{
    if (!service_authorized())
        return -EACCES;
    (void)dev;
    if (!S_ISREG(mode))
        return -ENOTSUP;
    struct fuse_file_info fi = { .flags = O_WRONLY };
    int result = fs_create(path, mode & 0777, &fi);
    if (!result)
        (void)fs_release(path, &fi);
    return result;
}

static int fs_unlink(const char *path)
{
    if (!service_authorized())
        return -EACCES;
    int acl=view_acl_check(path,NT_ACL_DELETE,false);
    if (acl) return acl;
    acl=view_acl_check(path,NT_ACL_DIR_DELETE_CHILD,true);
    if (acl) return acl;
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
    if (!service_authorized())
        return -EACCES;
    int acl=view_acl_check(path,NT_ACL_DELETE,false);
    if (acl) return acl;
    acl=view_acl_check(path,NT_ACL_DIR_DELETE_CHILD,true);
    if (acl) return acl;
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
    if (!service_authorized())
        return -EACCES;
    int acl=view_acl_check(from,NT_ACL_DELETE,false);
    if (acl) return acl;
    acl=view_acl_check(from,NT_ACL_DIR_DELETE_CHILD,true);
    if (acl) return acl;
    acl=view_acl_check(to,NT_ACL_DATA_WRITE,true);
    if (acl) return acl;
    if (!(flags&RENAME_NOREPLACE)) {
        acl=view_acl_replace_target(to);
        if (acl) return acl;
    }
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
    if (!service_authorized())
        return -EACCES;
    /* POSIX linkat bypasses Win32 FILE_SHARE_DELETE and pending-delete
     * rules. Until CreateHardLink is brokered, do not publish it in the
     * dedicated-UID namespace. Legacy non-service remains unchanged. */
    if (service_mode)
        return -EOPNOTSUPP;
    /* Acquire process-local namespace_lock before physical WAL, matching
     * unlink/rmdir/rename. The previous reverse order could ABBA-deadlock
     * two concurrent FUSE callbacks during namespace mutations. */
    pthread_mutex_lock(&namespace_lock);
    int guard = guest_mutation_begin(to);
    if (guard < 0) {
        pthread_mutex_unlock(&namespace_lock);
        return guard;
    }
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
    nt_runtime_end_namespace_mutation(guard);
    return error;
}

static int fs_statfs(const char *path, struct statvfs *st)
{
    if (!service_authorized())
        return -EACCES;
    (void)path;
    return fstatvfs(lower_root, st) == 0 ? 0 : -errno;
}

static int fs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    if (!service_authorized())
        return -EACCES;
    /* An authorized FUSE client must never chmod the physical backing
     * root from 0700 to a public mode, or change lower DAC at all.
     * Permissions exposed by the FUSE view are intentionally not POSIX-
     * mutable until an isolated permission mapping exists. */
    if (service_mode)
        return -EPERM;
    int fd = fi ? (int)fi->fh : lower_open(path, O_RDONLY, 0);
    if (fd < 0)
        return -errno;
    int result = fchmod(fd, mode);
    int error = errno;
    if (!fi)
        close(fd);
    return result == 0 ? 0 : -error;
}

/* Convert a POSIX timestamp to the 100-ns Win32 FILETIME epoch.
 * UTIME_NOW/UTIME_OMIT are handled by the caller; no unchecked signed
 * multiply or time_t overflow can reach the trusted Win32 backend. */
static int fs4_timespec_filetime(struct timespec value,
                                 struct nt_file_time *time)
{
    if (value.tv_nsec < 0 || value.tv_nsec >= 1000000000L ||
        (int64_t)value.tv_sec < -INT64_C(11644473600) ||
        (int64_t)value.tv_sec > INT64_C(1833029933770))
        return -EINVAL;
    const uint64_t shifted=(uint64_t)((int64_t)value.tv_sec+
                                      INT64_C(11644473600));
    if (shifted>UINT64_MAX/UINT64_C(10000000))
        return -ERANGE;
    const uint64_t ticks=shifted*UINT64_C(10000000)+
                         (uint64_t)value.tv_nsec/100u;
    time->low=(uint32_t)ticks;
    time->high=(uint32_t)(ticks>>32);
    return 0;
}

static int fs_utimens(const char *path, const struct timespec times[2],
                      struct fuse_file_info *fi)
{
    if (!service_authorized())
        return -EACCES;
    int acl=view_acl_check(path,NT_ACL_WRITE_ATTR,false);
    if (acl) return acl;
    if (!service_mode) {
        int fd=fi ? (int)fi->fh : lower_open(path,O_RDONLY,0);
        if (fd<0) return -errno;
        int result=futimens(fd,times);
        int error=errno;
        if (!fi) close(fd);
        return result==0 ? 0 : -error;
    }
    /* A regular file is mutated through the *same* Win32 HANDLE/WAL
     * path as SetFileTime, never raw futimens with service privileges. */
    struct timespec resolved[2];
    struct timespec now={0};
    if (clock_gettime(CLOCK_REALTIME,&now)!=0)
        return -errno;
    for (int i=0;i<2;i++) {
        resolved[i]=times ? times[i] :
                            (struct timespec){.tv_sec=0,.tv_nsec=UTIME_NOW};
        if (resolved[i].tv_nsec==UTIME_NOW)
            resolved[i]=now;
        else if (resolved[i].tv_nsec!=UTIME_OMIT &&
                 (resolved[i].tv_nsec<0 ||
                  resolved[i].tv_nsec>=1000000000L))
            return -EINVAL;
    }
    int fd=lower_open(path,O_PATH,0);
    if (fd<0) return -errno;
    struct stat pinned;
    int err=fstat(fd,&pinned)==0 ? 0 : -errno;
    close(fd);
    if (err) return err;
    if (S_ISREG(pinned.st_mode)) {
        struct nt_file_time access={0},write={0};
        const struct nt_file_time *a=NULL,*w=NULL;
        if (resolved[0].tv_nsec!=UTIME_OMIT) {
            err=fs4_timespec_filetime(resolved[0],&access);
            if (err) return err;
            a=&access;
        }
        if (resolved[1].tv_nsec!=UTIME_OMIT) {
            err=fs4_timespec_filetime(resolved[1],&write);
            if (err) return err;
            w=&write;
        }
        uint16_t dos[PATH_MAX];
        err=guest_wide(path,dos);
        if (err) return err;
        const uintptr_t handle=nt_CreateFileW(
            dos,UINT32_C(0x40000000),NT_FS_SHARE_ALL,NULL,
            3,0,0);
        if (handle==UINTPTR_MAX)
            return guest_error();
        const bool ok=nt_SetFileTime(handle,NULL,a,w)!=0;
        const int result=ok ? 0 : guest_error();
        (void)nt_CloseHandle(handle);
        return result;
    }
    if (!S_ISDIR(pinned.st_mode))
        return -EOPNOTSUPP;
    /* Directories have no FUSE regular-file lease. Serialize with the
     * namespace WAL and fsync the pinned directory, never chmod backing. */
    pthread_mutex_lock(&namespace_lock);
    int guard=guest_mutation_begin(path);
    if (guard<0) {
        pthread_mutex_unlock(&namespace_lock);
        return guard;
    }
    fd=lower_open(path,O_RDONLY|O_DIRECTORY,0);
    err=fd<0 ? -errno : 0;
    if (!err && futimens(fd,resolved)!=0)
        err=-errno;
    if (!err && fsync(fd)!=0)
        err=-errno;
    if (fd>=0) close(fd);
    nt_runtime_end_namespace_mutation(guard);
    pthread_mutex_unlock(&namespace_lock);
    return err;
}

/* Guests cannot alter the runtime's DOS/reparse/security bookkeeping.
 * Only ordinary user.* xattrs may be modified. The service UID must never
 * grant itself ambient authority to mutate trusted NT metadata on behalf
 * of a FUSE caller, even if Linux would permit fsetxattr. */
static bool reserved_xattr(const char *name)
{
    return strncmp(name, "user.ntnative.", strlen("user.ntnative.")) == 0;
}

static bool ordinary_xattr(const char *name)
{
    if (!name || strnlen(name,256)>=256)
        return false;
    return strncmp(name,"user.",5)==0 && name[5]!=0 &&
           !reserved_xattr(name);
}

static int fs4_service_xattr_mutation(const char *path,const char *name,
                                     const char *value,size_t size,int flags,
                                     bool remove)
{
    int acl=view_acl_check(path,NT_ACL_WRITE_EA,false);
    if (acl) return acl;
    if (!ordinary_xattr(name))
        return -EACCES;
    if ((!remove && (size>4096 || (size && !value) ||
                     (flags & ~(XATTR_CREATE|XATTR_REPLACE)) != 0)) ||
        (remove && (size || flags)))
        return -EINVAL;
    pthread_mutex_lock(&namespace_lock);
    int guard=guest_mutation_begin(path);
    if (guard<0) {
        pthread_mutex_unlock(&namespace_lock);
        return guard;
    }
    int fd=lower_open(path,O_RDONLY,0);
    int error=fd<0 ? -errno : 0;
    struct stat st;
    if (!error && fstat(fd,&st)!=0) error=-errno;
    if (!error && !S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))
        error=-EACCES;
    int lease=-1;
    if (!error && S_ISREG(st.st_mode)) {
        lease=nt_runtime_acquire_linux_nt_lease(fd,NT_FS_WRITE,
                                                NT_FS_SHARE_ALL);
        if (lease<0) error=-errno;
    }
    if (!error) {
        int rc=remove ? fremovexattr(fd,name) :
                        fsetxattr(fd,name,value,size,flags);
        if (rc!=0)
            error=-errno;
        else if (fsync(fd)!=0)
            error=-errno; /* Data may have changed: do not fake PASS. */
    }
    if (lease>=0)
        nt_runtime_release_linux_nt_lease(&lease);
    if (fd>=0) close(fd);
    nt_runtime_end_namespace_mutation(guard);
    pthread_mutex_unlock(&namespace_lock);
    return error;
}

static int fs_getxattr(const char *path, const char *name, char *value,
                       size_t size)
{
    if (!service_authorized())
        return -EACCES;
    int acl=view_acl_check(path,NT_ACL_READ_EA,false);
    if (acl) return acl;
    if (reserved_xattr(name) ||
        (service_mode && strncmp(name, "user.", 5) != 0))
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
    if (!service_authorized())
        return -EACCES;
    if (service_mode)
        return fs4_service_xattr_mutation(path,name,value,size,flags,false);
    if (reserved_xattr(name) ||
        (service_mode && strncmp(name, "user.", 5) != 0))
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
    if (!service_authorized())
        return -EACCES;
    if (service_mode)
        return fs4_service_xattr_mutation(path,name,NULL,0,0,true);
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
    if (!service_authorized())
        return -EACCES;
    int acl=view_acl_check(path,NT_ACL_READ_EA,false);
    if (acl) return acl;
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
        if (!reserved_xattr(names + p) &&
            (!service_mode || strncmp(names + p, "user.", 5) == 0)) {
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
/* Deterministic concurrent callback regression: mixed Win32-bridged
 * unlink/rmdir against WAL-staged FUSE create/mkdir. Both must acquire
 * namespace_lock before the global WAL or they can deadlock (ABBA). */
struct fs4_mutator_trial { int worker, error; };
static void *fs4_mutator_trial_run(void *input)
{
    struct fs4_mutator_trial *trial = input;
    for (int i = 0; i < 8; ++i) {
        char path[128];
        int n = snprintf(path, sizeof(path), "/FS4ConcurrentMutation%dItem%d",
                         trial->worker, i);
        if (n <= 0 || (size_t)n >= sizeof(path)) {
            trial->error = EINVAL;
            return NULL;
        }
        if (trial->worker == 0) {
            struct fuse_file_info file = {.flags = O_WRONLY};
            int code = fs_create(path, 0600, &file);
            if (!code)
                code = fs_release(path, &file);
            if (!code)
                code = fs_unlink(path);
            if (code) {
                trial->error = code;
                return NULL;
            }
        } else {
            int code = fs_mkdir(path, 0700);
            if (!code)
                code = fs_rmdir(path);
            if (code) {
                trial->error = code;
                return NULL;
            }
        }
    }
    return NULL;
}

static bool fs4_selftest_lock_order(void)
{
    struct fs4_mutator_trial one={.worker=0}, two={.worker=1};
    pthread_t a, b;
    if (pthread_create(&a,NULL,fs4_mutator_trial_run,&one)!=0)
        return false;
    if (pthread_create(&b,NULL,fs4_mutator_trial_run,&two)!=0) {
        (void)pthread_join(a,NULL);
        return false;
    }
    struct timespec limit;
    if (clock_gettime(CLOCK_REALTIME,&limit)!=0)
        return false;
    limit.tv_sec+=15;
    const int first=pthread_timedjoin_np(a,NULL,&limit);
    const int second=pthread_timedjoin_np(b,NULL,&limit);
    return first==0 && second==0 && !one.error && !two.error;
}

static int backend_selftest(void)
{
    fs4_staging_selftest = true;
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
    if (!accepted.direct_io) {
        fprintf(stderr, "FAIL: marked FUSE handle can use cached I/O\n");
        return 1;
    }
    char contents[4] = {0};
    if (fs_read(marked_path, contents, 3, 0, &accepted) != 3 ||
        memcmp(contents, "XYZ", 3) != 0 ||
        !nt_LockFile(nt_handle, 0, 0, 1, 0) ||
        fs_read(marked_path, contents, 1, 0, &accepted) != -EAGAIN ||
        fs_truncate(marked_path, 2, NULL) != -EBUSY ||
        fs_truncate(marked_path, 2, &accepted) != -EAGAIN ||
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
    /* A corrupt WAL must deny direct FUSE namespace modifications BEFORE
     * the POSIX syscall. No kernel FUSE mount is needed to run this test. */
    if (!fs4_selftest_lock_order()) {
        fprintf(stderr, "FAIL: concurrent FUSE namespace/WAL lock inversion or mutation\n");
        return 1;
    }
    printf("PASS concurrent FUSE create/mkdir versus delete/rmdir locks\n");
    const char *safe_name = "/FUSEWALSource.dat";
    int saved_fd = lower_open(safe_name, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (saved_fd < 0 || write(saved_fd, "SAFE", 4) != 4) {
        if (saved_fd >= 0) close(saved_fd);
        fprintf(stderr, "FAIL: FUSE WAL admission setup\\n");
        return 1;
    }
    close(saved_fd);
    int registry_fd = openat(lower_root, ":ntnative-file-leases",
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (registry_fd < 0) return 1;
    int wal_fd = openat(registry_fd, "dos83-rename-v1.wal",
                        O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    close(registry_fd);
    struct stat wal_st;
    if (wal_fd < 0 || fstat(wal_fd, &wal_st) != 0 || wal_st.st_size != 0 ||
        write(wal_fd, "BROKEN", 6) != 6 || fsync(wal_fd) != 0) {
        if (wal_fd >= 0) close(wal_fd);
        fprintf(stderr, "FAIL: corrupt WAL fixture\\n");
        return 1;
    }
    const char *denied_name = "/FUSEWALForbidden.dat";
    const char *denied_dir = "/FUSEWALForbiddenDir";
    struct fuse_file_info create_req = {.flags = O_WRONLY};
    struct fuse_file_info truncate_req = {.flags = O_WRONLY | O_TRUNC};
    if (fs_create(denied_name, 0600, &create_req) >= 0 ||
        fs_mkdir(denied_dir, 0700) >= 0 ||
        fs_link(safe_name, denied_name) >= 0 ||
        fs_truncate(safe_name, 0, NULL) >= 0 ||
        fs_open(safe_name, &truncate_req) >= 0) {
        fprintf(stderr, "FAIL: FUSE mutation bypasses corrupt WAL\\n");
        close(wal_fd);
        return 1;
    }
    int verify_fd = lower_open(safe_name, O_RDONLY, 0);
    char verify_bytes[4];
    bool preserved = verify_fd >= 0 && read(verify_fd, verify_bytes, 4) == 4 &&
                     memcmp(verify_bytes, "SAFE", 4) == 0;
    if (verify_fd >= 0) close(verify_fd);
    int unexpected_fd = lower_open(denied_name, O_RDONLY, 0);
    int unexpected_dir = lower_open(denied_dir, O_RDONLY | O_DIRECTORY, 0);
    if (unexpected_fd >= 0) close(unexpected_fd);
    if (unexpected_dir >= 0) close(unexpected_dir);
    if (!preserved || unexpected_fd >= 0 || unexpected_dir >= 0 ||
        ftruncate(wal_fd, 0) != 0 || fsync(wal_fd) != 0) {
        fprintf(stderr, "FAIL: corrupt WAL mutation changed backing\\n");
        close(wal_fd);
        return 1;
    }
    close(wal_fd);
    if (unlinkat(lower_root, safe_name + 1, 0) != 0) {
        fprintf(stderr, "FAIL: FUSE WAL test cleanup\\n");
        return 1;
    }
    /* Dedicated-UID create must acquire an OFD lease BEFORE linkat makes
     * the name visible, and may never fall back to unsafe O_CREAT. */
    const char *staged_name = "/FS4StagedLeaseTest.txt";
    struct fuse_file_info staging = {.flags = O_WRONLY};
    int stage_guard = guest_mutation_begin(staged_name);
    if (stage_guard < 0) {
        fprintf(stderr, "FAIL: staged service WAL admission\n");
        return 1;
    }
    int staged = fs_create_staged_service(staged_name, 0600, &staging);
    nt_runtime_end_namespace_mutation(stage_guard);
    if (staged == -EOPNOTSUPP || staged == -ENOTSUP || staged == -EPERM ||
        staged == -EINVAL) {
        /* A filesystem may not support unnamed inodes; admission must
         * refuse instead of publishing without a lease. */
        int check = lower_open(staged_name, O_RDONLY, 0);
        if (check >= 0) {
            close(check);
            fprintf(stderr, "FAIL: unsupported O_TMPFILE published a name\n");
            return 1;
        }
        printf("SKIP staged service inode test: backing denies O_TMPFILE\n");
    } else {
        if (staged || !staging.direct_io || !view_lease_active((int)staging.fh)) {
            fprintf(stderr, "FAIL: staged service inode was not leased\n");
            return 1;
        }
        uintptr_t opposing = nt_CreateFileW(u"C:\\FS4StagedLeaseTest.txt",
                                            UINT32_C(0x80000000), 0,
                                            NULL, 3, 0, 0);
        if (opposing != UINTPTR_MAX) {
            (void)nt_CloseHandle(opposing);
            fprintf(stderr, "FAIL: staged service inode bypassed PE share lease\n");
            return 1;
        }
        if (fs_release(staged_name, &staging) != 0) {
            fprintf(stderr, "FAIL: staged service lease cleanup\n");
            return 1;
        }
        opposing = nt_CreateFileW(u"C:\\FS4StagedLeaseTest.txt",
                                  UINT32_C(0x80000000), NT_FS_SHARE_ALL,
                                  NULL, 3, 0, 0);
        if (opposing == UINTPTR_MAX || !nt_CloseHandle(opposing) ||
            fs_unlink(staged_name) != 0) {
            fprintf(stderr, "FAIL: lease did not release after staged close\n");
            return 1;
        }
        printf("PASS staged FUSE inode is leased before publication\n");
    }
    printf("PASS FUSE fail-closed pre-mutation WAL admission\\n");
    printf("PASS NT opt-in share leases, pre-truncate denial, range locks, metadata isolation\n");
    printf("PASS FUSE callbacks, broker rename, alias persistence and confinement (mount not tested)\n");
    return 0;
}

static int service_metadata_selftest(void)
{
    const char *name="FS4MetadataServiceFixture.dat";
    const char *view="/FS4MetadataServiceFixture.dat";
    const char *attr="user.fs4_metadata_test";
    int fd=openat(lower_root,name,O_CREAT|O_EXCL|O_WRONLY|O_NOFOLLOW|
                  O_CLOEXEC,0600);
    if (fd<0) {
        perror("metadata fixture create");
        return 1;
    }
    const bool created=write(fd,"DATA",4)==4 && fsync(fd)==0;
    close(fd);
    if (!created) {
        (void)unlinkat(lower_root,name,0);
        return 1;
    }
    const struct timespec dates[2]={
        {.tv_sec=1577836800,.tv_nsec=123456700},
        {.tv_sec=1609459200,.tv_nsec=123456700}
    };
    int error=0;
    if (fs_setxattr(view,attr,"X",1,0)!=0 ||
        fs_getxattr(view,attr,NULL,0)!=1) {
        fprintf(stderr,"FAIL metadata user xattr grant\n");
        error=1;
    }
    char observed[2]={0};
    if (!error && (fs_getxattr(view,attr,observed,sizeof(observed))!=1 ||
                   observed[0]!='X' ||
                   fs_setxattr(view,"user.ntnative.secret","X",1,0)!=-EACCES ||
                   fs_removexattr(view,"user.ntnative.secret")!=-EACCES ||
                   fs_setxattr(view,"security.selinux","X",1,0)!=-EACCES ||
                   fs_setxattr(view,attr,"X",4097,0)!=-EINVAL)) {
        fprintf(stderr,"FAIL metadata reserved xattr confinement\n");
        error=1;
    }
    if (!error && (fs_utimens(view,dates,NULL)!=0 ||
                   fs_removexattr(view,attr)!=0 ||
                   fs_getxattr(view,attr,NULL,0)!=-ENODATA ||
                   fs_chmod(view,0777,NULL)!=-EPERM)) {
        fprintf(stderr,"FAIL metadata timestamp/xattr/physical DAC boundary\n");
        error=1;
    }
    fd=lower_open(view,O_RDONLY,0);
    struct stat st;
    if (!error && (fd<0 || fstat(fd,&st)!=0 ||
                   st.st_mtim.tv_sec!=1609459200 ||
                   st.st_mtim.tv_nsec!=123456700 ||
                   (st.st_mode&0777)!=0600)) {
        fprintf(stderr,"FAIL metadata timestamp persisted or DAC modified\n");
        error=1;
    }
    if (fd>=0) close(fd);
    const uintptr_t held=nt_CreateFileW(
        u"C:\\FS4MetadataServiceFixture.dat",UINT32_C(0x80000000),
        NT_FS_READ,NULL,3,0,0);
    if (!error && held==UINTPTR_MAX) {
        fprintf(stderr,"FAIL Win32 metadata lease test open\n");
        error=1;
    }
    if (!error && (fs_setxattr(view,attr,"Y",1,0)!=-EBUSY ||
                   fs_utimens(view,dates,NULL)!=-EBUSY)) {
        fprintf(stderr,"FAIL metadata bypassed Win32 FILE_SHARE_WRITE\n");
        error=1;
    }
    if (held!=UINTPTR_MAX)
        (void)nt_CloseHandle(held);
    if (!error && (fs_setxattr(view,attr,"Z",1,0)!=0 ||
                   fs_removexattr(view,attr)!=0)) {
        fprintf(stderr,"FAIL metadata lease not released\n");
        error=1;
    }
    /* Real on-disk corrupt WAL: all service metadata mutations must
     * refuse before touching timestamps or arbitrary user xattrs.
     * This test deliberately destroys only its disposable fixture WAL. */
    int registry=openat(lower_root,":ntnative-file-leases",
                        O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int wal=registry>=0 ? openat(registry,"dos83-rename-v1.wal",
                                 O_RDWR|O_NOFOLLOW|O_CLOEXEC) : -1;
    if (registry>=0) close(registry);
    struct stat journal;
    if (!error && (wal<0 || fstat(wal,&journal)!=0 ||
                   journal.st_size!=0 || write(wal,"BADWAL",6)!=6 ||
                   fsync(wal)!=0)) {
        fprintf(stderr,"FAIL metadata corrupt-WAL setup\n");
        error=1;
    }
    if (!error && (fs_setxattr(view,attr,"W",1,0)>=0 ||
                   fs_utimens(view,dates,NULL)>=0 ||
                   fs_getxattr(view,attr,NULL,0)!=-ENODATA)) {
        fprintf(stderr,"FAIL metadata mutated through corrupt WAL\n");
        error=1;
    }
    if (wal>=0) {
        if (ftruncate(wal,0)!=0 || fsync(wal)!=0)
            error=1;
        close(wal);
    }
    if (unlinkat(lower_root,name,0)!=0)
        error=1;
    if (!error)
        puts("PASS offline FUSE UID metadata: Win32 FILETIME WAL, xattrs "
             "under OFD leases, reserved metadata denied (mount not tested)");
    return error;
}

/* Explicit non-mounted managed ACL test. Only --selftest-acl can
 * supply an authenticated local UID/GID in place of a kernel FUSE request;
 * service_mode without this test flag always calls fuse_get_context. */
static void fs4_acl_test_put16(uint8_t *p,uint16_t value)
{
    p[0]=(uint8_t)value;p[1]=(uint8_t)(value>>8);
}
static void fs4_acl_test_put32(uint8_t *p,uint32_t value)
{
    for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(value>>(i*8));
}
static void fs4_acl_test_sid(uint8_t *p,uint32_t kind,uint32_t value)
{
    memset(p,0,16);
    p[0]=1;p[1]=2;p[7]=22;
    fs4_acl_test_put32(p+8,kind);
    fs4_acl_test_put32(p+12,value);
}
static int service_acl_selftest(void)
{
    static const uint8_t everyone[12]={1,1,0,0,0,0,0,1,0,0,0,0};
    if (mkdirat(lower_root,"ACLManagedParent",0700)!=0)
        return 1;
    int parent=openat(lower_root,"ACLManagedParent",
                       O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (parent<0)
        return 1;
    uint8_t parent_sd[104]={0};
    parent_sd[0]=1;
    fs4_acl_test_put16(parent_sd+2,UINT16_C(0x8004));
    fs4_acl_test_put32(parent_sd+4,20);
    fs4_acl_test_put32(parent_sd+8,36);
    fs4_acl_test_put32(parent_sd+16,52);
    fs4_acl_test_sid(parent_sd+20,1,(uint32_t)geteuid());
    fs4_acl_test_sid(parent_sd+36,2,(uint32_t)getegid());
    parent_sd[52]=2;
    fs4_acl_test_put16(parent_sd+54,52);
    fs4_acl_test_put16(parent_sd+56,2);
    parent_sd[60]=1; /* Inherited deny DELETE from Everyone */
    parent_sd[61]=3; /* OI | CI */
    fs4_acl_test_put16(parent_sd+62,20);
    fs4_acl_test_put32(parent_sd+64,NT_ACL_DELETE);
    memcpy(parent_sd+68,everyone,12);
    parent_sd[80]=0; /* Inherited allow for authenticated owner */
    parent_sd[81]=3; /* OI | CI */
    fs4_acl_test_put16(parent_sd+82,24);
    fs4_acl_test_put32(parent_sd+84,
        NT_ACL_DATA_READ|NT_ACL_DATA_WRITE|NT_ACL_DATA_APPEND|
        NT_ACL_READ_EA|NT_ACL_WRITE_EA|NT_ACL_READ_ATTR|
        NT_ACL_WRITE_ATTR|NT_ACL_READ_CONTROL|NT_ACL_WRITE_DAC|
        NT_ACL_DIR_DELETE_CHILD);
    fs4_acl_test_sid(parent_sd+88,1,(uint32_t)geteuid());
    if (!nt_acl_storage_write_trusted_fd(parent,parent_sd,sizeof(parent_sd))) {
        close(parent);
        return 1;
    }
    struct fuse_file_info fi={.flags=O_RDWR};
    int create=fs_create("/ACLManagedParent/Child",0600,&fi);
    if (create==-EOPNOTSUPP || create==-ENOTSUP) {
        printf("SKIP offline managed FUSE ACL: backing has no O_TMPFILE\n");
        close(parent);
        return 0;
    }
    if (create!=0) {
        fprintf(stderr,"FAIL managed FUSE O_TMPFILE create: %d\n",create);
        close(parent);
        return 1;
    }
    int failure=0;
    if (fs_write("/ACLManagedParent/Child","ACL",3,0,&fi)!=3 ||
        fs_release("/ACLManagedParent/Child",&fi)!=0)
        failure=1;
    int fd=lower_open("/ACLManagedParent/Child",O_RDONLY,0);
    size_t sd_size=0;
    uint8_t child[256]={0};
    if (!failure && (fd<0 ||
        nt_acl_storage_read_fd(fd,child,sizeof(child),&sd_size)!=
            NT_ACL_STORAGE_PRESENT ||
        sd_size<60 || sd_size>sizeof(child)))
        failure=1;
    struct stat st;
    if (!failure && (fstat(fd,&st)!=0 ||
        (st.st_mode&0777)!=0600))
        failure=1;
    if (fd>=0) close(fd);
    struct fuse_file_info again={.flags=O_RDONLY};
    if (!failure && (fs_open("/ACLManagedParent/Child",&again)!=0 ||
        fs_release("/ACLManagedParent/Child",&again)!=0))
        failure=1;
    if (!failure && (
        fs_unlink("/ACLManagedParent/Child")!=-EACCES ||
        fs_mkdir("/ACLManagedParent/UnsafeDir",0700)!=-ENOTSUP ||
        fs_setxattr("/ACLManagedParent/Child",
                    "user.ntnative.security.sd","X",1,0)!=-EACCES ||
        fs_getxattr("/ACLManagedParent/Child",
                    "user.ntnative.security.sd",NULL,0)!=-ENODATA ||
        fs_chmod("/ACLManagedParent/Child",0777,NULL)!=-EPERM))
        failure=1;
    if (unlinkat(lower_root,"ACLManagedParent/Child",0)!=0 ||
        unlinkat(lower_root,"ACLManagedParent",AT_REMOVEDIR)!=0)
        failure=1;
    close(parent);
    if (failure) {
        fprintf(stderr,"FAIL offline managed FUSE DACL inheritance/denial\n");
        return 1;
    }
    puts("PASS offline FUSE managed DACL inherited before publication, "
         "read/write grant and delete/chmod denied (mount not tested)");
    return 0;
}

int main(int argc, char **argv)
{
    /* Service client identity is explicit; a test invocation cannot
     * accidentally enable allow_other or weaken backing permissions. */
    if (argc == 3 && !strcmp(argv[1], "--selftest-acl")) {
        if (geteuid()==0) {
            fputs("ACL selftest must be non-root\n",stderr);
            return 2;
        }
        managed_acl_selftest=true;
        service_mode=true;
        service_client_uid=geteuid();
        service_client_gid=getegid();
    } else if (argc == 3 && !strcmp(argv[1], "--selftest-metadata")) {
        /* No mount, no credentials bypass in production. The test binary
         * demands an owner-only disposable backing and current UID. */
        if (geteuid()==0) {
            fputs("Metadata selftest must be non-root\n",stderr);
            return 2;
        }
        metadata_selftest=true;
        service_mode=true;
        service_client_uid=geteuid();
        service_client_gid=getegid();
    } else if (argc == 8 && strcmp(argv[1], "--selftest") &&
        !strcmp(argv[3], "-f") &&
        !strcmp(argv[4], "--service-client-uid") &&
        !strcmp(argv[6], "--service-client-gid")) {
        char *uid_end = NULL, *gid_end = NULL;
        errno = 0;
        unsigned long uid = strtoul(argv[5], &uid_end, 10);
        unsigned long gid = strtoul(argv[7], &gid_end, 10);
        if (errno || !uid_end || *uid_end || !gid_end || *gid_end ||
            uid == 0 || gid == 0 || uid > UINT32_MAX || gid > UINT32_MAX ||
            geteuid() == 0 || uid == (unsigned long)geteuid()) {
            fputs("Refusing insecure service client identity\n", stderr);
            return 2;
        }
        service_mode = true;
        service_client_uid = (uid_t)uid;
        service_client_gid = (gid_t)gid;
    } else if (!metadata_selftest && !managed_acl_selftest &&
               !((argc == 3 && !strcmp(argv[1], "--selftest")) ||
                 (argc >= 3 && argc <= 4 && strcmp(argv[1], "--selftest")))) {
        fprintf(stderr,
                "Usage: %s --selftest BACKING | --selftest-metadata BACKING | --selftest-acl BACKING | BACKING MOUNT [-f] | "
                "BACKING MOUNT -f --service-client-uid UID --service-client-gid GID\n",
                argv[0]);
        return 2;
    }
    const char *backing = !strcmp(argv[1], "--selftest") || metadata_selftest ||
                          managed_acl_selftest ? argv[2] : argv[1];
    lower_root = open(backing, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (lower_root < 0 || !nt_runtime_set_file_root(backing)) {
        perror("nt-dos83-fuse: backing");
        return 2;
    }
    if (service_mode) {
        struct stat st;
        if (fstat(lower_root, &st) != 0 || st.st_uid != geteuid() ||
            (st.st_mode & 077u) != 0) {
            fputs("Service backing must be private and owner-only\n", stderr);
            return 2;
        }
        const int staged=acl_stage_prepare();
        if (staged) {
            fprintf(stderr,"Cannot recover private ACL staging: %d\n",staged);
            return 2;
        }
    }
    if (metadata_selftest)
        return service_metadata_selftest();
    if (managed_acl_selftest)
        return service_acl_selftest();
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
                      service_mode ?
                      "default_permissions,allow_other,fsname=nt-dos83,subtype=nt-dos83" :
                      "default_permissions,fsname=nt-dos83,subtype=nt-dos83", NULL };
    return fuse_main(5, fargv, &ops, NULL);
}
