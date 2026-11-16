/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "thread_broker_registry.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { MAX_REGISTERED_CHILDREN = 64, MAX_BROKER_CAPABILITIES = 256,
       MAX_REGISTERED_THREADS = 512 };
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
struct registered_thread {
    pid_t owner, tid;
    int task_fd;
    uint64_t generation;
    bool active;
};
struct broker_capability {
    uint64_t handle;
    uint64_t generation;
    pid_t requester;
    uint32_t access;
    unsigned target_slot;
    int thread_slot;
    uint64_t thread_generation;
    bool active;
};
static pthread_mutex_t broker_lock = PTHREAD_MUTEX_INITIALIZER;
static struct registered_child children[MAX_REGISTERED_CHILDREN];
static struct broker_capability capabilities[MAX_BROKER_CAPABILITIES];
static struct registered_thread threads[MAX_REGISTERED_THREADS];
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
/* /proc/<owned child>/task/<tid> can only name a task in that child's
 * thread group. O_NOFOLLOW and inode stability prevent a recycled Linux TID
 * from inheriting an older guest object's privileges. */
static int open_owned_task(pid_t pid, pid_t tid)
{
    const int child = find_child_locked(pid);
    if (pid <= 0 || tid <= 0 || child < 0 ||
        !alive_pidfd(children[child].pidfd))
        return -1;
    char path[96];
    const int n = snprintf(path, sizeof(path), "/proc/%ld/task/%ld",
                           (long)pid, (long)tid);
    if (n <= 0 || n >= (int)sizeof(path))
        return -1;
    return open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}
static bool live_owned_task(const struct registered_thread *thread)
{
    if (!thread->active)
        return false;
    const int fd = open_owned_task(thread->owner, thread->tid);
    if (fd < 0)
        return false;
    struct stat was, now;
    const bool okay = fstat(thread->task_fd, &was) == 0 &&
        fstat(fd, &now) == 0 &&
        was.st_dev == now.st_dev && was.st_ino == now.st_ino;
    close(fd);
    return okay;
}
static void clear_threads_locked(pid_t pid)
{
    for (unsigned i = 0; i < MAX_REGISTERED_THREADS; ++i)
        if (threads[i].active && threads[i].owner == pid) {
            for (unsigned j = 0; j < MAX_BROKER_CAPABILITIES; ++j)
                if (capabilities[j].active && capabilities[j].thread_slot == (int)i)
                    capabilities[j].active = false;
            close(threads[i].task_fd);
            threads[i] = (struct registered_thread){0};
        }
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

bool nt_broker_install_child_session(pid_t pid, const uint8_t secret[16])
{
    if (secret == NULL)
        return false;
    pthread_mutex_lock(&broker_lock);
    const int child = find_child_locked(pid);
    const bool okay = active_child_locked(child);
    if (okay) {
        drop_capabilities_locked((unsigned)child);
        memcpy(children[child].secret, secret, 16);
    }
    pthread_mutex_unlock(&broker_lock);
    return okay;
}

void nt_broker_unregister_child(pid_t pid, uint64_t generation)
{
    pthread_mutex_lock(&broker_lock);
    const int slot = find_child_locked(pid);
    if (slot >= 0 && children[slot].generation == generation) {
        drop_capabilities_locked((unsigned)slot);
        clear_threads_locked(pid);
        for (unsigned i = 0; i < MAX_REGISTERED_CHILDREN; ++i)
            children[i].grants[slot] = 0;
        struct registered_child *child = &children[slot];
        close(child->pidfd);
        memset(child, 0, sizeof(*child));
        child->pidfd = -1;
    }
    pthread_mutex_unlock(&broker_lock);
}

bool nt_broker_register_guest_thread(pid_t owner, pid_t tid)
{
    /* Host-internal: only verified SCM_CREDENTIALS messages from the
     * owner process are allowed to reach this function. */
    if (owner <= 0 || tid <= 0 || owner == tid)
        return false;
    pthread_mutex_lock(&broker_lock);
    const int child = find_child_locked(owner);
    int slot = -1;
    if (active_child_locked(child) && next_generation != UINT64_MAX) {
        for (unsigned i = 0; i < MAX_REGISTERED_THREADS; ++i) {
            if (threads[i].active && threads[i].owner == owner &&
                threads[i].tid == tid) {
                pthread_mutex_unlock(&broker_lock);
                return false;
            }
            if (!threads[i].active && slot < 0)
                slot = (int)i;
        }
    }
    int fd = slot >= 0 ? open_owned_task(owner, tid) : -1;
    const bool okay = fd >= 0;
    if (okay)
        threads[slot] = (struct registered_thread){
            .owner = owner, .tid = tid, .task_fd = fd,
            .generation = next_generation++, .active = true
        };
    pthread_mutex_unlock(&broker_lock);
    return okay;
}

void nt_broker_unregister_guest_thread(pid_t owner, pid_t tid)
{
    pthread_mutex_lock(&broker_lock);
    for (unsigned i = 0; i < MAX_REGISTERED_THREADS; ++i)
        if (threads[i].active && threads[i].owner == owner &&
            threads[i].tid == tid) {
            for (unsigned j = 0; j < MAX_BROKER_CAPABILITIES; ++j)
                if (capabilities[j].active &&
                    capabilities[j].thread_slot == (int)i)
                    capabilities[j].active = false;
            close(threads[i].task_fd);
            threads[i] = (struct registered_thread){0};
            break;
        }
    pthread_mutex_unlock(&broker_lock);
}

void nt_broker_grant_existing_sibling_queries(pid_t new_child)
{
    pthread_mutex_lock(&broker_lock);
    const int newly = find_child_locked(new_child);
    if (active_child_locked(newly))
        for (unsigned i = 0; i < MAX_REGISTERED_CHILDREN; ++i)
            if ((int)i != newly && active_child_locked((int)i)) {
                children[newly].grants[i] = NT_BROKER_QUERY_LIMITED_INFORMATION;
                children[i].grants[newly] = NT_BROKER_QUERY_LIMITED_INFORMATION;
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

bool nt_broker_open_registered_thread(pid_t requester, pid_t tid,
                                      uint32_t access, uint64_t *opaque_handle)
{
    if (opaque_handle == NULL || tid <= 0 || !positive_rights(access))
        return false;
    pthread_mutex_lock(&broker_lock);
    int thread_slot = -1;
    int destination = find_child_locked(tid); /* initial: TID==PID */
    if (destination < 0) {
        for (unsigned i = 0; i < MAX_REGISTERED_THREADS; ++i)
            if (threads[i].active && threads[i].tid == tid &&
                live_owned_task(&threads[i])) {
                thread_slot = (int)i;
                destination = find_child_locked(threads[i].owner);
                break;
            }
    }
    const int client = find_child_locked(requester);
    const bool trusted_parent = requester == getpid();
    const bool permitted = active_child_locked(destination) &&
        (thread_slot < 0 || live_owned_task(&threads[thread_slot])) &&
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
            .thread_slot = thread_slot,
            .thread_generation = thread_slot >= 0 ?
                threads[thread_slot].generation : children[destination].generation,
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

bool nt_broker_open_child_thread(pid_t requester, pid_t target, uint32_t access,
                                uint64_t *opaque_handle)
{
    return nt_broker_open_registered_thread(requester, target, access,
                                            opaque_handle);
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
            const struct registered_thread *thread =
                cap->thread_slot >= 0 ? &threads[cap->thread_slot] : NULL;
            if (thread == NULL || (thread->active &&
                                   thread->generation == cap->thread_generation &&
                                   thread->owner == target->pid &&
                                   live_owned_task(thread))) {
                *info = (struct nt_broker_thread_info){
                    .guest_process_id = (uint32_t)target->pid,
                    .guest_thread_id = thread == NULL ? (uint32_t)target->pid :
                                                         (uint32_t)thread->tid,
                    .generation = cap->thread_generation
                };
                ok = true;
            }
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
