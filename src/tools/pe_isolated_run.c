/* SPDX-License-Identifier: MIT */
#define _XOPEN_SOURCE 700
#include "image.h"
#include "ntnative/guest_thread.h"
#include "pe_bytes.h"
#include "ntnative/runtime.h"
#include "compat.h"
#include "desktop.h"
#include "desktop_resources.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/magic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/vfs.h>
#include <unistd.h>

extern char **environ;

static struct pe_image active;
static const char *active_dos_path;

static uintptr_t resolve(const char *module, const char *name, uint16_t ordinal, void *context)
{
    (void)context;
    nt_guest_proc proc = name != NULL ? nt_runtime_resolve(module, name) :
                          nt_runtime_resolve_ordinal(module, ordinal);
    uintptr_t target = 0;
    _Static_assert(sizeof(target) == sizeof(proc), "AMD64 pointer ABI");
    memcpy(&target, &proc, sizeof(target));
    if (proc == NULL)
        fprintf(stderr, "Unsupported guest import %s!%s%u\n", module,
                name != NULL ? name : "#", name == NULL ? ordinal : 0);
    return target;
}

static int NT_MS_ABI guest_entry(void)
{
    nt_runtime_set_guest_environment((const char *const *)environ);
    if (!nt_runtime_set_file_root("/game"))
        return 118;
    char command[PATH_MAX + 4];
    if (snprintf(command, sizeof(command), "\"%s\"", active_dos_path) >= (int)sizeof(command))
        return 119;
    if (!nt_runtime_set_guest_image((uintptr_t)active.base, active_dos_path, command))
        return 120;
    if (!nt_runtime_set_standard_streams(STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO))
        return 121;
    if (!nt_runtime_open_desktop())
        return 122;
    if (active.resource_size &&
        !nt_runtime_set_icon_resources((uintptr_t)active.base, active.resource_rva,
                                       active.base + active.resource_rva, active.resource_size)) {
        nt_runtime_close_desktop();
        return 123;
    }
    if (active.exception_size) {
        if (active.exception_size % sizeof(struct nt_runtime_function)) {
            nt_runtime_close_desktop();
            return 124;
        }
        (void)nt_runtime_set_runtime_functions(
            (uintptr_t)active.base, active.size,
            (const struct nt_runtime_function *)(const void *)(active.base + active.exception_rva),
            active.exception_size / sizeof(struct nt_runtime_function));
    }
    struct nt_guest_thread guest_thread = {0};
    if (!nt_guest_thread_enter(&guest_thread, (uintptr_t)active.base)) {
        nt_runtime_close_desktop();
        return 125;
    }
    typedef void (NT_MS_ABI *tls_callback)(void *, uint32_t, void *);
    for (uint32_t i = 0; i < active.tls_callback_count; ++i) {
        const uint64_t value = read_u64(active.base + active.tls_callbacks_rva + i * 8);
        tls_callback callback;
        memcpy(&callback, &value, sizeof(callback));
        callback(active.base, 1, NULL); /* DLL_PROCESS_ATTACH */
    }
    const void *address = active.base + active.entry_rva;
    int (NT_MS_ABI *entry)(void);
    memcpy(&entry, &address, sizeof(entry));
    const int status = entry();
    for (uint32_t i = 0; i < active.tls_callback_count; ++i) {
        const uint64_t value = read_u64(active.base + active.tls_callbacks_rva + i * 8);
        tls_callback callback;
        memcpy(&callback, &value, sizeof(callback));
        callback(active.base, 0, NULL); /* DLL_PROCESS_DETACH */
    }
    if (!nt_guest_thread_leave(&guest_thread)) {
        nt_runtime_close_desktop();
        return 127;
    }
    nt_runtime_close_desktop();
    return status;
}

static int run_worker(const char *name)
{
    struct statfs root;
    if (getpid() != 1 || prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1 ||
        statfs("/", &root) != 0 || root.f_type != TMPFS_MAGIC) {
        fputs("Refusing worker outside sandbox; use pe-isolated-run FILE.exe\n", stderr);
        return 125;
    }
    if (name[0] != '/' || strncmp(name, "/game/", 6) != 0 ||
        strchr(name + 6, '/') != NULL || strchr(name + 6, '\\') != NULL ||
        strstr(name + 6, "..") != NULL)
        return 126;
    const int fd = open(name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        perror("PE open");
        return 2;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size <= 0 || st.st_size > PE_IMAGE_MAX_BYTES) {
        fputs("Invalid or oversized PE input\n", stderr);
        close(fd);
        return 2;
    }
    const size_t size = (size_t)st.st_size;
    unsigned char *bytes = malloc(size);
    if (bytes == NULL) {
        close(fd);
        return 2;
    }
    size_t count = 0;
    while (count < size) {
        ssize_t n = read(fd, bytes + count, size - count);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        count += (size_t)n;
    }
    close(fd);
    if (count != size) {
        fputs("Could not read PE image\n", stderr);
        free(bytes);
        return 2;
    }
    char error[160];
    const bool loaded = pe_image_load(bytes, size, resolve, NULL,
                                      &active, error, sizeof(error));
    free(bytes);
    if (!loaded) {
        fprintf(stderr, "PE loader: %s\n", error);
        return 3;
    }
    const char *file_name = name + 6;
    char dos_path[PATH_MAX];
    if (snprintf(dos_path, sizeof(dos_path), "C:\\%s", file_name) >= (int)sizeof(dos_path))
        return 4;
    active_dos_path = dos_path;
    uint32_t code = 0;
    const bool ran = nt_runtime_run_fixture(guest_entry, &code);
    pe_image_unload(&active);
    if (!ran) {
        fputs("Guest terminated abnormally or worker failed\n", stderr);
        return 5;
    }
    fprintf(stderr, "Guest exited with code %u\n", code);
    return code <= 255 ? (int)code : 1;
}

static bool add_arg(char **args, size_t *count, size_t max, char *item)
{
    if (*count + 1 >= max)
        return false;
    args[(*count)++] = item;
    return true;
}

static int run_isolated(const char *path)
{
    char absolute[PATH_MAX], runner[PATH_MAX], socket_path[PATH_MAX];
    if (realpath(path, absolute) == NULL) {
        perror("Cannot resolve executable paths");
        return 2;
    }
    const ssize_t got = readlink("/proc/self/exe", runner, sizeof(runner) - 1);
    if (got < 0 || got >= (ssize_t)sizeof(runner)) {
        fputs("Invalid runner path\n", stderr);
        return 2;
    }
    runner[got] = '\0';
    char *slash = strrchr(absolute, '/');
    if (slash == NULL || slash[1] == '\0' || strstr(slash + 1, "..") != NULL) {
        fputs("Invalid executable name\n", stderr);
        return 2;
    }
    struct stat input;
    if (stat(absolute, &input) != 0 || !S_ISREG(input.st_mode)) {
        fputs("Expected a regular PE file\n", stderr);
        return 2;
    }
    char inside[PATH_MAX];
    if (snprintf(inside, sizeof(inside), "/game/%s", slash + 1) >= (int)sizeof(inside))
        return 2;

    char *args[64];
    size_t n = 0;
#define ADD(x) do { if (!add_arg(args, &n, 64, (char *)(x))) return 2; } while (0)
    ADD("bwrap");
    ADD("--die-with-parent"); ADD("--unshare-all"); ADD("--new-session"); ADD("--as-pid-1");
    ADD("--clearenv"); ADD("--setenv"); ADD("PATH"); ADD("/usr/bin:/bin");
    ADD("--setenv"); ADD("HOME"); ADD("/game");
    ADD("--ro-bind"); ADD("/usr"); ADD("/usr");
    if (access("/lib", F_OK) == 0) { ADD("--ro-bind"); ADD("/lib"); ADD("/lib"); }
    if (access("/lib64", F_OK) == 0) { ADD("--ro-bind"); ADD("/lib64"); ADD("/lib64"); }
    ADD("--ro-bind-try"); ADD("/etc/fonts"); ADD("/etc/fonts");
    ADD("--proc"); ADD("/proc"); ADD("--dev"); ADD("/dev");
    ADD("--tmpfs"); ADD("/tmp");
    ADD("--tmpfs"); ADD("/game");
    ADD("--ro-bind"); ADD(absolute); ADD(inside);
    ADD("--ro-bind"); ADD(runner); ADD("/runner");
    ADD("--chdir"); ADD("/game");

    const char *video = getenv("SDL_VIDEODRIVER");
    if (video != NULL && strcmp(video, "offscreen") == 0) {
        ADD("--setenv"); ADD("SDL_VIDEODRIVER"); ADD("offscreen");
        ADD("--setenv"); ADD("SDL_FRAMEBUFFER_ACCELERATION"); ADD("0");
    } else {
        const char *runtime = getenv("XDG_RUNTIME_DIR");
        const char *wayland = getenv("WAYLAND_DISPLAY");
        if (runtime == NULL || wayland == NULL || strchr(wayland, '/') != NULL ||
            *wayland == '\0' || strlen(wayland) > 64) {
            fputs("Wayland session required (or SDL_VIDEODRIVER=offscreen for tests)\n", stderr);
            return 2;
        }
        struct stat socket_info;
        if (runtime[0] != '/' ||
            snprintf(socket_path, sizeof(socket_path), "%s/%s", runtime, wayland) >= (int)sizeof(socket_path) ||
            lstat(socket_path, &socket_info) != 0 || !S_ISSOCK(socket_info.st_mode)) {
            fputs("Wayland socket unavailable\n", stderr);
            return 2;
        }
        ADD("--dir"); ADD("/run/ntnative");
        ADD("--ro-bind"); ADD(socket_path); ADD("/run/ntnative/wayland");
        ADD("--setenv"); ADD("XDG_RUNTIME_DIR"); ADD("/run/ntnative");
        ADD("--setenv"); ADD("WAYLAND_DISPLAY"); ADD("wayland");
        ADD("--setenv"); ADD("SDL_VIDEODRIVER"); ADD("wayland");
    }
    ADD("--"); ADD("/runner"); ADD("--worker"); ADD(inside);
    args[n] = NULL;
#undef ADD
    execv("/usr/bin/bwrap", args);
    perror("bubblewrap unavailable; install bubblewrap and enable user namespaces");
    return 127;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--worker") == 0)
        return run_worker(argv[2]);
    if (argc != 2 || strcmp(argv[1], "--help") == 0) {
        fprintf(stderr, "Usage: %s path/to/Mines.exe\n"
                "Requires bubblewrap + Wayland. For headless smoke tests use SDL_VIDEODRIVER=offscreen.\n",
                argv[0]);
        return argc == 2 ? 0 : 2;
    }
    return run_isolated(argv[1]);
}
