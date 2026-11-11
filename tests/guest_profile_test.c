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
    const char *file = "Users/guest/AppData/Roaming/preferences.dat";
    int config = openat(drive, file, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    CHECK(config >= 0 && write(config, "settings", 8) == 8 && close(config) == 0);
    CHECK(close(drive) == 0);
    drive = nt_open_guest_profile(state, NULL);
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
    CHECK(nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
    puts("PASS persistent user profile, XDG fallback, concurrent opens and explicit shared directories");
    return 0;
}
