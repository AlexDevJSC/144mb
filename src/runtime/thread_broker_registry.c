/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "thread_broker_registry.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { MAX_REGISTERED_CHILDREN = 64, MAX_BROKER_CAPABILITIES = 256 };
struct registered_child {
    pid_t pid;
    uid_t uid;
    gid_t gid;
    int pidfd;
    uint64_t generation;
    uint8_t secret[16];
    uint32_t grants[MAX_REGISTERED_CHILDREN];
    bool active;
};
struct broker_capability {
    uint64_t handle;
    uint64_t generation;
    pid_t requester;
    uint32_t access;
    unsigned target_slot;
    bool active;
};
static pthread_mutex_t broker_lock = PTHREAD_MUTEX_INITIALIZER;
static struct registered_child children[MAX_REGISTERED_CHILDREN];
static struct broker_capability capabilities[MAX_BROKER_CAPABILITIES];
static uint64_t next_generation = 1;
static uint64_t next_handle = UINT64_C(0xb144000000000004);
static const uint32_t query_rights =
    NT_BROKER_QUERY_INFORMATION | NT_BROKER_QUERY_LIMITED_INFORMATION;

static bool positive_rights(uint32_t access)
{
    return access != 0 && (access & ~query_rights) == 0;
}
static bool alive_pidfd(int fd)
{
    if (fd < 0)
        return false;
    struct pollfd probe = {.fd = fd, .events = POLLIN};
    return poll(&probe, 1, 0) == 0 && probe.revents == 0;
}

/* A child PID alone is never accepted as identity. A fresh kernel pidfd
 * must identify the same task as the caller's trusted pidfd. */
static bool same_kernel_child(pid_t pid, int pidfd)
{
#if defined(SYS_pidfd_open)
    if (pid <= 0 || pid == getpid() || !alive_pidfd(pidfd))
        return false;
    const int fresh = (int)syscall(SYS_pidfd_open, pid, 0);
    if (fresh < 0)
        return false;
    struct stat original_stat, fresh_stat;
    const bool matched = fstat(pidfd, &original_stat) == 0 &&
        fstat(fresh, &fresh_stat) == 0 &&
        original_stat.st_dev == fresh_stat.st_dev &&
        original_stat.st_ino == fresh_stat.st_ino &&
        alive_pidfd(fresh);
    close(fresh);
    return matched;
#else
    (void)pid;
    (void)pidfd;
    return false;
#endif
}
static int find_child_locked(pid_t pid)
{
    for (unsigned i = 0; i < MAX_REGISTERED_CHILDREN; ++i)
        if (children[i].active && children[i].pid == pid)
            return (int)i;
    return -1;
}
static bool active_child_locked(int slot)
{
    return slot >= 0 && children[slot].active &&
           alive_pidfd(children[slot].pidfd);
}
static void drop_capabilities_locked(unsigned slot)
{
    for (size_t i = 0; i < MAX_BROKER_CAPABILITIES; ++i)
        if (capabilities[i].active &&
            (capabilities[i].target_slot == slot ||
             capabilities[i].requester == children[slot].pid))
            capabilities[i].active = false;
}

bool nt_broker_register_child(pid_t pid, int pidfd, uid_t uid, gid_t gid,
                              const uint8_t secret[16], uint64_t *generation)
{
    if (secret == NULL || generation == NULL ||
        !same_kernel_child(pid, pidfd))
        return false;
    const int copy = fcntl(pidfd, F_DUPFD_CLOEXEC, 3);
    if (copy < 0 || !same_kernel_child(pid, copy)) {
        if (copy >= 0)
            close(copy);
        return false;
    }
    pthread_mutex_lock(&broker_lock);
    int slot = -1;
    if (find_child_locked(pid) < 0 && next_generation != UINT64_MAX)
        for (unsigned i = 0; i < MAX_REGISTERED_CHILDREN; ++i)
            if (!children[i].active) {
                slot = (int)i;
                break;
            }
    if (slot >= 0) {
        /* A recycled slot cannot inherit grants from either direction. */
        for (unsigned i = 0; i < MAX_REGISTERED_CHILDREN; ++i)
            children[i].grants[slot] = 0;
        struct registered_child *child = &children[slot];
        *child = (struct registered_child){0};
        child->pid = pid;
        child->uid = uid;
        child->gid = gid;
        child->pidfd = copy;
        child->generation = next_generation++;
        memcpy(child->secret, secret, 16);
        child->active = true;
        *generation = child->generation;
    }
    pthread_mutex_unlock(&broker_lock);
    if (slot < 0)
        close(copy);
    return slot >= 0;
}

void nt_broker_unregister_child(pid_t pid, uint64_t generation)
{
    pthread_mutex_lock(&broker_lock);
    const int slot = find_child_locked(pid);
    if (slot >= 0 && children[slot].generation == generation) {
        drop_capabilities_locked((unsigned)slot);
        for (unsigned i = 0; i < MAX_REGISTERED_CHILDREN; ++i)
            children[i].grants[slot] = 0;
        struct registered_child *child = &children[slot];
        close(child->pidfd);
        memset(child, 0, sizeof(*child));
        child->pidfd = -1;
    }
    pthread_mutex_unlock(&broker_lock);
}

bool nt_broker_grant_child_query(pid_t requester, pid_t target, uint32_t access)
{
    if (!positive_rights(access) || requester == target)
        return false;
    pthread_mutex_lock(&broker_lock);
    const int client = find_child_locked(requester);
    const int destination = find_child_locked(target);
    const bool allowed = active_child_locked(client) &&
                         active_child_locked(destination);
    if (allowed)
        children[client].grants[destination] = access;
    pthread_mutex_unlock(&broker_lock);
    return allowed;
}

bool nt_broker_open_child_thread(pid_t requester, pid_t target, uint32_t access,
                                uint64_t *opaque_handle)
{
    if (opaque_handle == NULL || !positive_rights(access))
        return false;
    pthread_mutex_lock(&broker_lock);
    const int destination = find_child_locked(target);
    const int client = find_child_locked(requester);
    const bool trusted_parent = requester == getpid();
    const bool permitted = active_child_locked(destination) &&
        (trusted_parent ||
         (active_child_locked(client) && client != destination &&
          (access & ~children[client].grants[destination]) == 0));
    int free_slot = -1;
    if (permitted && next_handle <= UINT64_MAX - 4)
        for (unsigned i = 0; i < MAX_BROKER_CAPABILITIES; ++i)
            if (!capabilities[i].active) {
                free_slot = (int)i;
                break;
            }
    if (free_slot >= 0) {
        capabilities[free_slot] = (struct broker_capability){
            .handle = next_handle,
            .generation = children[destination].generation,
            .requester = requester,
            .access = access,
            .target_slot = (unsigned)destination,
            .active = true
        };
        *opaque_handle = next_handle;
        next_handle += 4;
    }
    pthread_mutex_unlock(&broker_lock);
    return free_slot >= 0;
}

static struct broker_capability *find_capability_locked(uint64_t opaque)
{
    if (opaque == 0)
        return NULL;
    for (size_t i = 0; i < MAX_BROKER_CAPABILITIES; ++i)
        if (capabilities[i].active && capabilities[i].handle == opaque)
            return &capabilities[i];
    return NULL;
}

bool nt_broker_query_child_thread(pid_t requester, uint64_t opaque_handle,
                                  uint32_t required_access,
                                  struct nt_broker_thread_info *info)
{
    if (!positive_rights(required_access) || info == NULL)
        return false;
    pthread_mutex_lock(&broker_lock);
    struct broker_capability *cap = find_capability_locked(opaque_handle);
    bool ok = false;
    if (cap != NULL && cap->requester == requester &&
        (required_access & ~cap->access) == 0) {
        const struct registered_child *target = &children[cap->target_slot];
        if (target->active && target->generation == cap->generation &&
            alive_pidfd(target->pidfd)) {
            /* Main thread only: the target PID is a verified child, not
             * a guest-provided Linux TID. No /proc memory or host mutation. */
            *info = (struct nt_broker_thread_info){
                .guest_process_id = (uint32_t)target->pid,
                .guest_thread_id = (uint32_t)target->pid,
                .generation = target->generation
            };
            ok = true;
        }
    }
    pthread_mutex_unlock(&broker_lock);
    return ok;
}

bool nt_broker_close_child_thread(pid_t requester, uint64_t opaque_handle)
{
    pthread_mutex_lock(&broker_lock);
    struct broker_capability *cap = find_capability_locked(opaque_handle);
    const bool ok = cap != NULL && cap->requester == requester;
    if (ok)
        cap->active = false;
    pthread_mutex_unlock(&broker_lock);
    return ok;
}

/* Wire authentication is performed independently of the trusted registry:
 * SCM_CREDENTIALS, pidfd and the random token are validated before
 * granting a capability. The peer's registered identity must also match
 * the registered child and the explicit child-to-child grant. */
bool nt_broker_authenticated_open_child(int socket_fd,
    const struct nt_thread_broker_registered_peer *peer,
    pid_t target, uint64_t *out_handle)
{
    if (peer == NULL || out_handle == NULL)
        return false;
    uint32_t requested_access = 0;
    if (nt_thread_broker_receive_query(socket_fd, peer, &requested_access) !=
        NT_THREAD_BROKER_OK)
        return false;
    pthread_mutex_lock(&broker_lock);
    const int client = find_child_locked(peer->child_pid);
    bool registered = active_child_locked(client);
    if (registered) {
        const struct registered_child *source = &children[client];
        struct stat a, b;
        registered = source->uid == peer->uid &&
            source->gid == peer->gid &&
            memcmp(source->secret, peer->secret, 16) == 0 &&
            fstat(source->pidfd, &a) == 0 &&
            fstat(peer->child_pidfd, &b) == 0 &&
            a.st_dev == b.st_dev && a.st_ino == b.st_ino;
    }
    pthread_mutex_unlock(&broker_lock);
    return registered && nt_broker_open_child_thread(
        peer->child_pid, target, requested_access, out_handle);
}
