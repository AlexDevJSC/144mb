/* SPDX-License-Identifier: MIT */
/* Linux-side end-to-end process lifetime test with real owned child PIDs.
 * It never terminates a process that was not forked by this test. */
#define _GNU_SOURCE
#include "child_process.h"
#include "nt_services.h"
#include "sync.h"
#include "winbase.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "process/thread regression: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

int main(void)
{
    int channel[2];
    check(pipe(channel) == 0, "pipe");
    pid_t child = fork();
    check(child >= 0, "fork");
    if (child == 0) {
        close(channel[0]);
        close(channel[1]);
        alarm(15);
        for (;;)
            pause();
    }
    close(channel[1]);
    struct nt_process_information info = {0};
    check(nt_runtime_watch_child(child, channel[0], &info), "watch child");
    check(info.process && info.thread && info.process_id == (uint32_t)child,
          "child handles and ID");
    check(nt_GetProcessId(UINTPTR_MAX) == (uint32_t)getpid(), "current process ID");
    check(nt_GetProcessId(info.process) == (uint32_t)child, "child process ID");
    check(nt_GetProcessIdOfThread(info.thread) == (uint32_t)child, "child thread owner");
    uintptr_t restricted = 0;
    check(nt_runtime_duplicate_waitable(info.process, 0x1000u, 0, 0, &restricted) ==
          NT_STATUS_SUCCESS && restricted != 0, "restricted process alias");
    check(nt_GetProcessId(restricted) == (uint32_t)child, "query alias ID");
    nt_SetLastError(0);
    check(!nt_TerminateProcess(restricted, 11) &&
          nt_GetLastError() == NT_ERROR_ACCESS_DENIED, "deny termination via query-only handle");
    nt_SetLastError(0);
    check(!nt_TerminateProcess(0x111u, 12) &&
          nt_GetLastError() == NT_ERROR_INVALID_HANDLE, "deny invalid process handle");
    check(nt_GetExitCodeProcess(info.process, &(uint32_t){0}), "initial exit query");

    nt_SetLastError(0x777u);
    nt_status status = nt_NtTerminateProcess(info.process, (nt_status)UINT32_C(0x12345678));
    if (status == NT_STATUS_NOT_SUPPORTED) {
        /* pidfd is a hard prerequisite for safe targeted termination:
         * do not replace it with kill(pid) vulnerable to PID reuse. */
        printf("SKIP child termination: host does not expose pidfd\n");
        kill(child, SIGKILL);
    } else {
        check(status == NT_STATUS_SUCCESS, "native termination");
        check(nt_GetLastError() == 0x777u, "NT does not change LastError");
    }
    check(nt_WaitForSingleObject(info.process, 5000) == 0, "wait on child");
    check(nt_WaitForSingleObject(info.thread, 0) == 0, "wait on child primary thread");
    if (status == NT_STATUS_SUCCESS) {
        uint32_t code = 0;
        check(nt_GetExitCodeProcess(info.process, &code) && code == 0x12345678u,
              "requested Windows 32-bit exit code");
        check(nt_GetExitCodeThread(info.thread, &code) && code == 0x12345678u,
              "primary thread exit code");
        check(nt_runtime_terminate_child_process(info.process, 4) == NT_STATUS_ACCESS_DENIED,
              "deny already terminated child");
    }
    check(nt_runtime_close_waitable(restricted), "close restricted alias");
    check(nt_runtime_close_waitable(info.thread), "close child thread");
    check(nt_runtime_close_waitable(info.process), "close child process");
    return EXIT_SUCCESS;
}
