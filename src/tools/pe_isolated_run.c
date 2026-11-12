/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "image.h"
#include "ntnative/guest_thread.h"
#include "ntnative/runtime.h"
#include "compat.h"
#include "desktop.h"
#include "desktop_resources.h"
#include "guest_profile.h"
#include "winbase.h"
#include "winfile.h"
#include "sync.h"
#include "user_folders.h"
#include "dxcompiler_path.h"
#include "child_process.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/magic.h>
#include <stdint.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/vfs.h>
#include <unistd.h>

extern char **environ;

static struct pe_image active;
static const char *active_dos_path, *active_command, *active_directory;

static uintptr_t resolve(const char *module, const char *name, uint16_t ordinal, void *context)
{
    (void)context;
    nt_guest_proc proc = name != NULL ? nt_runtime_resolve(module, name) :
                          nt_runtime_resolve_ordinal(module, ordinal);
    uintptr_t target = 0;
    _Static_assert(sizeof(target) == sizeof(proc), "AMD64 pointer ABI");
    memcpy(&target, &proc, sizeof(target));
    if (proc == NULL) {
        if (name != NULL)
            fprintf(stderr, "Unsupported guest import %s!%s\n", module, name);
        else
            fprintf(stderr, "Unsupported guest import %s!#%u\n", module, ordinal);
    }
    return target;
}

static int NT_MS_ABI guest_entry(void)
{
    nt_runtime_set_guest_environment((const char *const *)environ);
    if (!nt_runtime_set_file_root("/game"))
        return 118;
    for (size_t i = 0; i < NT_USER_FOLDER_COUNT; ++i) {
        char directory[sizeof(NT_GUEST_FOLDER_MOUNTS) + 32];
        snprintf(directory, sizeof(directory), "%s/%s", NT_GUEST_FOLDER_MOUNTS, nt_user_folders[i].name);
        if (!nt_runtime_set_user_folder_root(i, directory) && errno != ENOENT) {
            perror("Cannot open shared folder in sandbox");
            return 118;
        }
    }
    if (active_directory) {
        uint16_t *wide = nt_runtime_utf8_to_wide(active_directory);
        bool changed = wide && nt_SetCurrentDirectoryW(wide);
        free(wide);
        if (!changed)
            return 119;
    }
    if (!nt_runtime_set_guest_image((uintptr_t)active.base, active_dos_path, active_command))
        return 120;
    nt_runtime_set_guest_image_extent(active.size);
    const struct nt_guest_tls initial = {active.base + active.tls.data_rva, active.tls.bytes, active.tls.zero_fill};
    nt_runtime_set_guest_tls(&initial, active.base + active.tls.callbacks_rva, active.tls.callback_count);
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
        if (active.exception_size % sizeof(struct nt_runtime_function) ||
            !nt_runtime_set_runtime_functions((uintptr_t)active.base, active.size,
                (const struct nt_runtime_function *)(const void *)(active.base + active.exception_rva),
                active.exception_size / sizeof(struct nt_runtime_function)) ||
            !nt_runtime_enable_software_seh(active.code_rva, active.code_rva + active.code_size,
                                            pe_image_readable, &active)) {
            fputs("Cannot configure guest AMD64 unwind metadata\n", stderr);
            nt_runtime_close_desktop();
            return 124;
        }
    }
    struct nt_guest_thread guest_thread = {0};
    if (!nt_runtime_enter_guest_thread(&guest_thread, (uintptr_t)active.base, &initial)) {
        nt_runtime_close_desktop();
        return 125;
    }
    nt_runtime_notify_tls(1); /* DLL_PROCESS_ATTACH */
    const void *address = active.base + active.entry_rva;
    int (NT_MS_ABI *entry)(void);
    memcpy(&entry, &address, sizeof(entry));
    const int status = entry();
    nt_runtime_notify_tls(0); /* DLL_PROCESS_DETACH */
    nt_runtime_run_fls_cleanup();
    if (!nt_guest_thread_leave(&guest_thread)) {
        nt_runtime_close_desktop();
        return 127;
    }
    nt_runtime_close_desktop();
    return status;
}

static bool worker_ready(bool child)
{
    struct statfs root;
    if ((child ? getpid() == 1 || getppid() <= 1 : getpid() != 1) ||
        prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1 ||
        statfs("/", &root) != 0 || root.f_type != TMPFS_MAGIC) {
        fputs("Refusing worker outside sandbox; use pe-isolated-run FILE.exe\n", stderr);
        return false;
    }
    /* Windows reports a failed pipe write instead of terminating on SIGPIPE. */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        return false;
    /* Drop setup descriptors before loading guest code. A host directory fd
     * would otherwise retain access beyond the sandbox's filesystem root.
     */
    if (close_range(child ? 5 : 3, UINT_MAX, 0) != 0) {
        perror("Cannot close sandbox setup descriptors");
        return false;
    }
    if (child) {
        struct stat pipe;
        if (fstat(4, &pipe) != 0 || !S_ISFIFO(pipe.st_mode))
            return false;
    }
    return true;
}

static int execute_pe(int fd, const char *dos_path, const char *command,
                      const char *directory, int status_fd)
{
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
    active_dos_path = dos_path;
    active_command = command;
    active_directory = directory;
    uint32_t code = 0;
    const bool ran = nt_runtime_run_fixture(guest_entry, &code);
    pe_image_unload(&active);
    if (!ran) {
        fputs("Guest terminated abnormally or worker failed\n", stderr);
        return 5;
    }
    fprintf(stderr, "Guest exited with code %u\n", code);
    if (status_fd >= 0) {
        (void)write(status_fd, &code, sizeof(code));
        close(status_fd);
    }
    return code <= 255 ? (int)code : 1;
}

static int launch_child(int fd, const char *dos_path, const char *command,
                        const char *directory, pid_t *pid, int *status_fd)
{
    int pipe_fds[2];
    if (pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) != 0)
        return errno;
    int image = fcntl(fd, F_DUPFD_CLOEXEC, 5);
    int error = image < 0 ? errno : 0;
    int status = error ? -1 : fcntl(pipe_fds[1], F_DUPFD_CLOEXEC, 5);
    if (!error && status < 0)
        error = errno;
    if (!error) {
        posix_spawn_file_actions_t actions;
        error = posix_spawn_file_actions_init(&actions);
        if (!error) {
            error = posix_spawn_file_actions_adddup2(&actions, image, 3);
            if (!error)
                error = posix_spawn_file_actions_adddup2(&actions, status, 4);
            if (!error) {
                char *const args[] = {"/runner", "--child", (char *)dos_path,
                                      (char *)command, (char *)directory, NULL};
                error = posix_spawn(pid, "/runner", &actions, NULL, args, environ);
            }
            posix_spawn_file_actions_destroy(&actions);
        }
    }
    if (image >= 0)
        close(image);
    if (status >= 0)
        close(status);
    close(pipe_fds[1]);
    if (error)
        close(pipe_fds[0]);
    else
        *status_fd = pipe_fds[0];
    return error;
}

static int run_worker(const char *name)
{
    if (!worker_ready(false))
        return 125;
    if (strncmp(name, "/game/", 6) != 0 || strchr(name + 6, '/') != NULL ||
        strchr(name + 6, '\\') != NULL || strstr(name + 6, "..") != NULL)
        return 126;
    char dos_path[PATH_MAX], command[PATH_MAX + 3];
    if (snprintf(dos_path, sizeof(dos_path), "C:\\%s", name + 6) >= (int)sizeof(dos_path))
        return 4;
    snprintf(command, sizeof(command), "\"%s\"", dos_path);
    int fd = open(name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        perror("PE open");
        return 2;
    }
    nt_runtime_set_child_launcher(launch_child);
    return execute_pe(fd, dos_path, command, NULL, -1);
}

static bool add_arg(char **args, size_t *count, size_t max, char *item)
{
    if (*count + 1 >= max)
        return false;
    args[(*count)++] = item;
    return true;
}

static int inherit_mount_descriptor(int fd)
{
    if (fd < 0)
        return -1;
    /* Keep host directory handles out of the worker's standard streams. */
    const int inherited = fcntl(fd, F_DUPFD, STDERR_FILENO + 1);
    const int error = errno;
    close(fd);
    errno = error;
    return inherited;
}

static int run_isolated(const char *path, const char *const folders[NT_USER_FOLDER_COUNT])
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

    const char *home = getenv("HOME");
    const int profile = inherit_mount_descriptor(nt_open_guest_profile(getenv("XDG_STATE_HOME"), home));
    if (profile < 0) {
        perror("Cannot prepare guest profile");
        return 2;
    }
    char profile_source[64];
    snprintf(profile_source, sizeof(profile_source), "/proc/self/fd/%d", profile);
    char *args[128];
    size_t n = 0;
#define ADD(x) do { if (!add_arg(args, &n, 128, (char *)(x))) return 2; } while (0)
    ADD("bwrap");
    ADD("--die-with-parent"); ADD("--unshare-all"); ADD("--new-session"); ADD("--as-pid-1");
    ADD("--clearenv"); ADD("--setenv"); ADD("PATH"); ADD("/usr/bin:/bin");
    ADD("--setenv"); ADD("HOME"); ADD("/game");
    ADD("--setenv"); ADD("USERPROFILE"); ADD(NT_GUEST_USER_PROFILE);
    ADD("--setenv"); ADD("APPDATA"); ADD(NT_GUEST_USER_PROFILE "\\AppData\\Roaming");
    ADD("--setenv"); ADD("LOCALAPPDATA"); ADD(NT_GUEST_USER_PROFILE "\\AppData\\Local");
    ADD("--setenv"); ADD("TEMP"); ADD("C:\\Temp");
    ADD("--setenv"); ADD("TMP"); ADD("C:\\Temp");
    ADD("--ro-bind"); ADD("/usr"); ADD("/usr");
    if (access("/lib", F_OK) == 0) { ADD("--ro-bind"); ADD("/lib"); ADD("/lib"); }
    if (access("/lib64", F_OK) == 0) { ADD("--ro-bind"); ADD("/lib64"); ADD("/lib64"); }
    ADD("--ro-bind-try"); ADD("/etc/fonts"); ADD("/etc/fonts");
    ADD("--proc"); ADD("/proc"); ADD("--dev"); ADD("/dev");
    ADD("--tmpfs"); ADD("/tmp");
    ADD("--bind"); ADD(profile_source); ADD("/game");
    ADD("--ro-bind"); ADD(absolute); ADD(inside);
    ADD("--ro-bind"); ADD(runner); ADD("/runner");
    char dxc_directory[PATH_MAX];
    struct stat dxc_info;
    if (nt_dxc_install_path(runner, dxc_directory, sizeof(dxc_directory)) &&
        stat(dxc_directory, &dxc_info) == 0 && S_ISDIR(dxc_info.st_mode)) {
        ADD("--ro-bind"); ADD(dxc_directory); ADD(NT_DXC_SANDBOX_DIRECTORY);
        ADD("--setenv"); ADD("LD_LIBRARY_PATH"); ADD(NT_DXC_SANDBOX_DIRECTORY);
    }
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
    char folder_sources[NT_USER_FOLDER_COUNT][64];
    char folder_targets[NT_USER_FOLDER_COUNT][sizeof(NT_GUEST_FOLDER_MOUNTS) + 32];
    for (size_t i = 0; i < NT_USER_FOLDER_COUNT; ++i) {
        if (folders[i] == NULL)
            continue;
        const int fd = inherit_mount_descriptor(nt_open_shared_folder(folders[i], home));
        if (fd < 0) {
            fprintf(stderr, "Cannot share %s=%s: %s\n", nt_user_folders[i].name, folders[i], strerror(errno));
            return 2;
        }
        snprintf(folder_sources[i], sizeof(folder_sources[i]), "/proc/self/fd/%d", fd);
        snprintf(folder_targets[i], sizeof(folder_targets[i]), "%s/%s", NT_GUEST_FOLDER_MOUNTS, nt_user_folders[i].name);
        ADD("--bind"); ADD(folder_sources[i]); ADD(folder_targets[i]);
    }
    ADD("--"); ADD("/runner"); ADD("--worker"); ADD(inside);
    args[n] = NULL;
#undef ADD
    execv("/usr/bin/bwrap", args);
    const int error = errno;
    close(profile);
    errno = error;
    perror("bubblewrap unavailable; install bubblewrap and enable user namespaces");
    return 127;
}

static void usage(const char *command)
{
    fprintf(stderr, "Usage: %s [--share Folder=/absolute/linux/path]... [--] path/to/program.exe\n"
            "Folders with read/write access: Desktop, Documents, Downloads, Music, Pictures, Videos.\n"
            "Requires bubblewrap + Wayland. For headless smoke tests use SDL_VIDEODRIVER=offscreen.\n",
            command);
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--worker") == 0)
        return run_worker(argv[2]);
    if (argc == 5 && strcmp(argv[1], "--child") == 0)
        return worker_ready(true) ? execute_pe(3, argv[2], argv[3], argv[4], 4) : 125;
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(argv[0]);
        return 0;
    }
    const char *folders[NT_USER_FOLDER_COUNT] = {0};
    int arg = 1;
    while (arg + 1 < argc && strcmp(argv[arg], "--share") == 0) {
        const char *mapping = argv[arg + 1];
        const char *equals = strchr(mapping, '=');
        size_t folder = 0;
        if (equals != NULL && equals[1] == '/') {
            const size_t length = (size_t)(equals - mapping);
            for (; folder < NT_USER_FOLDER_COUNT; ++folder) {
                const char *name = nt_user_folders[folder].name;
                if (strlen(name) == length && memcmp(mapping, name, length) == 0)
                    break;
            }
        } else {
            folder = NT_USER_FOLDER_COUNT;
        }
        if (folder == NT_USER_FOLDER_COUNT || folders[folder] != NULL) {
            fprintf(stderr, "Invalid or duplicate folder mapping: %s\n", mapping);
            usage(argv[0]);
            return 2;
        }
        folders[folder] = equals + 1;
        arg += 2;
    }
    const bool end_options = arg < argc && strcmp(argv[arg], "--") == 0;
    if (end_options)
        ++arg;
    if (arg != argc - 1 || (!end_options && argv[arg][0] == '-')) {
        usage(argv[0]);
        return 2;
    }
    return run_isolated(argv[arg], folders);
}
