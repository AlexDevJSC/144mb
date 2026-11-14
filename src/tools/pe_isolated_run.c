/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "image.h"
#include "dll_graph.h"
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

#include <dirent.h>
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
static struct pe_dll_graph *active_dll_graph;
static const char *active_dos_path, *active_command, *active_directory;
/* The sealed guest environment is loaded before any child PE instructions
 * execute. It is never installed as the Linux process's environ. */
static char **active_child_environment;
static int active_child_manifest = -1;
static uintptr_t active_child_standard_handles[3];
static const char *requested_drives[26];
enum { EXTRA_UNC_LIMIT = 8 };
static struct { char server[64], share[64]; const char *directory; } requested_unc[EXTRA_UNC_LIMIT];
static size_t requested_unc_count;

static bool valid_share_label(const char *value)
{
    if (!value || !*value || strlen(value) >= 64)
        return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (!( (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
               (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.'))
            return false;
    return true;
}

static bool open_secondary_grants(void)
{
    for (char letter = 'A'; letter <= 'Z'; ++letter) {
        if (letter == 'C')
            continue;
        char directory[48];
        snprintf(directory, sizeof(directory), "/guest-drives/%c", letter);
        if (access(directory, F_OK) == 0 &&
            !nt_runtime_set_drive_root(letter, directory))
            return false;
    }
    DIR *servers = opendir("/guest-unc");
    if (!servers)
        return errno == ENOENT;
    struct dirent *server;
    bool success = true;
    while (success && (server = readdir(servers)) != NULL) {
        if (server->d_name[0] == '.' || !valid_share_label(server->d_name))
            continue;
        char server_directory[192];
        snprintf(server_directory, sizeof(server_directory), "/guest-unc/%s", server->d_name);
        DIR *shares = opendir(server_directory);
        if (!shares) {
            success = false;
            break;
        }
        struct dirent *share;
        while (success && (share = readdir(shares)) != NULL) {
            if (share->d_name[0] == '.' || !valid_share_label(share->d_name))
                continue;
            char path[320];
            snprintf(path, sizeof(path), "%s/%s", server_directory, share->d_name);
            success = nt_runtime_set_unc_root(server->d_name, share->d_name, path);
        }
        closedir(shares);
    }
    closedir(servers);
    return success;
}

static uintptr_t resolve(const char *module, const char *name, uint16_t ordinal, void *context)
{
    const uintptr_t address = pe_dll_graph_resolve(module, name, ordinal, context);
    if (address == 0) {
        if (name != NULL)
            fprintf(stderr, "Unsupported guest import %s!%s\n", module, name);
        else
            fprintf(stderr, "Unsupported guest import %s!#%u\n", module, ordinal);
    }
    return address;
}

static int NT_MS_ABI guest_entry(void)
{
    nt_runtime_set_guest_environment((const char *const *)(active_child_environment ?
                                                              active_child_environment : environ));
    if (!nt_runtime_set_file_root("/game"))
        return 118;
    if (!open_secondary_grants()) {
        perror("Cannot initialize secondary DOS/UNC roots");
        return 118;
    }
    if (active_child_manifest >= 0) {
        const bool restored =
            nt_runtime_inherited_standard_handles(active_child_manifest,
                                                  active_child_standard_handles) &&
            nt_runtime_restore_inherited_files(active_child_manifest);
        close(active_child_manifest);
        active_child_manifest = -1;
        if (!restored) {
            fputs("Refusing invalid inherited HANDLE manifest\n", stderr);
            return 118;
        }
    }
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
    nt_runtime_set_guest_image_entrypoint(active.entry_rva);
    const struct nt_guest_tls initial = {active.base + active.tls.data_rva, active.tls.bytes, active.tls.zero_fill};
    nt_runtime_set_guest_tls(&initial, active.base + active.tls.callbacks_rva, active.tls.callback_count);
    if (!nt_runtime_set_standard_streams(STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO))
        return 121;
    const uint32_t selectors[3] = {(uint32_t)-10, (uint32_t)-11, (uint32_t)-12};
    for (size_t i = 0; i < 3; ++i) {
        if (active_child_standard_handles[i] != 0 &&
            !nt_SetStdHandle(selectors[i], active_child_standard_handles[i]))
            return 121;
    }
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
    if (!pe_dll_graph_attach_all(active_dll_graph)) {
        fputs("Cannot initialize imported DLLs\n", stderr);
        (void)nt_guest_thread_leave(&guest_thread);
        nt_runtime_close_desktop();
        return 128;
    }
    nt_runtime_notify_tls(1); /* DLL_PROCESS_ATTACH */
    const void *address = active.base + active.entry_rva;
    int (NT_MS_ABI *entry)(void);
    memcpy(&entry, &address, sizeof(entry));
    const int status = entry();
    nt_runtime_notify_tls(0); /* DLL_PROCESS_DETACH */
    pe_dll_graph_detach_all(active_dll_graph);
    nt_runtime_run_fls_cleanup();
    if (!nt_guest_thread_leave(&guest_thread)) {
        nt_runtime_close_desktop();
        return 127;
    }
    nt_runtime_close_desktop();
    return status;
}

static bool worker_ready(bool child, bool suspended)
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
    /* The child must retain only authenticated protocol descriptors and
     * the bounded sequence of file HANDLE FDs explicitly listed by its
     * sealed manifest. Everything else is closed before loading guest PE. */
    size_t inherited_files = 0;
    if (child && !nt_runtime_inherited_file_count(7, &inherited_files))
        return false;
    if (child) {
        for (size_t i = 0; i < inherited_files; ++i) {
            struct stat inherited;
            if (fstat(NT_CHILD_FILE_FD_BASE + (int)i, &inherited) != 0)
                return false;
        }
    }
    if ((child ? (close_range(8u, NT_CHILD_FILE_FD_BASE - 1u, 0) != 0 ||
                  close_range(NT_CHILD_FILE_FD_BASE + (unsigned int)inherited_files,
                              UINT_MAX, 0) != 0) :
                 close_range(3u, UINT_MAX, 0) != 0)) {
        perror("Cannot close sandbox setup descriptors");
        return false;
    }
    if (child) {
        struct stat pipe;
        if (fstat(4, &pipe) != 0 || !S_ISFIFO(pipe.st_mode) ||
            (suspended && (fstat(5, &pipe) != 0 || !S_ISFIFO(pipe.st_mode))) ||
            fstat(6, &pipe) != 0 || !S_ISREG(pipe.st_mode))
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
    /* This call is made only after worker_ready confirmed the Linux
     * sandbox; DLLs cannot be read outside the provisioned /game root. */
    active_dll_graph = pe_dll_graph_create("/game");
    if (active_dll_graph == NULL) {
        free(bytes);
        return 126;
    }
    pe_dll_graph_install(active_dll_graph);
    char error[160];
    const bool loaded = pe_image_load(bytes, size, resolve, active_dll_graph,
                                      &active, error, sizeof(error));
    free(bytes);
    if (!loaded) {
        fprintf(stderr, "PE loader: %s\n", error);
        pe_dll_graph_uninstall();
        pe_dll_graph_destroy(active_dll_graph);
        active_dll_graph = NULL;
        return 3;
    }
    active_dos_path = dos_path;
    active_command = command;
    active_directory = directory;
    uint32_t code = 0;
    const bool ran = nt_runtime_run_fixture(guest_entry, &code);
    pe_image_unload(&active);
    pe_dll_graph_uninstall();
    pe_dll_graph_destroy(active_dll_graph);
    active_dll_graph = NULL;
    if (!ran) {
        fputs("Guest terminated abnormally or worker failed\n", stderr);
        return 5;
    }
    fprintf(stderr, "Guest exited with code %u\n", code);
    if (status_fd >= 0) {
        ssize_t written;
        do {
            written = write(status_fd, &code, sizeof(code));
        } while (written < 0 && errno == EINTR);
        close(status_fd);
        if (written != sizeof(code)) {
            fputs("Could not report guest exit code to the parent\n", stderr);
            return 6;
        }
    }
    return code <= 255 ? (int)code : 1;
}

static int launch_child(int fd, const char *dos_path, const char *command,
                        const char *directory, bool suspended, int environment_fd,
                        const struct nt_child_file_transfer *files, pid_t *pid,
                        int *status_fd, int *start_fd)
{
    int pipe_fds[2];
    if (pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) != 0)
        return errno;
    int gate[2] = {-1, -1};
    if (suspended && pipe2(gate, O_CLOEXEC) != 0) {
        const int error = errno;
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return error;
    }
    int image = fcntl(fd, F_DUPFD_CLOEXEC, 7);
    int error = image < 0 ? errno : 0;
    int status = error ? -1 : fcntl(pipe_fds[1], F_DUPFD_CLOEXEC, 7);
    if (!error && status < 0)
        error = errno;
    int start_reader = error || !suspended ? -1 : fcntl(gate[0], F_DUPFD_CLOEXEC, 7);
    if (!error && suspended && start_reader < 0)
        error = errno;
    int environment_reader = error ? -1 : fcntl(environment_fd, F_DUPFD_CLOEXEC, 7);
    if (!error && environment_reader < 0)
        error = errno;
    int manifest_reader = error ? -1 : fcntl(files->manifest_fd, F_DUPFD_CLOEXEC, 128);
    if (!error && manifest_reader < 0)
        error = errno;
    if (!error) {
        posix_spawn_file_actions_t actions;
        error = posix_spawn_file_actions_init(&actions);
        if (!error) {
            error = posix_spawn_file_actions_adddup2(&actions, image, 3);
            if (!error)
                error = posix_spawn_file_actions_adddup2(&actions, status, 4);
            if (!error && suspended)
                error = posix_spawn_file_actions_adddup2(&actions, start_reader, 5);
            if (!error)
                error = posix_spawn_file_actions_adddup2(&actions, environment_reader, 6);
            if (!error)
                error = posix_spawn_file_actions_adddup2(&actions, manifest_reader, 7);
            for (size_t i = 0; !error && i < files->count; ++i)
                error = posix_spawn_file_actions_adddup2(&actions, files->source_fds[i],
                                                         NT_CHILD_FILE_FD_BASE + (int)i);
            if (!error) {
                char *const args[] = {"/runner", suspended ? "--child-suspended" : "--child",
                                      (char *)dos_path, (char *)command, (char *)directory, NULL};
                error = posix_spawn(pid, "/runner", &actions, NULL, args, environ);
            }
            posix_spawn_file_actions_destroy(&actions);
        }
    }
    if (image >= 0)
        close(image);
    if (status >= 0)
        close(status);
    if (start_reader >= 0)
        close(start_reader);
    if (environment_reader >= 0)
        close(environment_reader);
    if (manifest_reader >= 0)
        close(manifest_reader);
    if (gate[0] >= 0)
        close(gate[0]);
    close(pipe_fds[1]);
    if (error) {
        close(pipe_fds[0]);
        if (gate[1] >= 0)
            close(gate[1]);
    } else {
        *status_fd = pipe_fds[0];
        *start_fd = gate[1];
    }
    return error;
}

static int run_worker(const char *name, const char *command)
{
    if (!worker_ready(false, false))
        return 125;
    if (strncmp(name, "/game/", 6) != 0 || strchr(name + 6, '/') != NULL ||
        strchr(name + 6, '\\') != NULL || strstr(name + 6, "..") != NULL)
        return 126;
    char dos_path[PATH_MAX];
    if (snprintf(dos_path, sizeof(dos_path), "C:\\%s", name + 6) >= (int)sizeof(dos_path))
        return 4;
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

static bool append_guest_argument(char **end, const char *limit, const char *value)
{
    char *out = *end;
    if (limit - out < 3)
        return false;
    *out++ = ' ';
    *out++ = '"';
    for (;;) {
        size_t slashes = strspn(value, "\\");
        value += slashes;
        if (*value == '\0' || *value == '"')
            slashes = 2 * slashes + (*value == '"');
        if (slashes >= (size_t)(limit - out))
            return false;
        memset(out, '\\', slashes);
        out += slashes;
        if (*value == '\0')
            break;
        *out++ = *value++;
    }
    *out++ = '"';
    *out = '\0';
    *end = out;
    return true;
}

static int run_isolated(const char *path, const char *const folders[NT_USER_FOLDER_COUNT],
                        int argument_count, char **arguments)
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
    if (slash == NULL || slash[1] == '\0' || strpbrk(slash + 1, "\\\"") ||
        strstr(slash + 1, "..") != NULL) {
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
    char command[32767];
    int length = snprintf(command, sizeof(command), "\"C:\\%s\"", slash + 1);
    if (length < 0 || (size_t)length >= sizeof(command))
        return 2;
    char *end = command + length;
    for (int i = 0; i < argument_count; ++i) {
        if (!append_guest_argument(&end, command + sizeof(command) - 1, arguments[i])) {
            fputs("Guest command line is too long\n", stderr);
            return 2;
        }
    }

    const char *home = getenv("HOME");
    const int profile = inherit_mount_descriptor(nt_open_guest_profile(getenv("XDG_STATE_HOME"), home));
    if (profile < 0) {
        perror("Cannot prepare guest profile");
        return 2;
    }
    char profile_source[64];
    snprintf(profile_source, sizeof(profile_source), "/proc/self/fd/%d", profile);
    char *args[256];
    size_t n = 0;
#define ADD(x) do { if (!add_arg(args, &n, 256, (char *)(x))) return 2; } while (0)
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
    /* Each extra DOS root is mapped directly from its authorized Linux
     * descriptor. The guest sees only its dedicated /guest-drives or
     * /guest-unc mount, never the original host directory pathname. */
    char drive_sources[26][64], drive_targets[26][48];
    for (size_t i = 0; i < 26; ++i) {
        if (requested_drives[i] == NULL)
            continue;
        const int fd = inherit_mount_descriptor(nt_open_shared_folder(requested_drives[i], home));
        if (fd < 0) {
            fprintf(stderr, "Cannot authorize drive %c: %s\n", (int)('A' + i), strerror(errno));
            return 2;
        }
        snprintf(drive_sources[i], sizeof(drive_sources[i]), "/proc/self/fd/%d", fd);
        snprintf(drive_targets[i], sizeof(drive_targets[i]), "/guest-drives/%c", (int)('A' + i));
        ADD("--bind"); ADD(drive_sources[i]); ADD(drive_targets[i]);
    }
    char unc_sources[EXTRA_UNC_LIMIT][64], unc_targets[EXTRA_UNC_LIMIT][320];
    for (size_t i = 0; i < requested_unc_count; ++i) {
        const int fd = inherit_mount_descriptor(nt_open_shared_folder(requested_unc[i].directory, home));
        if (fd < 0) {
            fprintf(stderr, "Cannot authorize UNC %s/%s: %s\n",
                    requested_unc[i].server, requested_unc[i].share, strerror(errno));
            return 2;
        }
        snprintf(unc_sources[i], sizeof(unc_sources[i]), "/proc/self/fd/%d", fd);
        snprintf(unc_targets[i], sizeof(unc_targets[i]), "/guest-unc/%s/%s",
                 requested_unc[i].server, requested_unc[i].share);
        ADD("--bind"); ADD(unc_sources[i]); ADD(unc_targets[i]);
    }
    ADD("--"); ADD("/runner"); ADD("--worker"); ADD(inside); ADD(command);
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
    fprintf(stderr,
            "Usage: %s [--share Folder=/absolute/linux/path]... [--drive D=/absolute/linux/path]\n"
            "          [--unc server/share=/absolute/linux/path]... [--] program.exe [arguments...]\n"
            "       %s --remember-share Folder=/absolute/linux/path\n"
            "       %s --forget-share Folder\n"
            "Mappings: Desktop, Documents, Downloads, Music, Pictures, Videos.\n"
            "Remembered grants are stored in private $XDG_CONFIG_HOME/nt-native-linux/shares.conf\n"
            "(or $HOME/.config/nt-native-linux/shares.conf) and apply to every launch.\n"
            "A per-launch --share overrides a remembered mapping for that folder.\n"
            "--drive and --unc are explicit per-launch Linux directory grants, never automatic mounts.\n"
            "Requires bubblewrap + Wayland. For headless smoke tests use SDL_VIDEODRIVER=offscreen.\n",
            command, command, command);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--worker") == 0)
        return argc == 4 ? run_worker(argv[2], argv[3]) : 125;
    if (argc == 5 &&
        (strcmp(argv[1], "--child") == 0 || strcmp(argv[1], "--child-suspended") == 0)) {
        const bool suspended = strcmp(argv[1], "--child-suspended") == 0;
        if (!worker_ready(true, suspended))
            return 125;
        if (suspended) {
            /* A trusted parent must release the startup gate. EOF must
             * not accidentally execute the guest, even if the parent dies. */
            unsigned char token = 0;
            ssize_t count;
            do {
                count = read(5, &token, 1);
            } while (count < 0 && errno == EINTR);
            close(5);
            if (count != 1 || token != 'G')
                return 125;
        }
        char **entries = NULL;
        char *storage = NULL;
        const bool valid = nt_runtime_read_child_environment(6, &entries, &storage);
        close(6);
        if (!valid) {
            fputs("Refusing unsealed or invalid child environment snapshot\n", stderr);
            return 125;
        }
        active_child_environment = entries;
        active_child_manifest = 7;
        const int result = execute_pe(3, argv[2], argv[3], argv[4], 4);
        if (active_child_manifest >= 0)
            close(active_child_manifest);
        active_child_manifest = -1;
        active_child_environment = NULL;
        free(entries);
        free(storage);
        return result;
    }
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(argv[0]);
        return 0;
    }
    const char *names[NT_USER_FOLDER_COUNT];
    for (size_t i = 0; i < NT_USER_FOLDER_COUNT; ++i)
        names[i] = nt_user_folders[i].name;
    const char *home = getenv("HOME");
    const char *config_home = getenv("XDG_CONFIG_HOME");
    if (argc >= 2 && (strcmp(argv[1], "--remember-share") == 0 ||
                      strcmp(argv[1], "--forget-share") == 0)) {
        const bool remember = strcmp(argv[1], "--remember-share") == 0;
        if (argc != 3) {
            usage(argv[0]);
            return 2;
        }
        const char *folder = argv[2];
        const char *directory = NULL;
        char name[64];
        if (remember) {
            const char *equals = strchr(argv[2], '=');
            const size_t length = equals != NULL ? (size_t)(equals - argv[2]) : 0;
            if (length == 0 || length >= sizeof(name) || equals[1] != '/') {
                usage(argv[0]);
                return 2;
            }
            memcpy(name, folder, length);
            name[length] = '\0';
            folder = name;
            directory = equals + 1;
        }
        if (nt_guest_share_update(config_home, home, names, NT_USER_FOLDER_COUNT,
                                  folder, directory) != 0) {
            fprintf(stderr, "Cannot update native folder grant: %s\n", strerror(errno));
            return 2;
        }
        fprintf(stderr, "%s native folder grant: %s\n", remember ? "Remembered" : "Removed",
                folder);
        return 0;
    }

    const char *folders[NT_USER_FOLDER_COUNT] = {0};
    int arg = 1;
    while (arg + 1 < argc) {
        const char *option = argv[arg], *mapping = argv[arg + 1];
        const char *equals = strchr(mapping, '=');
        if (strcmp(option, "--share") == 0) {
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
        } else if (strcmp(option, "--drive") == 0) {
            char letter = mapping[0];
            if (letter >= 'a' && letter <= 'z')
                letter -= 'a' - 'A';
            if (!equals || equals != mapping + 1 || equals[1] != '/' ||
                letter < 'A' || letter > 'Z' || letter == 'C' ||
                requested_drives[letter - 'A'] != NULL) {
                fprintf(stderr, "Invalid or duplicate drive grant: %s\n", mapping);
                return 2;
            }
            requested_drives[letter - 'A'] = equals + 1;
        } else if (strcmp(option, "--unc") == 0) {
            const char *slash = strchr(mapping, '/');
            char server[64], share[64];
            const size_t server_length = slash ? (size_t)(slash - mapping) : 0;
            const size_t share_length = equals && slash && equals > slash ?
                                        (size_t)(equals - slash - 1) : 0;
            if (!equals || equals[1] != '/' || server_length == 0 ||
                server_length >= sizeof(server) || share_length == 0 ||
                share_length >= sizeof(share) || requested_unc_count == EXTRA_UNC_LIMIT ||
                memchr(slash + 1, '/', share_length) != NULL) {
                fprintf(stderr, "Invalid UNC grant: %s\n", mapping);
                return 2;
            }
            memcpy(server, mapping, server_length);
            server[server_length] = '\0';
            memcpy(share, slash + 1, share_length);
            share[share_length] = '\0';
            if (!valid_share_label(server) || !valid_share_label(share) ||
                server[0] == '.' || share[0] == '.') {
                fprintf(stderr, "Invalid UNC server/share: %s\n", mapping);
                return 2;
            }
            for (size_t i = 0; i < requested_unc_count; ++i)
                if (strcasecmp(requested_unc[i].server, server) == 0 &&
                    strcasecmp(requested_unc[i].share, share) == 0) {
                    fprintf(stderr, "Duplicate UNC grant: %s\n", mapping);
                    return 2;
                }
            strcpy(requested_unc[requested_unc_count].server, server);
            strcpy(requested_unc[requested_unc_count].share, share);
            requested_unc[requested_unc_count++].directory = equals + 1;
        } else {
            break;
        }
        arg += 2;
    }
    const bool end_options = arg < argc && strcmp(argv[arg], "--") == 0;
    if (end_options)
        ++arg;
    if (arg >= argc || (!end_options && argv[arg][0] == '-')) {
        usage(argv[0]);
        return 2;
    }
    char *remembered[NT_USER_FOLDER_COUNT] = {0};
    if (nt_guest_share_load(config_home, home, names, NT_USER_FOLDER_COUNT, remembered) != 0) {
        fprintf(stderr, "Cannot load native folder grants: %s\n", strerror(errno));
        return 2;
    }
    for (size_t i = 0; i < NT_USER_FOLDER_COUNT; ++i)
        if (folders[i] == NULL)
            folders[i] = remembered[i];
    const int result = run_isolated(argv[arg], folders, argc - arg - 1, argv + arg + 1);
    nt_guest_share_free(remembered, NT_USER_FOLDER_COUNT);
    return result;
}
