/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "object_broker.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

enum { OBJECT_LIMIT = 256, CLIENT_LIMIT = 512 };
#define MAX_SECTION_BYTES UINT64_C(0x80000000)
struct broker_object {
    char name[NT_BROKER_NAME_BYTES];
    uint32_t kind, rights, protect, manual, maximum;
    uint64_t size;
    int fd, refs;
};
struct broker_client {
    int fd, object;
    /* Kernel credentials from the initial AF_UNIX connection. Each
     * subsequent message must carry the same real sender identity. */
    pid_t pid;
    uid_t uid;
    gid_t gid;
};
static struct broker_object objects[OBJECT_LIMIT];
static struct broker_client clients[CLIENT_LIMIT];
static volatile sig_atomic_t stopping;
static int global_gate_fd = -1;

static void stop_broker(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}
static bool allowed_directory(const char *path)
{
    char parent[PATH_MAX];
    size_t length = strlen(path);
    if (!length || length >= sizeof(parent))
        return false;
    memcpy(parent, path, length + 1);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent)
        return false;
    *slash = 0;
    struct stat st;
    return lstat(parent, &st) == 0 && S_ISDIR(st.st_mode) &&
           st.st_uid == geteuid() && (st.st_mode & 022u) == 0;
}
static int create_memfd(const char *name, uint64_t size)
{
    if (!size || size > INT64_MAX)
        return -1;
    int fd = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, (off_t)size) != 0 ||
        fcntl(fd, F_ADD_SEALS,
              F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}
static bool make_global_gate(void)
{
    global_gate_fd = create_memfd("nt-broker-gate", 4096);
    if (global_gate_fd < 0)
        return false;
    struct nt_broker_gate *gate = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                      MAP_SHARED, global_gate_fd, 0);
    if (gate == MAP_FAILED)
        return false;
    pthread_mutexattr_t attr;
    bool okay = pthread_mutexattr_init(&attr) == 0;
    if (okay) {
        okay = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == 0 &&
               pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == 0 &&
               pthread_mutex_init(&gate->mutex, &attr) == 0;
        pthread_mutexattr_destroy(&attr);
    }
    munmap(gate, 4096);
    return okay;
}
static bool valid_request(const struct nt_broker_request *req)
{
    const size_t n = strnlen(req->name, NT_BROKER_NAME_BYTES);
    if (req->magic != NT_BROKER_MAGIC || req->version != NT_BROKER_VERSION ||
        (req->action != NT_BROKER_CREATE && req->action != NT_BROKER_OPEN) ||
        req->kind < NT_BROKER_SECTION || req->kind > NT_BROKER_SEMAPHORE ||
        !n || n >= NT_BROKER_NAME_BYTES ||
        (req->flags & ~3u) != 0)
        return false;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)req->name[i] < 0x20 ||
            req->name[i] == '/' || req->name[i] == '\\')
            return false;
    if (req->action == NT_BROKER_OPEN)
        return true;
    if (req->kind == NT_BROKER_SECTION)
        return req->size > 0 && req->size <= MAX_SECTION_BYTES &&
               (req->protect == 2 || req->protect == 4) &&
               req->initial == 0 && req->maximum == 0;
    if (req->size != 0)
        return false;
    if (req->kind == NT_BROKER_EVENT)
        return req->initial <= 1 && req->maximum == 1 &&
               req->protect == 0;
    return req->initial <= req->maximum && req->maximum > 0 &&
           req->maximum <= INT32_MAX && req->protect == 0;
}
static struct broker_object *find_object(const struct nt_broker_request *req)
{
    for (size_t i = 0; i < OBJECT_LIMIT; ++i) {
        struct broker_object *obj = &objects[i];
        if (obj->refs <= 0)
            continue;
        if ((req->flags & 2u) ? strcasecmp(obj->name, req->name) == 0 :
                                strcmp(obj->name, req->name) == 0)
            return obj;
    }
    return NULL;
}
static struct broker_object *reserve_object(void)
{
    for (size_t i = 0; i < OBJECT_LIMIT; ++i)
        if (objects[i].refs == 0)
            return &objects[i];
    return NULL;
}
static bool initialize_object(struct broker_object *obj,
                              const struct nt_broker_request *req)
{
    int fd = create_memfd(req->kind == NT_BROKER_SECTION ?
                          "nt-broker-section" : "nt-broker-sync",
                          req->kind == NT_BROKER_SECTION ? req->size : 4096);
    if (fd < 0)
        return false;
    *obj = (struct broker_object){
        .kind = req->kind, .fd = fd,
        .size = req->kind == NT_BROKER_SECTION ? req->size : 4096,
        .rights = req->rights, .protect = req->protect,
        .manual = req->kind == NT_BROKER_EVENT ? (req->flags & 1u) : 0u,
        .maximum = req->maximum, .refs = 1
    };
    memcpy(obj->name, req->name, NT_BROKER_NAME_BYTES);
    if (req->kind != NT_BROKER_SECTION) {
        struct nt_broker_sync_state *state = mmap(NULL, 4096,
            PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (state == MAP_FAILED) {
            close(fd);
            memset(obj, 0, sizeof(*obj));
            return false;
        }
        *state = (struct nt_broker_sync_state){
            .count = (int32_t)req->initial,
            .maximum = (int32_t)req->maximum,
            .kind = req->kind,
            .manual = obj->manual
        };
        munmap(state, 4096);
    }
    return true;
}
static void release_object(int index)
{
    struct broker_object *obj = &objects[index];
    if (--obj->refs == 0) {
        close(obj->fd);
        memset(obj, 0, sizeof(*obj));
    }
}

static bool reply_to_client(int fd, const struct nt_broker_request *req,
                            uint32_t result, const struct broker_object *obj)
{
    struct nt_broker_reply reply = {
        .magic = NT_BROKER_MAGIC, .version = NT_BROKER_VERSION,
        .result = result, .kind = req->kind
    };
    int descriptors[2] = {0};
    int count = 0;
    if (obj && (result == NT_BROKER_CREATED || result == NT_BROKER_EXISTS)) {
        reply.kind = obj->kind;
        /* The stored object's maximum grant is NOT the rights of every
         * newly opened HANDLE. Echo only this request's checked subset. */
        reply.rights = req->rights;
        reply.protect = obj->protect;
        reply.manual = obj->manual;
        reply.maximum = obj->maximum;
        reply.size = obj->size;
        descriptors[count++] = obj->fd;
        if (obj->kind != NT_BROKER_SECTION)
            descriptors[count++] = global_gate_fd;
    }
    struct iovec iov = {.iov_base = &reply, .iov_len = sizeof(reply)};
    char ancillary[CMSG_SPACE(2 * sizeof(int))] = {0};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
    if (count) {
        msg.msg_control = ancillary;
        msg.msg_controllen = CMSG_SPACE((size_t)count * sizeof(int));
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN((size_t)count * sizeof(int));
        memcpy(CMSG_DATA(cmsg), descriptors, (size_t)count * sizeof(int));
    }
    return sendmsg(fd, &msg, MSG_NOSIGNAL) == (ssize_t)sizeof(reply);
}
static void forget_client(struct broker_client *client)
{
    if (client->object >= 0)
        release_object(client->object);
    if (client->fd >= 0)
        close(client->fd);
    client->fd = -1;
    client->object = -1;
}
/* SCM_RIGHTS may be installed by recvmsg even on a rejected packet.
 * Client requests must never contain host FDs; close every received
 * descriptor rather than leaking resources into the shared broker. */
static bool authenticated_request(struct broker_client *client,
                                  struct nt_broker_request *req)
{
    char ancillary[CMSG_SPACE(sizeof(struct ucred)) +
                   CMSG_SPACE(8 * sizeof(int))] = {0};
    struct iovec iov = {.iov_base = req, .iov_len = sizeof(*req)};
    struct msghdr message = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = ancillary, .msg_controllen = sizeof(ancillary)
    };
    const ssize_t n = recvmsg(client->fd, &message,
                              MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    struct ucred sender = {0};
    unsigned credential_count = 0;
    bool unexpected = false;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message); cmsg;
         cmsg = CMSG_NXTHDR(&message, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET &&
            cmsg->cmsg_type == SCM_CREDENTIALS &&
            cmsg->cmsg_len == CMSG_LEN(sizeof(struct ucred))) {
            memcpy(&sender, CMSG_DATA(cmsg), sizeof(sender));
            ++credential_count;
        } else {
            unexpected = true;
            if (cmsg->cmsg_level == SOL_SOCKET &&
                cmsg->cmsg_type == SCM_RIGHTS &&
                cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
                const size_t count =
                    (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                const unsigned char *fd_data =
                    (const unsigned char *)CMSG_DATA(cmsg);
                for (size_t i = 0; i < count; ++i) {
                    int received_fd;
                    memcpy(&received_fd, fd_data + i * sizeof(int),
                           sizeof(received_fd));
                    if (received_fd >= 0)
                        (void)close(received_fd);
                }
            }
        }
    }
    return n == (ssize_t)sizeof(*req) &&
           !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) &&
           !unexpected && credential_count == 1 &&
           sender.pid == client->pid && sender.uid == client->uid &&
           sender.gid == client->gid;
}

static void accept_request(struct broker_client *client)
{
    struct nt_broker_request req = {0};
    if (!authenticated_request(client, &req) || !valid_request(&req)) {
        forget_client(client);
        return;
    }
    struct broker_object *obj = find_object(&req);
    uint32_t result = NT_BROKER_MISSING;
    if (obj) {
        if (obj->kind != req.kind)
            result = NT_BROKER_TYPE_MISMATCH;
        else if ((req.rights & ~obj->rights) != 0)
            result = NT_BROKER_ACCESS_DENIED;
        else
            result = NT_BROKER_EXISTS;
    } else if (req.action == NT_BROKER_CREATE) {
        obj = reserve_object();
        if (!obj || !initialize_object(obj, &req)) {
            obj = NULL;
            result = NT_BROKER_RESOURCE_ERROR;
        } else
            result = NT_BROKER_CREATED;
    }
    if (!reply_to_client(client->fd, &req, result, obj)) {
        if (result == NT_BROKER_CREATED && obj)
            release_object((int)(obj - objects));
        forget_client(client);
        return;
    }
    if (result == NT_BROKER_CREATED || result == NT_BROKER_EXISTS) {
        if (result == NT_BROKER_EXISTS)
            ++obj->refs;
        client->object = (int)(obj - objects);
    } else
        forget_client(client);
}
static int serve(int listener)
{
    for (size_t i = 0; i < CLIENT_LIMIT; ++i)
        clients[i] = (struct broker_client){.fd = -1, .object = -1};
    while (!stopping) {
        struct pollfd polled[CLIENT_LIMIT + 1];
        int positions[CLIENT_LIMIT + 1] = {0};
        nfds_t count = 1;
        polled[0] = (struct pollfd){.fd = listener, .events = POLLIN};
        for (size_t i = 0; i < CLIENT_LIMIT; ++i)
            if (clients[i].fd >= 0) {
                positions[count] = (int)i;
                polled[count++] = (struct pollfd){
                    .fd = clients[i].fd, .events = POLLIN | POLLHUP | POLLERR
                };
            }
        int n = poll(polled, count, 200);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return 1;
        if (polled[0].revents & POLLIN) {
            int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (fd >= 0) {
                struct ucred cred;
                socklen_t len = sizeof(cred);
                bool valid = getsockopt(fd, SOL_SOCKET, SO_PEERCRED,
                                        &cred, &len) == 0 &&
                             len == sizeof(cred) && cred.uid == geteuid() &&
                             cred.pid > 0 &&
                             setsockopt(fd, SOL_SOCKET, SO_PASSCRED,
                                        &(int){1}, sizeof(int)) == 0;
                size_t i = 0;
                while (i < CLIENT_LIMIT && clients[i].fd >= 0)
                    ++i;
                if (!valid || i == CLIENT_LIMIT)
                    close(fd);
                else
                    clients[i] = (struct broker_client){
                        .fd = fd, .object = -1, .pid = cred.pid,
                        .uid = cred.uid, .gid = cred.gid
                    };
            }
        }
        for (nfds_t i = 1; i < count; ++i) {
            if (!polled[i].revents)
                continue;
            struct broker_client *client = &clients[positions[i]];
            if (client->object >= 0)
                forget_client(client); /* lease channel closed/modified */
            else
                accept_request(client);
        }
    }
    for (size_t i = 0; i < CLIENT_LIMIT; ++i)
        if (clients[i].fd >= 0)
            forget_client(&clients[i]);
    return 0;
}
int main(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "--socket") != 0) {
        fprintf(stderr, "Usage: %s --socket /private/user/dir/nt-object-broker.sock\n",
                argv[0]);
        return 2;
    }
    const char *path = argv[2];
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    size_t n = strlen(path);
    if (path[0] != '/' || n == 0 || n >= sizeof(addr.sun_path) ||
        !allowed_directory(path)) {
        fprintf(stderr, "Broker socket must be under an owned, non-writable-by-others directory\n");
        return 2;
    }
    memcpy(addr.sun_path, path, n + 1);
    if (!make_global_gate()) {
        perror("Cannot create shared broker gate");
        return 2;
    }
    umask(077);
    int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    socklen_t length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + 1);
    if (listener < 0 || bind(listener, (struct sockaddr *)&addr, length) != 0 ||
        chmod(path, 0600) != 0 || listen(listener, 32) != 0) {
        perror("Cannot bind broker socket (will not replace an existing entry)");
        if (listener >= 0)
            close(listener);
        close(global_gate_fd);
        return 2;
    }
    struct sigaction action = {.sa_handler = stop_broker};
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    printf("READY nt-object-broker %s\n", path);
    fflush(stdout);
    int status = serve(listener);
    close(listener);
    unlink(path);
    close(global_gate_fd);
    return status;
}
