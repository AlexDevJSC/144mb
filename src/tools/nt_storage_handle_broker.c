/* SPDX-License-Identifier: MIT */
/* FileSystem4 v3 trusted, per-connection Win32 HANDLE broker.
 * The dedicated storage UID retains every real inode FD and OFD lease.
 * Clients receive only random per-session capability numbers; no SCM_RIGHTS.
 * Disabled at the service boundary until the PE client can use it.
 */
#define _GNU_SOURCE
#include "compat.h"
#include "winfile.h"
#include "nt_services.h"
#include "nt_acl_policy.h"
#include "nt_acl_storage.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/limits.h>
#include <linux/openat2.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

enum { FS4_MAGIC = 0x34465348u, FS4_VERSION = 3, FS4_DATA_MAX = 2048,
       FS4_PATH_MAX = 1024, FS4_HANDLE_MAX = 32, FS4_REQUEST_MAX = 256 };
enum { FS4_HELLO = 0, FS4_OPEN = 1, FS4_READ = 2, FS4_WRITE = 3,
       FS4_CLOSE = 4, FS4_DELETE = 5, FS4_RENAME = 6,
       FS4_TRUNCATE = 7, FS4_LOCK = 8, FS4_UNLOCK = 9,
       FS4_SEEK = 10, FS4_SIZE = 11, FS4_ATTR_GET=12,
       FS4_ATTR_SET=13, FS4_FIND_FIRST=14, FS4_FIND_NEXT=15,
       FS4_FIND_CLOSE=16, FS4_DUPLICATE=17,
       FS4_READ_AT=18, FS4_WRITE_AT=19, FS4_HANDLE_INFO=20,
       FS4_NATIVE_QUERY=21, FS4_NATIVE_SET=22, FS4_NATIVE_FLUSH=23,
       FS4_HARDLINK=24, FS4_SECURITY_PATH=25, FS4_NATIVE_SECURITY=26, FS4_TIME_SET=27, FS4_INFO_GET=28, FS4_INFO_SET=29,
       FS4_SECURITY_SET_PATH=30, FS4_SECURITY_SET_HANDLE=31 };
struct fs4_request {
    uint32_t magic;
    uint16_t version, op;
    uint64_t handle;
    int64_t offset;
    uint32_t access, share, disposition, flags, size, reserved;
    uint64_t nonce;
};
struct fs4_reply {
    uint32_t magic;
    uint16_t version, op;
    uint32_t error, length;
    uint64_t handle, value;
};
_Static_assert(sizeof(struct fs4_request) == 56, "FS4 request ABI");
_Static_assert(sizeof(struct fs4_reply) == 32, "FS4 reply ABI");

struct session_handle { uint64_t capability; uintptr_t native; bool search;
                        uint32_t security_grants; };
struct session { int socket_fd; const char *view; bool offline_test;
                 int root_fd; struct nt_acl_peer_token identity;
                 uint64_t nonce; struct session_handle entries[FS4_HANDLE_MAX]; };

static bool entropy(uint64_t *number)
{
    ssize_t n;
    do { n = getrandom(number, sizeof(*number), 0); }
    while (n < 0 && errno == EINTR);
    return n == (ssize_t)sizeof(*number) && *number != 0;
}

static bool send_reply(struct session *s, const struct fs4_reply *reply,
                       const void *data)
{
    struct iovec vec[2] = {{(void *)reply, sizeof(*reply)},
                            {(void *)data, reply->length}};
    struct msghdr message = {.msg_iov = vec, .msg_iovlen = data ? 2u : 1u};
    const ssize_t count = sendmsg(s->socket_fd, &message, MSG_NOSIGNAL);
    return count == (ssize_t)(sizeof(*reply) + reply->length);
}

static bool receive_request(int socket_fd, struct fs4_request *request,
                            unsigned char data[FS4_DATA_MAX], size_t *length)
{
    struct iovec vec[2] = {{request, sizeof(*request)},
                           {data, FS4_DATA_MAX}};
    struct msghdr message = {.msg_iov = vec, .msg_iovlen = 2};
    ssize_t count;
    do { count = recvmsg(socket_fd, &message, 0); }
    while (count < 0 && errno == EINTR);
    if (count <= 0 || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
        count < (ssize_t)sizeof(*request))
        return false;
    *length = (size_t)count - sizeof(*request);
    return true;
}

/* Do not expose a Linux path as a guest grant. Input paths are UTF-8 DOS C:
 * names with no traversal, hidden WAL registry, NUL or alternate streams.
 * Win32's own path resolver performs the final inode confinement. */
static bool valid_path(const unsigned char *path, size_t size)
{
    if (size < 4 || size > FS4_PATH_MAX || memchr(path, '\0', size) != NULL ||
        memcmp(path, "C:/", 3) != 0)
        return false;
    size_t start = 3;
    for (size_t i = 3; i <= size; ++i) {
        if (i < size && path[i] != '/' && path[i] != '\\') {
            if (path[i] == ':' || path[i] < 32 || path[i] == 127)
                return false;
            continue;
        }
        const size_t length = i - start;
        if (!length || (length == 1 && path[start] == '.') ||
            (length == 2 && path[start] == '.' && path[start+1] == '.'))
            return false;
        if (start == 3 && length >= 21 &&
            memcmp(path + start, ":ntnative-file-leases", 21) == 0)
            return false;
        start = i + 1;
    }
    return true;
}

static bool to_wide(const unsigned char *data, size_t length,
                    uint16_t wide[FS4_PATH_MAX + 1])
{
    if (!valid_path(data, length))
        return false;
    char text[FS4_PATH_MAX + 1];
    memcpy(text, data, length);
    text[length] = '\0';
    return nt_MultiByteToWideChar(65001, 8, text, -1,
                                  wide, FS4_PATH_MAX + 1) > 0;
}

static struct session_handle *lookup(struct session *s, uint64_t cap)
{
    if (!cap)
        return NULL;
    for (size_t i = 0; i < FS4_HANDLE_MAX; ++i)
        if (s->entries[i].capability == cap)
            return &s->entries[i];
    return NULL;
}

/* Use only a token created from kernel peer credentials, never from
 * q->access, a PE-supplied SID or a path alias. A record missing on disk
 * retains the legacy service/DAC rules; a bad record never does. */
static uint32_t acl_path_gate(struct session *s,const unsigned char *path,
                              size_t length,uint32_t requested,bool parent)
{
    if (!valid_path(path,length) || length>FS4_PATH_MAX)
        return NT_ERROR_INVALID_PARAMETER;
    char name[FS4_PATH_MAX+1];
    memcpy(name,path,length);
    name[length]=0;
    int state=nt_acl_policy_check_path(
        s->root_fd,name,parent,&s->identity,requested,
        parent ? &nt_acl_directory_generic_mapping :
                 &nt_acl_file_generic_mapping);
    if (state==NT_ACL_POLICY_ALLOWED || state==NT_ACL_POLICY_ABSENT)
        return NT_ERROR_SUCCESS;
    return NT_ERROR_ACCESS_DENIED;
}

static uint32_t acl_open_gate(struct session *s,const unsigned char *path,
                              size_t length,uint32_t desired,
                              uint32_t disposition)
{
    /* Do not drop explicit FILE_READ_DATA / FILE_WRITE_DATA bits:
     * otherwise a low-level NtCreateFile could bypass a managed DACL
     * by avoiding GENERIC_READ/WRITE. */
    uint32_t required=desired & (
        UINT32_C(0xf0000000)|UINT32_C(0x000f0000)|UINT32_C(0x000001ff));
    if (!required)
        required=NT_ACL_READ_ATTR;
    if (!valid_path(path,length))
        return NT_ERROR_INVALID_PARAMETER;
    char name[FS4_PATH_MAX+1];
    memcpy(name,path,length);
    name[length]=0;
    const int access=nt_acl_policy_check_path(
        s->root_fd,name,false,&s->identity,required,
        &nt_acl_file_generic_mapping);
    const int cause=errno;
    if (access==NT_ACL_POLICY_ALLOWED || access==NT_ACL_POLICY_ABSENT) {
        /* Opening a pre-existing file requires the target's DACL, while
         * creation of a new name additionally needs ADD_FILE on parent. */
        return NT_ERROR_SUCCESS;
    }
    if (cause==ENOENT && access==NT_ACL_POLICY_ERROR &&
        (disposition==1 || disposition==2 || disposition==4)) {
        /* CreateFile currently uses O_CREAT, not staged publication.
         * It must not create a child with NO inherited SD underneath
         * a managed ACL parent. The FUSE staged-file path is allowed
         * to inherit before linkat; this PE creation path fails closed. */
        const int parent=nt_acl_policy_check_path(
            s->root_fd,name,true,&s->identity,NT_ACL_DATA_WRITE,
            &nt_acl_directory_generic_mapping);
        if (parent==NT_ACL_POLICY_ALLOWED)
            return NT_ERROR_NOT_SUPPORTED;
        if (parent==NT_ACL_POLICY_ABSENT)
            return NT_ERROR_SUCCESS;
    }
    return NT_ERROR_ACCESS_DENIED;
}

/* End-to-end ACL writes are gated by BOTH authenticated token rights
 * and the physical NT share/delete-pending lease. Caller retains WAL.
 * A denial never creates or replaces a security xattr. */
/* PE managed-parent creation: build inherited SD on O_TMPFILE before
 * publishing any name. The private inode holds its NT OFD lease before
 * linkat, preventing another PE/FUSE open from bypassing share checks.
 * The service WAL protects parent policy admission and the name change. */
static uintptr_t fs4_create_managed_file(struct session *s,
                                         const unsigned char *name,size_t n,
                                         uint32_t desired,uint32_t share,
                                         uint32_t disposition,uint32_t flags,
                                         uint32_t *failure)
{
    *failure=NT_ERROR_ACCESS_DENIED;
    if (!valid_path(name,n) || n>FS4_PATH_MAX ||
        (desired & ~UINT32_C(0xc0070000))!=0 ||
        (desired&UINT32_C(0xc0000000))==0 ||
        (share&~7u)!=0 ||
        (disposition!=1 && disposition!=2 && disposition!=4) ||
        (flags & ~(UINT32_C(0x80)))!=0) {
        *failure=NT_ERROR_NOT_SUPPORTED;
        return UINTPTR_MAX;
    }
    char dos[FS4_PATH_MAX+1];
    memcpy(dos,name,n);dos[n]=0;
    int guard=nt_runtime_begin_namespace_mutation(dos);
    if (guard<0) return UINTPTR_MAX;
    uintptr_t result=UINTPTR_MAX;
    int dir=-1,fd=-1,lease=-1;
    uint8_t *parent_sd=NULL,*child_sd=NULL;
    const char *relative=dos+3;
    const char *slash=strrchr(relative,'/');
    const char *leaf=slash ? slash+1 : relative;
    size_t parent_bytes=slash ? (size_t)(slash-relative) : 0;
    if (!*leaf || parent_bytes>=FS4_PATH_MAX)
        goto done;
    char parent_path[FS4_PATH_MAX+1];
    if (parent_bytes) memcpy(parent_path,relative,parent_bytes);
    parent_path[parent_bytes]=0;
    struct open_how how={
        .flags=O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,
        .resolve=RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV
    };
    dir=(int)syscall(SYS_openat2,s->root_fd,
                      parent_bytes ? parent_path : ".",&how,sizeof(how));
    if (dir<0) goto done;
    const int parent_grant=nt_acl_policy_check_fd(
        dir,&s->identity,NT_ACL_DATA_WRITE,
        &nt_acl_directory_generic_mapping);
    if (parent_grant!=NT_ACL_POLICY_ALLOWED)
        goto done; /* ABSENT is never authorization for this special path */
    struct stat existing;
    if (fstatat(dir,leaf,&existing,AT_SYMLINK_NOFOLLOW)==0) {
        *failure=NT_ERROR_ALREADY_EXISTS;
        goto done;
    }
    if (errno!=ENOENT) goto done;
    parent_sd=malloc(NT_ACL_SD_MAX_BYTES);
    if (!parent_sd) { *failure=NT_ERROR_NOT_ENOUGH_MEMORY;goto done; }
    size_t parent_len=0;
    if (nt_acl_storage_read_fd(dir,parent_sd,NT_ACL_SD_MAX_BYTES,
                               &parent_len)!=NT_ACL_STORAGE_PRESENT)
        goto done;
    size_t required=0;
    int state=nt_acl_inherit_descriptor(
        parent_sd,parent_len,false,
        s->identity.token.sid[0],s->identity.token.length[0],
        s->identity.token.sid[1],s->identity.token.length[1],
        NULL,0,&required);
    if (state!=NT_ACL_DENIED || required<28 ||
        required>NT_ACL_SD_MAX_BYTES) {
        *failure=NT_ERROR_NOT_SUPPORTED;
        goto done;
    }
    child_sd=malloc(required);
    if (!child_sd) { *failure=NT_ERROR_NOT_ENOUGH_MEMORY;goto done; }
    state=nt_acl_inherit_descriptor(
        parent_sd,parent_len,false,
        s->identity.token.sid[0],s->identity.token.length[0],
        s->identity.token.sid[1],s->identity.token.length[1],
        child_sd,required,&required);
    if (state!=NT_ACL_ALLOWED) {
        *failure=NT_ERROR_NOT_SUPPORTED;
        goto done;
    }
    uint32_t granted=0;
    const uint32_t requested=desired & UINT32_C(0xc0010000);
    state=nt_acl_access_check_mapped(
        child_sd,required,&s->identity.token,
        requested & ~NT_ACL_DELETE,
        &nt_acl_file_generic_mapping,&granted);
    if (state!=NT_ACL_ALLOWED) goto done;
    if ((desired&NT_ACL_DELETE)!=0 &&
        nt_acl_access_check_mapped(child_sd,required,
                     &s->identity.token,NT_ACL_DELETE,
                     &nt_acl_file_generic_mapping,&granted)!=NT_ACL_ALLOWED)
        goto done;
    fd=openat(dir,".",O_TMPFILE|O_RDWR|O_CLOEXEC,0600);
    if (fd<0) {
        *failure=NT_ERROR_NOT_SUPPORTED; /* never O_CREAT fallback */
        goto done;
    }
    if (!nt_acl_storage_write_trusted_fd(fd,child_sd,required))
        goto done;
    uint32_t data_access=
        ((desired&UINT32_C(0x80000000)) ? NT_FS_READ : 0u) |
        ((desired&UINT32_C(0x40000000)) ? NT_FS_WRITE : 0u) |
        ((desired&NT_ACL_DELETE) ? NT_FS_DELETE : 0u);
    lease=nt_runtime_acquire_linux_nt_lease(fd,data_access,share);
    if (lease<0) {
        *failure=NT_ERROR_SHARING_VIOLATION;
        goto done;
    }
    if (fsync(fd)!=0) goto done;
    if (linkat(fd,"",dir,leaf,AT_EMPTY_PATH)!=0) {
        *failure=errno==EEXIST ? NT_ERROR_FILE_EXISTS : NT_ERROR_ACCESS_DENIED;
        goto done;
    }
    bool published=true;
    if (fsync(dir)!=0) {
        /* If durable publication cannot be confirmed, no success HANDLE
         * may escape; remove ONLY if same pinned inode still at this name. */
        struct stat current,expected;
        if (fstat(fd,&expected)==0 &&
            fstatat(dir,leaf,&current,AT_SYMLINK_NOFOLLOW)==0 &&
            current.st_dev==expected.st_dev &&
            current.st_ino==expected.st_ino)
            (void)unlinkat(dir,leaf,0);
        goto done;
    }
    if (!nt_runtime_refresh_dos83_path(dos)) {
        struct stat current,expected;
        if (fstat(fd,&expected)==0 &&
            fstatat(dir,leaf,&current,AT_SYMLINK_NOFOLLOW)==0 &&
            current.st_dev==expected.st_dev &&
            current.st_ino==expected.st_ino)
            (void)unlinkat(dir,leaf,0);
        goto done;
    }
    result=nt_runtime_fs4_adopt_preleased_file(
        fd,lease,dos,desired & ~UINT32_C(0x000e0000),share);
    if (result==UINTPTR_MAX && published) {
        struct stat current,expected;
        if (fstat(fd,&expected)==0 &&
            fstatat(dir,leaf,&current,AT_SYMLINK_NOFOLLOW)==0 &&
            current.st_dev==expected.st_dev &&
            current.st_ino==expected.st_ino)
            (void)unlinkat(dir,leaf,0);
    }
    if (result!=UINTPTR_MAX) {
        fd=-1;lease=-1; /* ownership moved into Win32 HANDLE table */
        *failure=NT_ERROR_SUCCESS;
    }
done:
    free(child_sd);
    free(parent_sd);
    if (lease>=0) nt_runtime_release_linux_nt_lease(&lease);
    if (fd>=0) close(fd);
    if (dir>=0) close(dir);
    nt_runtime_end_namespace_mutation(guard);
    return result;
}

static uint32_t fs4_set_pinned_dacl(struct session *s,int fd,
                                   const uint8_t *sd,size_t bytes)
{
    struct stat inode={0};
    if (fstat(fd,&inode)!=0)
        return NT_ERROR_INVALID_HANDLE;
    /* There are no regular-file OFD sharing records for directories.
     * A directory security update is still protected by the namespace
     * WAL held by the caller, never a naked host chmod/fsetxattr. */
    if (!S_ISREG(inode.st_mode) && !S_ISDIR(inode.st_mode))
        return NT_ERROR_NOT_SUPPORTED;
    int lease=-1;
    if (S_ISREG(inode.st_mode)) {
        lease=nt_runtime_acquire_linux_nt_lease(
            fd,NT_FS_WRITE,NT_FS_SHARE_ALL);
        if (lease<0)
            return errno==EAGAIN || errno==EACCES ?
                   NT_ERROR_SHARING_VIOLATION : NT_ERROR_ACCESS_DENIED;
    }
    const int state=nt_acl_policy_set_dacl_fd(fd,&s->identity,sd,bytes);
    const int error=errno;
    nt_runtime_release_linux_nt_lease(&lease);
    if (state==NT_ACL_POLICY_ALLOWED)
        return NT_ERROR_SUCCESS;
    if (state==NT_ACL_POLICY_DENIED)
        return NT_ERROR_ACCESS_DENIED;
    return error==ENOTSUP || error==EINVAL ?
           NT_ERROR_NOT_SUPPORTED : NT_ERROR_ACCESS_DENIED;
}

static uint32_t fs4_set_path_dacl(struct session *s,
                                 const unsigned char *path,size_t path_size,
                                 const uint8_t *sd,size_t bytes)
{
    if (!valid_path(path,path_size) || path_size>FS4_PATH_MAX)
        return NT_ERROR_INVALID_PARAMETER;
    char dos[FS4_PATH_MAX+1];
    memcpy(dos,path,path_size);
    dos[path_size]=0;
    int guard=nt_runtime_begin_namespace_mutation(dos);
    if (guard<0)
        return NT_ERROR_ACCESS_DENIED;
    /* Use the same pinned C: root and openat2 confinement as the policy
     * check; a client cannot follow symlinks or escape through another
     * mount. The actual ACE check is on this exact FD, not a stale path. */
    struct open_how how={
        .flags=O_RDONLY|O_CLOEXEC|O_NOFOLLOW,
        .resolve=RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV
    };
    int fd=(int)syscall(SYS_openat2,s->root_fd,dos+3,&how,sizeof(how));
    uint32_t answer=NT_ERROR_ACCESS_DENIED;
    if (fd>=0) {
        answer=fs4_set_pinned_dacl(s,fd,sd,bytes);
        close(fd);
    } else if (errno==ENOENT) answer=NT_ERROR_FILE_NOT_FOUND;
    nt_runtime_end_namespace_mutation(guard);
    return answer;
}

static uint32_t fs4_set_handle_dacl(struct session *s,
                                   struct session_handle *slot,
                                   const uint8_t *sd,size_t bytes)
{
    if (!slot || slot->search ||
        !(slot->security_grants&NT_ACL_WRITE_DAC))
        return NT_ERROR_ACCESS_DENIED;
    char dos[PATH_MAX];
    int fd=nt_runtime_fs4_pin_security_handle(slot->native,dos,sizeof(dos));
    if (fd<0) return NT_ERROR_INVALID_HANDLE;
    int guard=nt_runtime_begin_namespace_mutation(dos);
    uint32_t answer=NT_ERROR_ACCESS_DENIED;
    if (guard>=0) {
        answer=fs4_set_pinned_dacl(s,fd,sd,bytes);
        nt_runtime_end_namespace_mutation(guard);
    }
    close(fd);
    return answer;
}

static bool view_alive(const char *view)
{
    if (view == NULL || view[0] != '/')
        return false;
    FILE *stream = fopen("/proc/self/mountinfo", "r");
    if (!stream)
        return false;
    char *line = NULL;
    size_t cap = 0;
    bool found = false;
    while (getline(&line, &cap, stream) > 0) {
        char *sep = strstr(line, " - ");
        if (!sep)
            continue;
        *sep = '\0';
        char *save = NULL;
        char *token = strtok_r(line, " ", &save);
        for (int n = 0; n < 4 && token; ++n)
            token = strtok_r(NULL, " ", &save);
        if (token && strcmp(token, view) == 0 &&
            strncmp(sep + 3, "fuse.nt-dos83 ", 14) == 0) {
            found = true;
            break;
        }
    }
    free(line);
    fclose(stream);
    return found;
}

static struct fs4_reply dispatch(struct session *s, const struct fs4_request *q,
                                 const unsigned char *payload, size_t n,
                                 unsigned char output[FS4_DATA_MAX])
{
    struct fs4_reply result = {.magic = FS4_MAGIC, .version = FS4_VERSION,
                               .op = q->op, .error = NT_ERROR_INVALID_PARAMETER};
    if (q->magic != FS4_MAGIC || q->version != FS4_VERSION ||
        q->nonce != s->nonce || q->reserved != 0)
        return result;
    if (!s->offline_test && !view_alive(s->view)) {
        result.error = NT_ERROR_ACCESS_DENIED;
        return result;
    }
    uint16_t path[FS4_PATH_MAX + 1], other[FS4_PATH_MAX + 1];
    struct session_handle *slot = lookup(s, q->handle);
    uint32_t done = 0;
    switch (q->op) {
    case FS4_OPEN: {
        if (n != q->size || q->size > FS4_PATH_MAX ||
            !to_wide(payload, n, path) || q->handle ||
            (q->flags & UINT32_C(0x40000000))) /* no async handles */
            break;
        struct session_handle *free_slot = NULL;
        for (size_t i = 0; i < FS4_HANDLE_MAX; ++i)
            if (!s->entries[i].capability && !free_slot)
                free_slot = &s->entries[i];
        if (!free_slot) {
            result.error = NT_ERROR_NOT_ENOUGH_MEMORY;
            break;
        }
        result.error=acl_open_gate(s,payload,n,q->access,q->disposition);
        const bool inherited_creation=
            result.error==NT_ERROR_NOT_SUPPORTED &&
            (q->disposition==1 || q->disposition==2 || q->disposition==4);
        if (result.error!=NT_ERROR_SUCCESS && !inherited_creation)
            break;
        /* The runtime's real FD opens with DATA rights only. Security
         * rights exist exclusively as broker-side capability grants from
         * acl_open_gate; the guest never gains Linux backing access. */
        const uint32_t secure=q->access & UINT32_C(0x000e0000);
        if (secure & NT_ACL_WRITE_OWNER) {
            result.error=NT_ERROR_NOT_SUPPORTED;
            break; /* no privilege model for owner replacement */
        }
        const uintptr_t native=inherited_creation ?
            fs4_create_managed_file(
                s,payload,n,q->access,q->share,q->disposition,q->flags,
                &result.error) :
            nt_CreateFileW(
                path,q->access & ~UINT32_C(0x000e0000),q->share,
                NULL,q->disposition,q->flags,0);
        if (native == UINTPTR_MAX) {
            if (!inherited_creation) result.error = nt_GetLastError();
            break;
        }
        uint64_t capability = 0;
        bool unique = false;
        for (int trial = 0; trial < 8; ++trial)
            if (entropy(&capability) && !lookup(s, capability)) {
                unique = true;
                break;
            }
        if (!unique) {
            (void)nt_CloseHandle(native);
            result.error = NT_ERROR_GEN_FAILURE;
            break;
        }
        free_slot->native = native;
        free_slot->capability = capability;
        free_slot->search = false;
        free_slot->security_grants = secure |
            ((q->access & (UINT32_C(0x80000000)|
                            UINT32_C(0x40000000)|
                            UINT32_C(0x10000000))) ?
                           NT_ACL_READ_CONTROL : 0u);
        result.handle = capability;
        result.error = NT_ERROR_SUCCESS;
        break;
    }
    case FS4_READ:
    case FS4_WRITE:
    case FS4_READ_AT:
    case FS4_WRITE_AT: {
        const bool writing=q->op==FS4_WRITE || q->op==FS4_WRITE_AT;
        const bool positional=q->op==FS4_READ_AT || q->op==FS4_WRITE_AT;
        if (!slot || slot->search || q->size>FS4_DATA_MAX ||
            n != (writing ? q->size : 0u) ||
            (positional ? (q->offset<0 || q->offset>INT64_MAX-(int64_t)q->size) :
                          q->offset!=-1))
            break;
        /* OVERLAPPED on a synchronous service HANDLE is immediate and
         * positional, preserving its shared file cursor. No guest pointer
         * is transferred into the service process. */
        struct nt_overlapped position={0};
        if (positional) {
            position.offset=(uint32_t)q->offset;
            position.offset_high=(uint32_t)((uint64_t)q->offset>>32);
        }
        const bool ok=writing ?
            nt_WriteFile(slot->native,payload,q->size,&done,
                         positional ? &position : NULL) :
            nt_ReadFile(slot->native,output,q->size,&done,
                        positional ? &position : NULL);
        result.error=ok ? NT_ERROR_SUCCESS : nt_GetLastError();
        if (ok) {
            result.value=done;
            if (!writing)
                result.length=done;
        }
        break;
    }
    case FS4_CLOSE:
        if (!slot || slot->search || n || q->size)
            break;
        result.error = nt_CloseHandle(slot->native) ? NT_ERROR_SUCCESS :
                       nt_GetLastError();
        slot->capability = 0;
        slot->native = 0;
        break;
    case FS4_ATTR_GET: {
        if (n!=q->size || !to_wide(payload,n,path) || q->handle)
            break;
        result.error=acl_path_gate(s,payload,n,NT_ACL_READ_ATTR,false);
        if (result.error!=NT_ERROR_SUCCESS)
            break;
        struct nt_file_attribute_data info={0};
        if (!nt_GetFileAttributesExW(path,0,&info))
            result.error=nt_GetLastError();
        else {
            memcpy(output,&info,sizeof(info));
            result.length=sizeof(info);
            result.error=NT_ERROR_SUCCESS;
        }
        break;
    }
    case FS4_ATTR_SET:
        if (n!=q->size || !to_wide(payload,n,path) || q->handle)
            break;
        result.error=acl_path_gate(s,payload,n,NT_ACL_WRITE_ATTR,false);
        if (result.error!=NT_ERROR_SUCCESS)
            break;
        result.error=nt_SetFileAttributesW(path,q->flags) ?
                     NT_ERROR_SUCCESS : nt_GetLastError();
        break;
    case FS4_FIND_FIRST: {
        if (n!=q->size || !to_wide(payload,n,path) || q->handle)
            break;
        struct session_handle *free_slot=NULL;
        for (size_t i=0;i<FS4_HANDLE_MAX;i++)
            if (!s->entries[i].capability && !free_slot)
                free_slot=&s->entries[i];
        if (!free_slot) { result.error=NT_ERROR_NOT_ENOUGH_MEMORY; break; }
        result.error=acl_path_gate(s,payload,n,NT_ACL_DATA_READ,true);
        if (result.error!=NT_ERROR_SUCCESS)
            break;
        struct nt_find_data_w item={0};
        uintptr_t native=nt_FindFirstFileExW(path,(int32_t)q->access,&item,
                                              (int32_t)q->share,NULL,q->flags);
        if (native==UINTPTR_MAX) { result.error=nt_GetLastError(); break; }
        uint64_t cap=0;bool unique=false;
        for (int attempt=0;attempt<8;attempt++)
            if (entropy(&cap) && !lookup(s,cap)) {unique=true;break;}
        if (!unique) {
            (void)nt_FindClose(native);
            result.error=NT_ERROR_GEN_FAILURE;break;
        }
        free_slot->native=native;free_slot->capability=cap;
        free_slot->search=true;
        memcpy(output,&item,sizeof(item));
        result.length=sizeof(item);result.handle=cap;
        result.error=NT_ERROR_SUCCESS;
        break;
    }
    case FS4_FIND_NEXT: {
        if (!slot || !slot->search || n || q->size)
            break;
        struct nt_find_data_w item={0};
        if (!nt_FindNextFileW(slot->native,&item)) {
            result.error=nt_GetLastError();break;
        }
        memcpy(output,&item,sizeof(item));
        result.length=sizeof(item);result.error=NT_ERROR_SUCCESS;
        break;
    }
    case FS4_FIND_CLOSE:
        if (!slot || !slot->search || n || q->size)
            break;
        result.error=nt_FindClose(slot->native) ?
                     NT_ERROR_SUCCESS : nt_GetLastError();
        slot->capability=0;slot->native=0;slot->search=false;
        break;
    case FS4_HANDLE_INFO: {
        if (!slot || slot->search || n || q->size)
            break;
        struct nt_handle_file_info info={0};
        if (!nt_GetFileInformationByHandle(slot->native,&info))
            result.error=nt_GetLastError();
        else {
            memcpy(output,&info,sizeof(info));
            result.length=sizeof(info);result.error=NT_ERROR_SUCCESS;
        }
        break;
    }
    case FS4_DUPLICATE: {
        if (!slot || slot->search || n || q->size ||
            q->flags!=2u || q->share!=0)
            break;
        /* Same-process noninheritable SAME_ACCESS only. Physical FDs
         * NEVER transfer to the guest or an arbitrary child process. */
        struct session_handle *free_slot=NULL;
        for(size_t i=0;i<FS4_HANDLE_MAX;i++)
            if(!s->entries[i].capability && !free_slot)
                free_slot=&s->entries[i];
        if (!free_slot) {result.error=NT_ERROR_NOT_ENOUGH_MEMORY;break;}
        uintptr_t duplicate=0;
        if(!nt_DuplicateHandle(nt_GetCurrentProcess(),slot->native,
                               nt_GetCurrentProcess(),&duplicate,0,0,2)) {
            result.error=nt_GetLastError();break;
        }
        uint64_t cap=0;bool unique=false;
        for(int attempt=0;attempt<8;attempt++)
            if(entropy(&cap) && !lookup(s,cap)) {unique=true;break;}
        if(!unique) {
            (void)nt_CloseHandle(duplicate);
            result.error=NT_ERROR_GEN_FAILURE;break;
        }
        free_slot->native=duplicate;free_slot->capability=cap;
        free_slot->search=false;
        free_slot->security_grants=slot->security_grants; /* SAME_ACCESS */
        result.handle=cap;
        result.error=NT_ERROR_SUCCESS;break;
    }
    /* A bounded, read-only owner/group/DACL projection. The trusted
     * runtime rejects SACL, extended POSIX ACLs and unknown SID mappings
     * rather than fabricate an NTFS security descriptor. */
    case FS4_SECURITY_PATH: {
        if (q->handle || n != q->size || q->size > FS4_PATH_MAX ||
            q->share || q->flags || q->offset != -1 ||
            q->disposition > FS4_DATA_MAX || !to_wide(payload, n, path))
            break;
        result.error=acl_path_gate(s,payload,n,NT_ACL_READ_CONTROL,false);
        if (result.error!=NT_ERROR_SUCCESS)
            break;
        uint32_t needed=0;
        const bool ok=nt_GetFileSecurityW(path,q->access,output,
                                         q->disposition,&needed)!=0;
        result.error=ok ? NT_ERROR_SUCCESS : nt_GetLastError();
        result.value=needed;
        if (ok && needed<=FS4_DATA_MAX)
            result.length=needed;
        else if (ok)
            result.error=NT_ERROR_GEN_FAILURE;
        break;
    }
    case FS4_NATIVE_SECURITY: {
        if (!slot || slot->search || n || q->share || q->disposition ||
            q->flags || q->offset != -1 || q->size>FS4_DATA_MAX)
            break;
        uint32_t needed=0;
        /* READ_CONTROL belongs to this authenticated capability; the
         * native DATA_READ check must not be substituted for NT security
         * rights, nor may a read-only HANDLE acquire WRITE_DAC. */
        nt_status status=NT_STATUS_ACCESS_DENIED;
        if ((slot->security_grants&NT_ACL_READ_CONTROL)!=0) {
            if (nt_runtime_fs4_query_security_handle(
                    slot->native,q->access,output,q->size,&needed))
                status=NT_STATUS_SUCCESS;
            else if (nt_GetLastError()==NT_ERROR_INSUFFICIENT_BUFFER)
                status=(nt_status)UINT32_C(0xc0000023);
            else if (nt_GetLastError()==NT_ERROR_INVALID_PARAMETER)
                status=NT_STATUS_INVALID_PARAMETER;
            else if (nt_GetLastError()==NT_ERROR_NOT_SUPPORTED)
                status=NT_STATUS_NOT_SUPPORTED;
        }
        result.error=NT_ERROR_SUCCESS; /* NTSTATUS is NOT LastError. */
        result.value=(uint32_t)status;
        result.handle=needed; /* metadata, NOT a HANDLE capability */
        if (status==NT_STATUS_SUCCESS && needed<=FS4_DATA_MAX)
            result.length=needed;
        else if (status==NT_STATUS_SUCCESS)
            result.error=NT_ERROR_GEN_FAILURE;
        break;
    }
    /* DACL_SECURITY_INFORMATION writes only. The descriptor is a
     * validated, immutable binary blob supplied by a guest; owner and
     * group are ALWAYS taken from existing SD or verified peer token. */
    case FS4_SECURITY_SET_PATH: {
        if (q->handle || q->access!=4 || q->flags ||
            q->offset!=-1 || q->share<4 ||
            q->share>FS4_PATH_MAX || q->disposition<28 ||
            q->share>FS4_DATA_MAX ||
            q->disposition>FS4_DATA_MAX-q->share ||
            q->size>FS4_DATA_MAX || q->size!=q->share+q->disposition ||
            n!=q->size || !to_wide(payload,q->share,path))
            break;
        result.error=fs4_set_path_dacl(
            s,payload,q->share,payload+q->share,q->disposition);
        break;
    }
    case FS4_SECURITY_SET_HANDLE: {
        if (!slot || slot->search || q->access!=4 ||
            q->share || q->disposition || q->flags ||
            q->offset!=-1 || q->size<28 ||
            q->size>FS4_DATA_MAX || n!=q->size)
            break;
        result.error=fs4_set_handle_dacl(s,slot,payload,n);
        break;
    }
    /* SetFileTime payload: exactly three native FILETIME pairs, in
     * creation/access/write order, with presence bits in flags. The
     * worker checks the real HANDLE rights and WAL/fsync barriers. */
    case FS4_TIME_SET: {
        if (!slot || slot->search || n!=24 || q->size!=24 ||
            q->access || q->share || q->disposition ||
            q->offset!=-1 || !q->flags || (q->flags & ~7u))
            break;
        struct nt_file_time times[3];
        memcpy(times,payload,sizeof(times));
        const struct nt_file_time *created=(q->flags&1u) ? &times[0] : NULL;
        const struct nt_file_time *accessed=(q->flags&2u) ? &times[1] : NULL;
        const struct nt_file_time *written=(q->flags&4u) ? &times[2] : NULL;
        result.error=nt_SetFileTime(slot->native,created,accessed,written) ?
                     NT_ERROR_SUCCESS : nt_GetLastError();
        break;
    }
    case FS4_INFO_GET: {
        const uint32_t required=q->access==0 ? 40u :
                                 q->access==1 ? 24u :
                                 q->access==9 ? 8u : 0u;
        if (!slot || slot->search || n || !required ||
            q->size!=required || q->share || q->disposition ||
            q->flags || q->offset!=-1)
            break;
        result.error=nt_GetFileInformationByHandleEx(
            slot->native,(int32_t)q->access,output,required) ?
                     NT_ERROR_SUCCESS : nt_GetLastError();
        if (result.error==NT_ERROR_SUCCESS)
            result.length=required;
        break;
    }
    case FS4_INFO_SET: {
        /* FILE_BASIC_INFO (class 0) only; existing local code explicitly
         * refuses unmodelled changed time, attributes and NT ACL writes. */
        if (!slot || slot->search || q->access!=0 || n!=40 ||
            q->size!=40 || q->share || q->disposition ||
            q->flags || q->offset!=-1)
            break;
        result.error=nt_SetFileInformationByHandle(
            slot->native,0,payload,40) ?
                     NT_ERROR_SUCCESS : nt_GetLastError();
        break;
    }
    /* Raw NTSTATUS is returned in value; error remains zero for a valid
     * RPC even when the native operation fails. No guest pointers or FDs. */
    case FS4_NATIVE_QUERY: {
        if (!slot || slot->search || n || q->size > FS4_DATA_MAX ||
            (q->access != 5 && q->access != 14))
            break;
        struct nt_io_status_block io={0};
        const nt_status status=nt_NtQueryInformationFile(slot->native,&io,
                                        output,q->size,q->access);
        result.error=NT_ERROR_SUCCESS;
        result.value=(uint32_t)status;
        if (status==NT_STATUS_SUCCESS && io.information <= FS4_DATA_MAX)
            result.length=(uint32_t)io.information;
        break;
    }
    case FS4_NATIVE_SET: {
        if (!slot || slot->search || n!=q->size ||
            (q->access!=13 && q->access!=14 && q->access!=20) ||
            (q->access==13 ? n!=1 : n!=8))
            break;
        struct nt_io_status_block io={0};
        const nt_status status=nt_NtSetInformationFile(slot->native,&io,
                                        payload,q->size,q->access);
        result.error=NT_ERROR_SUCCESS;
        result.value=(uint32_t)status;
        break;
    }
    case FS4_NATIVE_FLUSH: {
        if (!slot || slot->search || n || q->size)
            break;
        struct nt_io_status_block io={0};
        const nt_status status=nt_NtFlushBuffersFile(slot->native,&io);
        result.error=NT_ERROR_SUCCESS;
        result.value=(uint32_t)status;
        break;
    }
    case FS4_DELETE:
        if (n != q->size || !to_wide(payload, n, path))
            break;
        result.error=acl_path_gate(s,payload,n,NT_ACL_DELETE,false);
        if (result.error!=NT_ERROR_SUCCESS) break;
        result.error=acl_path_gate(s,payload,n,NT_ACL_DIR_DELETE_CHILD,true);
        if (result.error!=NT_ERROR_SUCCESS) break;
        result.error = nt_DeleteFileW(path) ? NT_ERROR_SUCCESS : nt_GetLastError();
        break;
    case FS4_RENAME: {
        if (n != q->size || n < 9 || q->flags & ~1u)
            break;
        const unsigned char *sep = memchr(payload, '\0', n);
        if (!sep)
            break;
        size_t before = (size_t)(sep - payload);
        if (!to_wide(payload, before, path) ||
            !to_wide(sep + 1, n - before - 1, other))
            break;
        result.error=acl_path_gate(s,payload,before,NT_ACL_DELETE,false);
        if (result.error!=NT_ERROR_SUCCESS) break;
        result.error=acl_path_gate(s,payload,before,NT_ACL_DIR_DELETE_CHILD,true);
        if (result.error!=NT_ERROR_SUCCESS) break;
        result.error=acl_path_gate(s,sep+1,n-before-1,NT_ACL_DATA_WRITE,true);
        if (result.error!=NT_ERROR_SUCCESS) break;
        /* Replacement also requires DELETE on the existing target.
         * A missing new target does not imply a permission grant. */
        if (q->flags & 1u) {
            char target[FS4_PATH_MAX+1];
            memcpy(target,sep+1,n-before-1);
            target[n-before-1]=0;
            int t=nt_acl_policy_check_path(s->root_fd,target,false,
                    &s->identity,NT_ACL_DELETE,&nt_acl_file_generic_mapping);
            if (t==NT_ACL_POLICY_DENIED ||
                (t==NT_ACL_POLICY_ERROR && errno!=ENOENT)) {
                result.error=NT_ERROR_ACCESS_DENIED;
                break;
            }
        }
        result.error = nt_MoveFileExW(path, other, q->flags) ?
                       NT_ERROR_SUCCESS : nt_GetLastError();
        break;
    }
    case FS4_HARDLINK: {
        if (n != q->size || n < 9 || q->handle || q->flags ||
            q->access || q->share || q->disposition || q->offset != -1)
            break;
        const unsigned char *sep = memchr(payload, '\0', n);
        if (!sep)
            break;
        const size_t before = (size_t)(sep - payload);
        if (!to_wide(payload, before, path) ||
            !to_wide(sep + 1, n - before - 1, other))
            break;
        result.error=acl_path_gate(s,payload,before,NT_ACL_READ_ATTR,false);
        if (result.error!=NT_ERROR_SUCCESS) break;
        result.error=acl_path_gate(s,sep+1,n-before-1,NT_ACL_DATA_WRITE,true);
        if (result.error!=NT_ERROR_SUCCESS) break;
        /* Existing/source is first in the wire format. The Win32 W API
         * accepts (new_name, existing_name). The physical file root and
         * every WAL/lease operation stay inside this trusted UID worker. */
        result.error = nt_CreateHardLinkW(other, path, NULL) ?
                       NT_ERROR_SUCCESS : nt_GetLastError();
        break;
    }
    case FS4_TRUNCATE:
        if (!slot || slot->search || n || q->size || q->offset < -1)
            break;
        if (q->offset >= 0 &&
            !nt_SetFilePointerEx(slot->native, q->offset, NULL, 0)) {
            result.error = nt_GetLastError();
            break;
        }
        result.error = nt_SetEndOfFile(slot->native) ?
                       NT_ERROR_SUCCESS : nt_GetLastError();
        break;
    case FS4_SEEK: {
        int64_t position = -1;
        if (!slot || slot->search || n || q->size || q->flags > 2)
            break;
        bool success = nt_SetFilePointerEx(slot->native, q->offset,
                                            &position, q->flags) != 0;
        result.error = success ? NT_ERROR_SUCCESS : nt_GetLastError();
        if (success)
            result.value = (uint64_t)position;
        break;
    }
    case FS4_SIZE: {
        int64_t size = -1;
        if (!slot || slot->search || n || q->size)
            break;
        bool success = nt_GetFileSizeEx(slot->native, &size) != 0;
        result.error = success ? NT_ERROR_SUCCESS : nt_GetLastError();
        if (success)
            result.value = (uint64_t)size;
        break;
    }
    case FS4_LOCK:
    case FS4_UNLOCK:
        if (!slot || slot->search || n || q->offset < 0 || q->size == 0)
            break;
        result.error = (q->op == FS4_LOCK ?
                        nt_LockFile(slot->native, (uint32_t)q->offset,
                                    (uint32_t)((uint64_t)q->offset >> 32),
                                    q->size, q->flags) :
                        nt_UnlockFile(slot->native, (uint32_t)q->offset,
                                      (uint32_t)((uint64_t)q->offset >> 32),
                                      q->size, q->flags)) ?
                       NT_ERROR_SUCCESS : nt_GetLastError();
        break;
    default:
        result.error = NT_ERROR_NOT_SUPPORTED;
        break;
    }
    return result;
}

static int serve(struct session *s)
{
    if (!entropy(&s->nonce))
        return 3;
    const struct fs4_reply hello = {.magic = FS4_MAGIC, .version = FS4_VERSION,
                                    .op = FS4_HELLO, .value = s->nonce};
    if (!send_reply(s, &hello, NULL))
        return 3;
    /* The 120-second limit is a SESSION lifetime, not a two-second
     * idle timeout. Use a monotonic deadline: poll(2000) returning zero
     * must not release live handles or consume the request quota. */
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
        return 3;
    /* Only the offline --test worker may shorten its lifetime for a real
     * expiration/lease-recovery regression; production stays at 120 s. */
    const char *fast_expire = s->offline_test ?
        getenv("NT_FS4_TEST_EXPIRE_1S") : NULL;
    deadline.tv_sec += fast_expire && strcmp(fast_expire, "1") == 0 ?
                       1 : 120;
    unsigned char payload[FS4_DATA_MAX], output[FS4_DATA_MAX];
    for (int count = 0; count < FS4_REQUEST_MAX; ) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
            break;
        const int64_t remaining_ns =
            ((int64_t)deadline.tv_sec - (int64_t)now.tv_sec) *
                INT64_C(1000000000) +
            ((int64_t)deadline.tv_nsec - (int64_t)now.tv_nsec);
        if (remaining_ns <= 0)
            break;
        const int timeout_ms = remaining_ns >= INT64_C(2000000000) ?
            2000 : (int)((remaining_ns + 999999) / 1000000);
        struct pollfd fd = {.fd = s->socket_fd, .events = POLLIN};
        const int ready = poll(&fd, 1, timeout_ms);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready == 0)
            continue; /* Idle is not disconnect; keep OFD leases alive. */
        if (ready < 0 || !(fd.revents & POLLIN))
            break;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
            now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec &&
             now.tv_nsec >= deadline.tv_nsec))
            break;
        struct fs4_request request;
        size_t n = 0;
        if (!receive_request(s->socket_fd, &request, payload, &n))
            break;
        ++count; /* A real IPC request, not poll timeout or EINTR. */
        const struct fs4_reply reply = dispatch(s, &request, payload, n, output);
        /* Offline-only SIGKILL injection happens AFTER a successful
         * physical mutation but BEFORE its IPC reply is observable.
         * Never available on a production --view worker. */
        if (s->offline_test && reply.error == NT_ERROR_SUCCESS &&
            (request.op != FS4_NATIVE_SET || reply.value == NT_STATUS_SUCCESS)) {
            const char *fault = getenv("NT_FS4_TEST_CRASH");
            const char *phase = request.op == FS4_OPEN ? "after-open" :
                request.op == FS4_WRITE ? "after-write" :
                request.op == FS4_RENAME ? "after-rename" :
                request.op == FS4_DELETE ? "after-delete" :
                request.op == FS4_ATTR_SET ? "after-attribute" :
                request.op == FS4_NATIVE_SET ? "after-native-set" :
                (request.op == FS4_SECURITY_SET_PATH ||
                 request.op == FS4_SECURITY_SET_HANDLE) ?
                 "after-security-set" : NULL;
            if (fault && phase && strcmp(fault, phase) == 0)
                (void)raise(SIGKILL);
        }
        if (!send_reply(s, &reply, reply.length ? output : NULL))
            break;
        if (request.nonce != s->nonce || request.magic != FS4_MAGIC)
            break;
    }
    for (size_t i = 0; i < FS4_HANDLE_MAX; ++i)
        if (s->entries[i].capability) {
            if (s->entries[i].search)
                (void)nt_FindClose(s->entries[i].native);
            else
                (void)nt_CloseHandle(s->entries[i].native);
            s->entries[i].capability = 0;
        }
    return 0;
}

int main(int argc, char **argv)
{
    if ((argc != 8 && argc != 9) || strcmp(argv[1], "--root") != 0 ||
        strcmp(argv[3], "--fd") != 0 ||
        strcmp(argv[5], "--client-uid") != 0 ||
        (argc == 8 && strcmp(argv[7], "--test") != 0) ||
        (argc == 9 && (strcmp(argv[7], "--view") != 0 || argv[8][0] != '/'))) {
        fputs("Usage: nt-storage-handle-broker --root ROOT --fd N --client-uid UID --view MOUNT | --test\n", stderr);
        return 2;
    }
    char *end = NULL;
    long number = strtol(argv[4], &end, 10);
    if (!end || *end || number < 3 || number > INT_MAX)
        return 3;
    int sock = (int)number, type = 0;
    socklen_t length = sizeof(type);
    if (getsockopt(sock, SOL_SOCKET, SO_TYPE, &type, &length) != 0 ||
        type != SOCK_SEQPACKET)
        return 3;
    struct ucred peer;
    length = sizeof(peer);
    if (getsockopt(sock, SOL_SOCKET, SO_PEERCRED, &peer, &length) != 0)
        return 3;
    end = NULL;
    long expected = strtol(argv[6], &end, 10);
    if (!end || *end || expected < 0 || expected > INT_MAX ||
        peer.uid != (uid_t)expected)
        return 3;
    bool test = argc == 8;
    if (!test && (geteuid() == 0 || geteuid() == peer.uid))
        return 3;
    struct stat root;
    if (lstat(argv[2], &root) != 0 || !S_ISDIR(root.st_mode) ||
        root.st_uid != geteuid() || (root.st_mode & 077u))
        return 3;
    /* SO_PEERGROUPS is a kernel snapshot of the connecting process'
     * supplementary groups. Failing to obtain ALL groups must fail
     * closed: truncation could omit a deny-only or DENY ACE principal.
     * The request payload cannot specify or add any SID or group. */
    gid_t supplementary[29]={0};
    socklen_t groups_bytes=sizeof(supplementary);
    if (getsockopt(sock,SOL_SOCKET,SO_PEERGROUPS,supplementary,
                   &groups_bytes)!=0 ||
        (groups_bytes%sizeof(gid_t))!=0 ||
        groups_bytes>sizeof(supplementary))
        return 3;
    if (!nt_runtime_set_file_root(argv[2]) ||
        !nt_runtime_recover_dos83())
        return 3;
    const int pinned_root=open(argv[2],O_PATH|O_DIRECTORY|O_CLOEXEC);
    if (pinned_root<0)
        return 3;
    struct session state = {.socket_fd = sock, .view = test ? NULL : argv[8],
                            .offline_test = test, .root_fd=pinned_root};
    if (nt_acl_peer_token_init(&state.identity,peer.uid,peer.gid,
                supplementary,groups_bytes/sizeof(gid_t),NULL)!=0) {
        close(pinned_root);
        return 3;
    }
    int result = serve(&state);
    close(pinned_root);
    close(sock);
    return result;
}
