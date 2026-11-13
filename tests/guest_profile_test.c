/* SPDX-License-Identifier: MIT */
#define _XOPEN_SOURCE 700
#include "guest_profile.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(test) do { \
    if (!(test)) { \
        fprintf(stderr, "FAIL line %d %s errno=%d\n", __LINE__, #test, errno); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static int remove_entry(const char *path, const struct stat *info, int type, struct FTW *walk)
{
    (void)info;
    (void)type;
    (void)walk;
    return remove(path);
}

int main(void)
{
    char root[] = "/tmp/nt-profile-test-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char state[512], other[512];
    CHECK(snprintf(state, sizeof(state), "%s/state", root) > 0);
    CHECK(snprintf(other, sizeof(other), "%s/other", root) > 0 && mkdir(other, 0700) == 0);
    int shared = nt_open_shared_folder(other, root);
    CHECK(shared >= 0 && (fcntl(shared, F_GETFD) & FD_CLOEXEC) != 0 && close(shared) == 0);
    CHECK(nt_open_shared_folder(root, root) < 0 && errno == EACCES);
    CHECK(nt_open_shared_folder(root, other) < 0 && errno == EACCES);
    CHECK(nt_open_shared_folder("/", root) < 0 && errno == EACCES);
    CHECK(nt_open_shared_folder("other", root) < 0 && errno == EINVAL);
    CHECK(nt_open_guest_profile("relative", NULL) < 0 && errno == EINVAL);
    int drive = nt_open_guest_profile(state, NULL);
    CHECK(drive >= 0);
    struct stat info;
    CHECK(fstat(drive, &info) == 0 && (info.st_mode & 0777) == 0700);
    /* C: is literally a Linux directory; names are only a folder skeleton. */
    char physical_c[512], profile_base[512];
    CHECK(snprintf(physical_c, sizeof(physical_c),
                   "%s/nt-native-linux/C:", state) > 0);
    CHECK(snprintf(profile_base, sizeof(profile_base),
                   "%s/nt-native-linux", state) > 0);
    CHECK(stat(physical_c, &info) == 0 && S_ISDIR(info.st_mode));
    CHECK(fstatat(drive, "Windows/System32", &info, AT_SYMLINK_NOFOLLOW) == 0 &&
          S_ISDIR(info.st_mode));
    CHECK(fstatat(drive, "Windows/SysWOW64", &info, AT_SYMLINK_NOFOLLOW) == 0 &&
          S_ISDIR(info.st_mode));
    CHECK(fstatat(drive, "Program Files", &info, AT_SYMLINK_NOFOLLOW) == 0 &&
          S_ISDIR(info.st_mode));
    CHECK(fstatat(drive, "Program Files (x86)", &info, AT_SYMLINK_NOFOLLOW) == 0 &&
          S_ISDIR(info.st_mode));
    CHECK(fstatat(drive, "ProgramData", &info, AT_SYMLINK_NOFOLLOW) == 0 &&
          S_ISDIR(info.st_mode));
    CHECK(fstatat(drive, "Users/guest/Documents", &info, AT_SYMLINK_NOFOLLOW) == 0 &&
          S_ISDIR(info.st_mode));
    CHECK(fstatat(drive, "Users/guest/AppData/Roaming", &info, AT_SYMLINK_NOFOLLOW) == 0 &&
          S_ISDIR(info.st_mode));
    CHECK(mkdirat(drive, "Program Files/Demo App", 0700) == 0);
    int install = openat(drive, "Program Files/Demo App/settings.ini",
                         O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    CHECK(install >= 0 && write(install, "fedora", 6) == 6 && close(install) == 0);

    const char *file = "Users/guest/AppData/Roaming/preferences.dat";
    int config = openat(drive, file, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    CHECK(config >= 0 && write(config, "settings", 8) == 8 && close(config) == 0);
    CHECK(close(drive) == 0);
    /* Atomically move a populated older profile, never copy its files. */
    int profile_dir = open(profile_base, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(profile_dir >= 0);
    CHECK(renameat(profile_dir, "C:", profile_dir, "drive-c") == 0);
    CHECK(fstatat(profile_dir, "C:", &info, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT);
    drive = nt_open_guest_profile(state, NULL);
    CHECK(drive >= 0);
    CHECK(fstatat(profile_dir, "drive-c", &info, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT);
    CHECK(stat(physical_c, &info) == 0 && S_ISDIR(info.st_mode));
    int migrated = openat(drive, "Program Files/Demo App/settings.ini", O_RDONLY | O_CLOEXEC);
    char original[6];
    CHECK(migrated >= 0 && read(migrated, original, sizeof(original)) == 6 &&
          memcmp(original, "fedora", 6) == 0 && close(migrated) == 0);
    /* In case of conflicting roots, refuse to choose and risk data loss. */
    CHECK(mkdirat(profile_dir, "drive-c", 0700) == 0);
    CHECK(nt_open_guest_profile(state, NULL) < 0 && errno == EEXIST);
    CHECK(unlinkat(profile_dir, "drive-c", AT_REMOVEDIR) == 0);
    CHECK(close(profile_dir) == 0);
    CHECK(drive >= 0);
    config = openat(drive, file, O_RDONLY | O_CLOEXEC);
    char saved[8];
    CHECK(config >= 0 && read(config, saved, sizeof(saved)) == 8 &&
          memcmp(saved, "settings", 8) == 0 && close(config) == 0);

    pid_t children[4];
    for (size_t i = 0; i < sizeof(children) / sizeof(children[0]); ++i) {
        children[i] = fork();
        CHECK(children[i] >= 0);
        if (children[i] == 0) {
            const int opened = nt_open_guest_profile(state, NULL);
            if (opened >= 0)
                close(opened);
            _exit(opened >= 0 ? 0 : 1);
        }
    }
    for (size_t i = 0; i < sizeof(children) / sizeof(children[0]); ++i) {
        int status;
        CHECK(waitpid(children[i], &status, 0) == children[i] &&
              WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    CHECK(renameat(drive, "Users", drive, "Users.saved") == 0);
    CHECK(symlinkat(other, drive, "Users") == 0);
    CHECK(nt_open_guest_profile(state, NULL) < 0);
    CHECK(unlinkat(drive, "Users", 0) == 0 && renameat(drive, "Users.saved", drive, "Users") == 0);
    CHECK(fchmod(drive, 0755) == 0);
    CHECK(nt_open_guest_profile(state, NULL) < 0 && errno == EACCES);
    CHECK(fchmod(drive, 0700) == 0 && close(drive) == 0);
    drive = nt_open_guest_profile("relative", root);
    CHECK(drive >= 0 && close(drive) == 0);

    /* Per-user native directory grants survive a new process/launch. */
    const char *const names[] = {"Desktop", "Documents", "Downloads",
                                  "Music", "Pictures", "Videos"};
    char *paths[6] = {0};
    CHECK(nt_guest_share_load(NULL, root, names, 6, paths) == 0);
    for (size_t i = 0; i < 6; ++i)
        CHECK(paths[i] == NULL);
    CHECK(nt_guest_share_update(NULL, root, names, 6, "Documents", other) == 0);
    CHECK(nt_guest_share_load(NULL, root, names, 6, paths) == 0);
    CHECK(paths[1] != NULL && strcmp(paths[1], other) == 0);
    nt_guest_share_free(paths, 6);

    /* The granted native directory contains the actual file, not a copy. */
    shared = nt_open_shared_folder(other, root);
    CHECK(shared >= 0);
    int native_file = openat(shared, "fedora-native.txt", O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(native_file >= 0 && write(native_file, "native", 6) == 6);
    CHECK(close(native_file) == 0 && close(shared) == 0);
    char native_path[512];
    CHECK(snprintf(native_path, sizeof(native_path), "%s/fedora-native.txt", other) > 0);
    CHECK(stat(native_path, &info) == 0 && info.st_size == 6);

    /* Concurrent updates must preserve independent grants. */
    for (size_t i = 0; i < 2; ++i) {
        children[i] = fork();
        CHECK(children[i] >= 0);
        if (children[i] == 0)
            _exit(nt_guest_share_update(NULL, root, names, 6,
                    i == 0 ? "Desktop" : "Pictures", other) == 0 ? 0 : 1);
    }
    for (size_t i = 0; i < 2; ++i) {
        int status;
        CHECK(waitpid(children[i], &status, 0) == children[i] &&
              WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    CHECK(nt_guest_share_load(NULL, root, names, 6, paths) == 0);
    CHECK(paths[0] != NULL && paths[1] != NULL && paths[4] != NULL);
    nt_guest_share_free(paths, 6);

    CHECK(nt_guest_share_update(NULL, root, names, 6, "Documents", NULL) == 0);
    CHECK(nt_guest_share_load(NULL, root, names, 6, paths) == 0);
    CHECK(paths[1] == NULL && paths[0] != NULL && paths[4] != NULL);
    nt_guest_share_free(paths, 6);

    CHECK(nt_guest_share_update(NULL, root, names, 6, "Documents", root) < 0 &&
          errno == EACCES);
    CHECK(nt_guest_share_update(NULL, root, names, 6, "Documents", "relative") < 0 &&
          errno == EINVAL);
    CHECK(nt_guest_share_update(NULL, root, names, 6, "InvalidFolder", other) < 0 &&
          errno == EINVAL);

    char config_path[512];
    CHECK(snprintf(config_path, sizeof(config_path),
                   "%s/.config/nt-native-linux/shares.conf", root) > 0);
    CHECK(stat(config_path, &info) == 0 && S_ISREG(info.st_mode) &&
          (info.st_mode & 0077) == 0 && info.st_uid == geteuid());
    CHECK(chmod(config_path, 0644) == 0);
    CHECK(nt_guest_share_load(NULL, root, names, 6, paths) < 0 && errno == EACCES);
    CHECK(chmod(config_path, 0600) == 0);
    CHECK(unlink(config_path) == 0);
    CHECK(symlink(native_path, config_path) == 0);
    CHECK(nt_guest_share_load(NULL, root, names, 6, paths) < 0);
    CHECK(unlink(config_path) == 0);

    CHECK(nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
    puts("PASS persistent profile, native folder grants, atomic concurrent updates and symlink denial");
    return 0;
}
