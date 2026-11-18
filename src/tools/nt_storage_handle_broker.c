/* SPDX-License-Identifier: MIT */
/* FileSystem4 v3 trusted, per-connection Win32 HANDLE broker.
 * The dedicated storage UID retains every real inode FD and OFD lease.
 * Clients receive only random per-session capability numbers; no SCM_RIGHTS.
 * Disabled at the service boundary until the PE client can use it.
 */
#define _GNU_SOURCE
#include "compat.h"
#include "winfile.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
       FS4_READ_AT=18, FS4_WRITE_AT=19, FS4_HANDLE_INFO=20 };
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

struct session_handle { uint64_t capability; uintptr_t native; bool search; };
struct session { int socket_fd; const char *view; bool offline_test;
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
        const uintptr_t native = nt_CreateFileW(path, q->access, q->share, NULL,
                                                 q->disposition, q->flags, 0);
        if (native == UINTPTR_MAX) {
            result.error = nt_GetLastError();
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
        free_slot->search=false;result.handle=cap;
        result.error=NT_ERROR_SUCCESS;break;
    }
    case FS4_DELETE:
        if (n != q->size || !to_wide(payload, n, path))
            break;
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
        result.error = nt_MoveFileExW(path, other, q->flags) ?
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
    const time_t deadline = time(NULL) + 120;
    unsigned char payload[FS4_DATA_MAX], output[FS4_DATA_MAX];
    for (int count = 0; count < FS4_REQUEST_MAX && time(NULL) < deadline; ++count) {
        struct pollfd fd = {.fd = s->socket_fd, .events = POLLIN};
        const int ready = poll(&fd, 1, 2000);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0 || !(fd.revents & POLLIN))
            break;
        struct fs4_request request;
        size_t n = 0;
        if (!receive_request(s->socket_fd, &request, payload, &n))
            break;
        /* Cross-process gate is held until the real open/create/delete,
         * share lease and WAL work have ALL completed. FUSE takes the
         * identical gate before the same operations. */
        int admission=nt_runtime_begin_service_admission();
        if (admission<0)
            break; /* connection closes; never retry without mediation */
        const struct fs4_reply reply = dispatch(s, &request, payload, n, output);
        nt_runtime_end_service_admission(admission);
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
    if (!nt_runtime_set_file_root(argv[2]) ||
        !nt_runtime_recover_dos83())
        return 3;
    struct session state = {.socket_fd = sock, .view = test ? NULL : argv[8],
                            .offline_test = test};
    int result = serve(&state);
    close(sock);
    return result;
}
