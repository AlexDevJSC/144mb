/* SPDX-License-Identifier: MIT */
/* Stage-4 external-thread boundary: raw Linux PID/TID values never become
 * authenticated Win32 HANDLEs or grant rights to an unrelated process.
 * A deliberately forked disposable child is the unregistered target. */
#define _GNU_SOURCE
#include "sync.h"
#include "nt_services.h"
#include "nt_core_information.h"
#include <sched.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static int check(int ok, const char *reason)
{
    if (!ok)
        fprintf(stderr, "external thread boundary: %s\n", reason);
    return !ok;
}

int main(void)
{
    int fd[2];
    if (pipe(fd) != 0)
        return EXIT_FAILURE;
    const pid_t child = fork();
    if (child < 0)
        return EXIT_FAILURE;
    if (child == 0) {
        close(fd[1]);
        char byte;
        const ssize_t read_result = read(fd[0], &byte, 1);
        close(fd[0]);
        _exit(read_result == 1 && byte == 'q' ? 0 : 1);
    }
    close(fd[0]);
    int failures = 0;
    cpu_set_t before = {0}, after = {0};
    failures += check(sched_getaffinity(child, sizeof(before), &before) == 0,
                      "read test child CPU entitlement");
    errno = 0;
    const int nice_before = getpriority(PRIO_PROCESS, (id_t)child);
    failures += check(errno == 0, "read test child native nice");

    nt_SetLastError(0);
    const uintptr_t handle = nt_OpenThread(0x20u, 0, (uint32_t)child);
    failures += check(handle == 0 && nt_GetLastError() == NT_ERROR_INVALID_PARAMETER,
                      "Win32 OpenThread refuses unregistered real Linux child TID");

    uintptr_t output = 0x5151u;
    const struct nt_client_id remote = {
        .unique_process = (uintptr_t)child, .unique_thread = (uintptr_t)child
    };
    failures += check(nt_NtOpenThread(&output, 0x20u, NULL, &remote) ==
                      NT_STATUS_NOT_SUPPORTED && output == 0x5151u,
                      "NtOpenThread refuses foreign process without touching output");

    struct nt_group_affinity wanted = {.mask = 1u, .group = 0};
    nt_SetLastError(0);
    failures += check(!nt_SetThreadGroupAffinity((uintptr_t)child, &wanted, NULL) &&
                      nt_GetLastError() == NT_ERROR_INVALID_HANDLE,
                      "numeric Linux PID is not a Win32 group-affinity HANDLE");
    nt_SetLastError(0);
    failures += check(nt_SetThreadAffinityMask((uintptr_t)child, 1u) == 0 &&
                      nt_GetLastError() == NT_ERROR_INVALID_HANDLE,
                      "numeric Linux PID is not a Win32 legacy-affinity HANDLE");

    failures += check(sched_getaffinity(child, sizeof(after), &after) == 0 &&
                      CPU_EQUAL(&before, &after),
                      "test child cpuset unchanged by all denied operations");
    errno = 0;
    const int nice_after = getpriority(PRIO_PROCESS, (id_t)child);
    failures += check(errno == 0 && nice_after == nice_before,
                      "test child nice unchanged by denied operations");

    failures += check(write(fd[1], "q", 1) == 1, "release disposable child");
    close(fd[1]);
    int status = 0;
    failures += check(waitpid(child, &status, 0) == child &&
                      WIFEXITED(status) && WEXITSTATUS(status) == 0,
                      "child exits cleanly");
    if (!failures)
        puts("PASS external thread boundary: no raw PID/TID authority, native child unchanged");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
