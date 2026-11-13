/* SPDX-License-Identifier: MIT */
/* Linux-side end-to-end process lifetime test with real owned child PIDs.
 * It never terminates a process that was not forked by this test. */
#define _GNU_SOURCE
#include "child_process.h"
#include "nt_services.h"
#include "nt_core_information.h"
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
    check(nt_runtime_watch_child(child, channel[0], -1, &info), "watch child");
    check(info.process && info.thread && info.process_id == (uint32_t)child,
          "child handles and ID");
    check(nt_GetProcessId(UINTPTR_MAX) == (uint32_t)getpid(), "current process ID");
    uintptr_t self = nt_OpenProcess(0x101000u, 0, (uint32_t)getpid());
    check(self != 0 && nt_GetProcessId(self) == (uint32_t)getpid(),
          "OpenProcess(current PID) returns real alias");
    uintptr_t wow64 = 1;
    uint32_t length = 0;
    check(nt_NtQueryInformationProcess(self, 26, &wow64, sizeof(wow64), &length) ==
          NT_STATUS_SUCCESS && wow64 == 0 && length == sizeof(wow64),
          "native process query accepts real self handle");
    nt_SetLastError(0);
    check(!nt_TerminateProcess(self, 44) &&
          nt_GetLastError() == NT_ERROR_ACCESS_DENIED,
          "self alias without terminate right");
    check(nt_runtime_close_waitable(self), "release self process alias");
    check(nt_GetProcessId(info.process) == (uint32_t)child, "child process ID");
    check(nt_GetProcessIdOfThread(info.thread) == (uint32_t)child, "child thread owner");
    uintptr_t restricted = 0;
    check(nt_runtime_duplicate_waitable(info.process, 0x1000u, 0, 0, &restricted) ==
          NT_STATUS_SUCCESS && restricted != 0, "restricted process alias");
    check(nt_GetProcessId(restricted) == (uint32_t)child, "query alias ID");
    uintptr_t by_pid = nt_OpenProcess(0x101000u, 0, (uint32_t)child);
    check(by_pid && by_pid != info.process && nt_GetProcessId(by_pid) == (uint32_t)child,
          "reopen child process by PID with SYNCHRONIZE and query");
    struct nt_client_id ids = {(uintptr_t)child, 0};
    struct nt_object_attributes attrs = {.size = sizeof(attrs)};
    uintptr_t native = 0;
    nt_SetLastError(0x717u);
    check(nt_NtOpenProcess(&native, 0x101000u, &attrs, &ids) == NT_STATUS_SUCCESS &&
          native && native != by_pid && nt_GetLastError() == 0x717u,
          "native NtOpenProcess preserves LastError");
    check(nt_runtime_terminate_child_process(native, 12) == NT_STATUS_ACCESS_DENIED,
          "native restricted process cannot terminate child");
    check(nt_runtime_close_waitable(native) && nt_runtime_close_waitable(by_pid),
          "close reopened child handles");

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

    /* A separate fork models the trusted runner's pre-entry fd5 gate. */
    int gate[2], exit_pipe[2];
    check(pipe(gate) == 0 && pipe(exit_pipe) == 0, "child startup pipes");
    pid_t gated = fork();
    check(gated >= 0, "fork gated child");
    if (gated == 0) {
        close(gate[1]);
        close(exit_pipe[0]);
        alarm(15);
        unsigned char token = 0;
        ssize_t bytes = read(gate[0], &token, 1);
        close(gate[0]);
        if (bytes != 1 || token != 'G')
            _Exit(41);
        const uint32_t result = UINT32_C(0x55667788);
        if (write(exit_pipe[1], &result, sizeof(result)) != sizeof(result))
            _Exit(42);
        close(exit_pipe[1]);
        _Exit(0);
    }
    close(gate[0]);
    close(exit_pipe[1]);
    info = (struct nt_process_information){0};
    check(nt_runtime_watch_child(gated, exit_pipe[0], gate[1], &info),
          "watch gated child");
    check(nt_WaitForSingleObject(info.process, 0) == NT_STATUS_TIMEOUT,
          "suspended child cannot exit");
    uint32_t previous = 0;
    check(nt_NtSuspendThread(info.thread, &previous) == NT_STATUS_SUCCESS &&
          previous == 1, "increment child initial suspension");
    check(nt_NtResumeThread(info.thread, &previous) == NT_STATUS_SUCCESS &&
          previous == 2, "decrement without releasing child");
    check(nt_WaitForSingleObject(info.process, 0) == NT_STATUS_TIMEOUT,
          "child still blocked");
    check(nt_NtResumeThread(info.thread, &previous) == NT_STATUS_SUCCESS &&
          previous == 1, "release final child gate");
    check(nt_WaitForSingleObject(info.process, 5000) == 0, "gated child finished");
    uint32_t code = 0;
    check(nt_GetExitCodeProcess(info.process, &code) && code == 0x55667788,
          "child exit after resume");
    check(nt_runtime_close_waitable(info.thread) &&
          nt_runtime_close_waitable(info.process), "close resumed child handles");
    return EXIT_SUCCESS;
}
