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
#include <string.h>
#include <fcntl.h>
#include <spawn.h>
#include <errno.h>
extern char **environ;
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static uint64_t filetime_value(const struct nt_file_time *stamp)
{
    return ((uint64_t)stamp->high << 32) | stamp->low;
}

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "process/thread regression: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static int environment_child(void)
{
    char **entries = NULL;
    char *storage = NULL;
    if (!nt_runtime_read_child_environment(6, &entries, &storage))
        return 11;
    close(6);
    if (getenv("NT_CI_TEST") != NULL)
        return 12; /* Host envp must not have been polluted. */
    nt_runtime_set_guest_environment((const char *const *)entries);
    uint16_t received[32] = {0};
    if (nt_GetEnvironmentVariableW(u"NT_CI_TEST", received, 32) != 4 ||
        memcmp(received, u"niño", sizeof(u"niño")) != 0 ||
        nt_GetEnvironmentVariableW(u"LD_PRELOAD", received, 32) != 5 ||
        memcmp(received, u"inert", sizeof(u"inert")) != 0)
        return 13;
    free(entries);
    free(storage);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--env-child") == 0)
        return environment_child();
    /* An immutable UTF-16 snapshot must round-trip Unicode without
     * touching the host environment (including dangerous LD_* names). */
    static const uint16_t custom[] = u"NT_CI_TEST=niño\0LD_PRELOAD=inert\0";
    int environment_fd = -1;
    check(nt_runtime_build_child_environment(custom, &environment_fd) ==
          NT_ERROR_SUCCESS && environment_fd >= 0, "build sealed guest environment");
    char **entries = NULL;
    char *storage = NULL;
    check(nt_runtime_read_child_environment(environment_fd, &entries, &storage) &&
          entries != NULL && storage != NULL && entries[0] != NULL &&
          strcmp(entries[0], "NT_CI_TEST=niño") == 0 &&
          entries[1] != NULL && strcmp(entries[1], "LD_PRELOAD=inert") == 0 &&
          entries[2] == NULL, "read sealed Unicode environment");
    free(entries);
    free(storage);
    /* Cross a real exec boundary with the sealed descriptor, preserving
     * the sanitized host envp while mounting the guest-only variables. */
    int send_fd = fcntl(environment_fd, F_DUPFD_CLOEXEC, 16);
    check(send_fd >= 16, "duplicate sealed environment outside reserved fd6");
    posix_spawn_file_actions_t actions;
    check(posix_spawn_file_actions_init(&actions) == 0 &&
          posix_spawn_file_actions_adddup2(&actions, send_fd, 6) == 0,
          "install sealed descriptor into child fd6");
    char *const args[] = {argv[0], "--env-child", NULL};
    pid_t env_child = -1;
    check(posix_spawn(&env_child, argv[0], &actions, NULL, args, environ) == 0,
          "exec child with sealed guest-only environment");
    posix_spawn_file_actions_destroy(&actions);
    close(send_fd);
    int env_status = 0;
    check(waitpid(env_child, &env_status, 0) == env_child &&
          WIFEXITED(env_status) && WEXITSTATUS(env_status) == 0,
          "child reconstructed Unicode environment without altering host envp");
    close(environment_fd);
    static const uint16_t bad[] = u"MISSING_EQUALS\0";
    check(nt_runtime_build_child_environment(bad, &environment_fd) ==
          NT_ERROR_INVALID_PARAMETER, "reject invalid environment record");
    const uint16_t empty[] = {0, 0};
    check(nt_runtime_build_child_environment(empty, &environment_fd) ==
          NT_ERROR_SUCCESS && environment_fd >= 0 &&
          nt_runtime_read_child_environment(environment_fd, &entries, &storage) &&
          entries[0] == NULL, "round-trip empty environment block");
    free(entries);
    free(storage);
    close(environment_fd);
    /* Windows ANSI means guest ACP=1252, not the host Linux locale.
     * The final double NUL and each entry's name/value must survive. */
    static const char cp1252_env[] = "NT_CP1252=Espa\xf1a\0NT_ACCENT=caf\xe9\0";
    uint16_t *converted_cp = nt_runtime_ansi_child_environment(cp1252_env);
    check(converted_cp != NULL &&
          memcmp(converted_cp, u"NT_CP1252=España", sizeof(u"NT_CP1252=España")) == 0 &&
          memcmp(converted_cp + sizeof(u"NT_CP1252=España") / sizeof(uint16_t),
                 u"NT_ACCENT=café", sizeof(u"NT_ACCENT=café")) == 0,
          "convert CP1252 ANSI multi-SZ to exact UTF16 entries");
    check(nt_runtime_build_child_environment(converted_cp, &environment_fd) ==
          NT_ERROR_SUCCESS &&
          nt_runtime_read_child_environment(environment_fd, &entries, &storage) &&
          strcmp(entries[0], "NT_CP1252=España") == 0 &&
          strcmp(entries[1], "NT_ACCENT=café") == 0 &&
          entries[2] == NULL,
          "sealed child environment represents CP1252 input as UTF8");
    free(converted_cp);
    free(entries);
    free(storage);
    close(environment_fd);
    const char empty_ansi[] = {0, 0};
    converted_cp = nt_runtime_ansi_child_environment(empty_ansi);
    check(converted_cp != NULL && converted_cp[0] == 0 && converted_cp[1] == 0,
          "empty ANSI multi-SZ is double-terminated");
    free(converted_cp);
    static const char broken_ansi[] = "BROKEN_RECORD\0";
    converted_cp = nt_runtime_ansi_child_environment(broken_ansi);
    check(converted_cp != NULL &&
          nt_runtime_build_child_environment(converted_cp, &environment_fd) ==
          NT_ERROR_INVALID_PARAMETER,
          "reject malformed converted environment entry without equals sign");
    free(converted_cp);

    int unsealed[2];
    check(pipe(unsealed) == 0, "unsealed test pipe");
    check(!nt_runtime_read_child_environment(unsealed[0], &entries, &storage),
          "reject non-sealed environment descriptor");
    close(unsealed[0]);
    close(unsealed[1]);

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
    struct nt_file_time created = {0}, exited = {0}, kernel = {0}, user = {0};
    check(nt_GetProcessTimes(info.process, &created, &exited, &kernel, &user) &&
          filetime_value(&created) != 0 && filetime_value(&exited) == 0,
          "running child process creation and CPU times");
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
    check(!nt_GetProcessTimes(restricted, NULL, &exited, &kernel, &user) &&
          nt_GetLastError() == NT_ERROR_INVALID_PARAMETER,
          "reject incomplete GetProcessTimes outputs");

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
    check(nt_GetProcessTimes(info.process, &created, &exited, &kernel, &user) &&
          filetime_value(&exited) >= filetime_value(&created) &&
          filetime_value(&exited) != 0,
          "terminated child process exit and wait4 rusage");
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
