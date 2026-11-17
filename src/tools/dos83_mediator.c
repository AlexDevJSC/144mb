/* SPDX-License-Identifier: MIT */
/* Optional, unprivileged DOS 8.3 namespace mediator for a user-granted C:.
 * --move performs an opted-in Win32-mediated rename. --watch reconciles
 * paired native Linux renames AFTER they happen; it cannot intercept them
 * before they occur, nor guarantee event delivery on every filesystem. */
#define _GNU_SOURCE
#include "compat.h"
#include "winfile.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/openat2.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

enum { WATCH_LIMIT = 8192, PAIR_LIMIT = 256, MAX_DEPTH = 64 };
static volatile sig_atomic_t stop_requested;

static void request_stop(int signum)
{
    (void)signum;
    stop_requested = 1;
}

struct watched {
    int wd;
    char *relative;
};
struct pending {
    uint32_t cookie;
    time_t timestamp;
    char *old_path;
    bool directory;
};
struct monitor {
    int root;
    int inotify_fd;
    struct watched *watches;
    size_t watch_count, watch_capacity;
    struct pending pending[PAIR_LIMIT];
    bool degraded;
};

static bool joined(const char *parent, const char *name, char out[PATH_MAX])
{
    const int n = parent[0] ?
        snprintf(out, PATH_MAX, "%s/%s", parent, name) :
        snprintf(out, PATH_MAX, "%s", name);
    if (n < 0 || n >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}

static bool dos_path(const char *relative, char out[PATH_MAX])
{
    const int n = snprintf(out, PATH_MAX, "C:/%s", relative);
    if (n < 0 || n >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}

/* Use the already-pinned root and openat2, never a host symlink or a
 * second device, to discover directories for recursive inotify watches. */
static int open_directory(struct monitor *state, const char *relative)
{
    const struct open_how how = {
        .flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC,
        .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV
    };
    return (int)syscall(SYS_openat2, state->root,
                        relative[0] ? relative : ".", &how, sizeof(how));
}

static void pending_clear(struct pending *p)
{
    free(p->old_path);
    *p = (struct pending){0};
}

static void monitor_clear(struct monitor *state)
{
    for (size_t i = 0; i < state->watch_count; ++i) {
        (void)inotify_rm_watch(state->inotify_fd, state->watches[i].wd);
        free(state->watches[i].relative);
    }
    free(state->watches);
    state->watches = NULL;
    state->watch_count = state->watch_capacity = 0;
    for (size_t i = 0; i < PAIR_LIMIT; ++i)
        pending_clear(&state->pending[i]);
}

static struct watched *watch_by_wd(struct monitor *state, int wd)
{
    for (size_t i = 0; i < state->watch_count; ++i)
        if (state->watches[i].wd == wd)
            return &state->watches[i];
    return NULL;
}

static bool register_watch(struct monitor *state, int fd, const char *relative)
{
    char procpath[64];
    (void)snprintf(procpath, sizeof(procpath), "/proc/self/fd/%d", fd);
    const uint32_t mask = IN_ONLYDIR | IN_CREATE | IN_DELETE |
                          IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF |
                          IN_MOVE_SELF | IN_ATTRIB | IN_Q_OVERFLOW;
    const int wd = inotify_add_watch(state->inotify_fd, procpath, mask);
    if (wd < 0)
        return false;
    struct watched *existing = watch_by_wd(state, wd);
    if (existing != NULL)
        return true;
    if (state->watch_count >= WATCH_LIMIT) {
        errno = ENOSPC;
        return false;
    }
    if (state->watch_count == state->watch_capacity) {
        size_t next = state->watch_capacity ? state->watch_capacity * 2 : 32;
        struct watched *allocated = realloc(state->watches, next * sizeof(*allocated));
        if (!allocated)
            return false;
        state->watches = allocated;
        state->watch_capacity = next;
    }
    char *copy = strdup(relative);
    if (!copy)
        return false;
    state->watches[state->watch_count++] = (struct watched){wd, copy};
    return true;
}

static bool add_tree(struct monitor *state, const char *relative,
                     unsigned int depth, bool refresh)
{
    if (depth > MAX_DEPTH) {
        errno = ELOOP;
        return false;
    }
    int directory = open_directory(state, relative);
    if (directory < 0)
        return false;
    bool ok = register_watch(state, directory, relative);
    int scan_fd = dup(directory);
    if (scan_fd < 0) {
        close(directory);
        return false;
    }
    DIR *scan = fdopendir(scan_fd);
    if (!scan) {
        close(scan_fd);
        close(directory);
        return false;
    }
    for (;;) {
        errno = 0;
        const struct dirent *entry = readdir(scan);
        if (!entry) {
            if (errno != 0)
                ok = false;
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        struct stat st;
        if (fstatat(directory, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            ok = false;
            continue;
        }
        if (!(S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)))
            continue;
        char child[PATH_MAX];
        if (!joined(relative, entry->d_name, child)) {
            ok = false;
            continue;
        }
        if (refresh) {
            char guest[PATH_MAX];
            if (dos_path(child, guest) &&
                !nt_runtime_refresh_dos83_path(guest) &&
                errno != EINVAL && errno != EILSEQ)
                ok = false;
        }
        if (S_ISDIR(st.st_mode) &&
            !add_tree(state, child, depth + 1, refresh))
            ok = false;
    }
    if (closedir(scan) != 0)
        ok = false;
    close(directory);
    return ok;
}

static bool rebuild(struct monitor *state, bool refresh)
{
    monitor_clear(state);
    const bool ok = add_tree(state, "", 0, refresh);
    if (!ok) {
        state->degraded = true;
        fprintf(stderr, "DEGRADED: watch tree incomplete: %s\n", strerror(errno));
    }
    return ok;
}

static void update_watch_paths(struct monitor *state, const char *former,
                               const char *current)
{
    const size_t old_len = strlen(former);
    for (size_t i = 0; i < state->watch_count; ++i) {
        const char *path = state->watches[i].relative;
        if (strncmp(path, former, old_len) != 0 ||
            (path[old_len] != '\0' && path[old_len] != '/'))
            continue;
        char replacement[PATH_MAX];
        const int n = snprintf(replacement, sizeof(replacement), "%s%s",
                               current, path + old_len);
        if (n < 0 || n >= PATH_MAX) {
            state->degraded = true;
            continue;
        }
        char *copy = strdup(replacement);
        if (!copy) {
            state->degraded = true;
            continue;
        }
        free(state->watches[i].relative);
        state->watches[i].relative = copy;
    }
}

static struct pending *find_pair(struct monitor *state, uint32_t cookie)
{
    if (!cookie)
        return NULL;
    for (size_t i = 0; i < PAIR_LIMIT; ++i)
        if (state->pending[i].cookie == cookie)
            return &state->pending[i];
    return NULL;
}

static void pending_expire(struct monitor *state)
{
    time_t now = time(NULL);
    for (size_t i = 0; i < PAIR_LIMIT; ++i)
        if (state->pending[i].cookie &&
            now - state->pending[i].timestamp > 2) {
            state->degraded = true;
            fprintf(stderr, "DEGRADED: unmatched native rename (outside root or missed event)\n");
            pending_clear(&state->pending[i]);
        }
}

static bool save_pending(struct monitor *state, uint32_t cookie,
                         const char *relative, bool directory)
{
    for (size_t i = 0; i < PAIR_LIMIT; ++i)
        if (state->pending[i].cookie == 0) {
            char *copy = strdup(relative);
            if (!copy)
                return false;
            state->pending[i] = (struct pending){
                .cookie = cookie, .timestamp = time(NULL),
                .old_path = copy, .directory = directory
            };
            return true;
        }
    errno = ENOSPC;
    return false;
}

static void refresh_one(const char *relative)
{
    char guest[PATH_MAX];
    if (!dos_path(relative, guest))
        return;
    if (!nt_runtime_refresh_dos83_path(guest))
        fprintf(stderr, "DEGRADED: cannot refresh short name %s: %s\n",
                relative, strerror(errno));
}

static void monitor_event(struct monitor *state, const struct inotify_event *e,
                          bool *needs_rescan)
{
    if (e->mask & IN_Q_OVERFLOW) {
        state->degraded = true;
        *needs_rescan = true;
        return;
    }
    struct watched *watch = watch_by_wd(state, e->wd);
    if (!watch || !e->len || !e->name[0])
        return;
    char path[PATH_MAX];
    if (!joined(watch->relative, e->name, path)) {
        state->degraded = true;
        return;
    }
    const bool directory = (e->mask & IN_ISDIR) != 0;
    if (e->mask & IN_MOVED_FROM) {
        if (!save_pending(state, e->cookie, path, directory))
            state->degraded = true;
        return;
    }
    if (e->mask & IN_MOVED_TO) {
        struct pending *paired = find_pair(state, e->cookie);
        if (paired != NULL) {
            char oldguest[PATH_MAX], newguest[PATH_MAX];
            if (dos_path(paired->old_path, oldguest) && dos_path(path, newguest)) {
                if (!nt_runtime_reconcile_host_rename(oldguest, newguest)) {
                    if (errno == ENODATA)
                        refresh_one(path);
                    else {
                        state->degraded = true;
                        fprintf(stderr, "DEGRADED: cannot reconcile %s -> %s: %s\n",
                                paired->old_path, path, strerror(errno));
                    }
                } else {
                    printf("RECONCILED %s -> %s\n", paired->old_path, path);
                    fflush(stdout);
                }
            }
            if (directory)
                update_watch_paths(state, paired->old_path, path);
            pending_clear(paired);
        } else {
            state->degraded = true; /* Unmatched move into the granted root. */
            refresh_one(path);
        }
    } else if (e->mask & IN_CREATE) {
        refresh_one(path);
    }
    if (directory && (e->mask & (IN_MOVED_TO | IN_CREATE))) {
        if (!add_tree(state, path, 0, true))
            state->degraded = true;
    }
}

static int run_watch(const char *rootpath)
{
    struct monitor state = { .root = -1, .inotify_fd = -1 };
    state.root = open(rootpath, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (state.root < 0)
        return 2;
    state.inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (state.inotify_fd < 0) {
        close(state.root);
        return 2;
    }
    if (!rebuild(&state, true))
        fprintf(stderr, "DEGRADED: initial scan incomplete\n");
    (void)signal(SIGINT, request_stop);
    (void)signal(SIGTERM, request_stop);
    puts("READY nt-dos83-mediator: inotify is eventual, not an intercept");
    fflush(stdout);
    while (!stop_requested) {
        struct pollfd pfd = { .fd = state.inotify_fd, .events = POLLIN };
        int result = poll(&pfd, 1, 500);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            state.degraded = true;
            break;
        }
        pending_expire(&state);
        if (!result)
            continue;
        char bytes[65536] __attribute__((aligned(__alignof__(struct inotify_event))));
        ssize_t got = read(state.inotify_fd, bytes, sizeof(bytes));
        if (got < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (got <= 0) {
            state.degraded = true;
            break;
        }
        bool overflow = false;
        for (size_t pos = 0; pos + sizeof(struct inotify_event) <= (size_t)got; ) {
            const struct inotify_event *e = (const void *)(bytes + pos);
            const size_t event_size = sizeof(*e) + (size_t)e->len;
            if (event_size > (size_t)got - pos) {
                overflow = true;
                break;
            }
            monitor_event(&state, e, &overflow);
            pos += event_size;
        }
        if (overflow) {
            fprintf(stderr, "DEGRADED: event overflow; rebuilding watch tree\n");
            (void)rebuild(&state, true);
        }
    }
    const bool degraded = state.degraded;
    monitor_clear(&state);
    close(state.inotify_fd);
    close(state.root);
    return degraded ? 3 : 0;
}

static bool to_wide(const char *input, uint16_t *output, size_t capacity)
{
    const size_t len = strlen(input);
    if (len >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return false;
    }
    /* The CLI takes host UTF-8, not Win32 A/CP1252. */
    const int32_t result = nt_MultiByteToWideChar(65001, 8, input, -1,
                                                  output, (int32_t)capacity);
    if (result <= 0)
        errno = EILSEQ;
    return result > 0;
}

int main(int argc, char **argv)
{
    if (argc < 4 || strcmp(argv[1], "--root") != 0) {
        fprintf(stderr, "Usage: %s --root DIR --watch | --scan | --recover | --recover-with-drive D_ROOT | --refresh DOS_PATH | --mark DOS_PATH | --reconcile OLD NEW | --move OLD NEW | --replace OLD NEW\n", argv[0]);
        return 2;
    }
    if (!nt_runtime_set_file_root(argv[2])) {
        perror("root");
        return 2;
    }
    int result = 2;
    if (argc == 4 && strcmp(argv[3], "--recover") == 0) {
        result = nt_runtime_recover_dos83() ? 0 : 3;
        if (result)
            perror("DOS 8.3 WAL recovery refused");
    } else if (argc == 5 && strcmp(argv[3], "--recover-with-drive") == 0) {
        /* Host diagnostic for the same D: preflight used by the PE worker.
         * This deliberately requires an explicit D: directory grant. */
        result = nt_runtime_set_drive_root('D', argv[4]) &&
                 nt_runtime_recover_all_dos83_grants() ? 0 : 3;
        if (result)
            perror("DOS 8.3 WAL recovery refused on a granted drive");
    } else if (argc == 4 && strcmp(argv[3], "--watch") == 0) {
        result = nt_runtime_recover_dos83() ? run_watch(argv[2]) : 3;
    } else if (argc == 4 && strcmp(argv[3], "--scan") == 0) {
        if (!nt_runtime_recover_dos83()) {
            perror("DOS 8.3 WAL recovery refused");
            return 3;
        }
        struct monitor state = { .root = -1, .inotify_fd = -1 };
        state.root = open(argv[2], O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (state.root < 0) {
            perror("root");
            return 2;
        }
        state.inotify_fd = inotify_init1(IN_CLOEXEC);
        if (state.inotify_fd < 0) {
            perror("inotify");
            close(state.root);
            return 2;
        }
        result = rebuild(&state, true) ? 0 : 3;
        monitor_clear(&state);
        close(state.inotify_fd);
        close(state.root);
    } else if (argc == 5 && strcmp(argv[3], "--mark") == 0) {
        /* Mark a pinned regular file via the authorized DOS grant, never
         * by following arbitrary host symlinks or a caller-provided FD. */
        const int fd = nt_runtime_open_guest_file_readonly(argv[4]);
        if (fd < 0) {
            result = 1;
        } else {
            struct stat info;
            result = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
                     fsetxattr(fd, NT_FS_COMPAT_MARKER, "1", 1, 0) == 0 ? 0 : 1;
            close(fd);
        }
    } else if (argc == 5 && strcmp(argv[3], "--refresh") == 0) {
        result = nt_runtime_refresh_dos83_path(argv[4]) ? 0 : 1;
    } else if (argc == 6 && strcmp(argv[3], "--reconcile") == 0) {
        result = nt_runtime_reconcile_host_rename(argv[4], argv[5]) ? 0 : 1;
    } else if (argc == 6 &&
               (strcmp(argv[3], "--move") == 0 ||
                strcmp(argv[3], "--replace") == 0)) {
        uint16_t from[PATH_MAX], to[PATH_MAX];
        const uint32_t flags = strcmp(argv[3], "--replace") == 0 ? 1u : 0u;
        result = to_wide(argv[4], from, PATH_MAX) &&
                 to_wide(argv[5], to, PATH_MAX) &&
                 nt_MoveFileExW(from, to, flags) ? 0 : 1;
    } else {
        fprintf(stderr, "Unsupported mode or argument count\n");
    }
    if (result == 1)
        fprintf(stderr, "Operation failed (errno=%d, native=%s)\n",
                errno, strerror(errno));
    return result;
}
