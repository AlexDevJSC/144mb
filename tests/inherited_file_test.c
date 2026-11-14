/* SPDX-License-Identifier: MIT */
/* Native end-to-end regression for NT file HANDLE inheritance.
 * The test deliberately crosses a real posix_spawn/exec boundary, preserves
 * an open-file description and proves host descriptors are not guest HANDLEs. */
#define _GNU_SOURCE
#include "compat.h"
#include "sync.h"
#include "winfile.h"
#include "child_process.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static void check(bool okay, const char *step)
{
    if (!okay) {
        fprintf(stderr, "inherited file regression: %s\n", step);
        exit(1);
    }
}

static int run_child(bool should_inherit, uintptr_t value)
{
    size_t count = 500;
    if (!nt_runtime_inherited_file_count(7, &count) ||
        count != (should_inherit ? 1u : 0u) ||
        !nt_runtime_restore_inherited_files(7))
        return 31;
    close(7);
    uint32_t flags = 0;
    if (!should_inherit) {
        nt_SetLastError(0);
        return nt_GetHandleInformation(value, &flags) ||
               nt_GetLastError() != NT_ERROR_INVALID_HANDLE ? 32 : 0;
    }
    if (!nt_GetHandleInformation(value, &flags) || (flags & 1u) == 0)
        return 33;
    char result[6] = {0};
    uint32_t read = 0;
    if (!nt_ReadFile(value, result, 5, &read, NULL) ||
        read != 5 || strcmp(result, "HELLO") != 0)
        return 34;
    if (!nt_CloseHandle(value))
        return 35;
    return 0;
}

static int run_pipe_child(uintptr_t value)
{
    size_t count = 0;
    if (!nt_runtime_inherited_file_count(7, &count) || count != 1u ||
        !nt_runtime_restore_inherited_files(7))
        return 36;
    close(7);
    uint32_t written = 0;
    if (nt_GetFileType(value) != 3 ||
        !nt_WriteFile(value, "PIPE_OK", 7, &written, NULL) || written != 7 ||
        !nt_CloseHandle(value))
        return 37;
    return 0;
}

static void spawn_child(const char *executable, const char *mode,
                        uintptr_t handle, const struct nt_child_file_transfer *transfer)
{
    posix_spawn_file_actions_t actions;
    check(posix_spawn_file_actions_init(&actions) == 0, "init spawn actions");
    const int manifest = fcntl(transfer->manifest_fd, F_DUPFD_CLOEXEC, 96);
    check(manifest >= 96, "clone sealed metadata descriptor");
    check(posix_spawn_file_actions_adddup2(&actions, manifest, 7) == 0,
          "inherit sealed metadata as fd7");
    for (size_t i = 0; i < transfer->count; ++i)
        check(posix_spawn_file_actions_adddup2(&actions, transfer->source_fds[i],
                                               NT_CHILD_FILE_FD_BASE + (int)i) == 0,
              "inherit only selected host file descriptors");
    char token[48];
    snprintf(token, sizeof(token), "%llu", (unsigned long long)handle);
    char *const arguments[] = {(char *)executable, (char *)mode, token, NULL};
    pid_t pid = -1;
    check(posix_spawn(&pid, executable, &actions, NULL, arguments, environ) == 0,
          "start subprocess with sealed metadata and selected fd");
    posix_spawn_file_actions_destroy(&actions);
    close(manifest);
    int code = 0;
    check(waitpid(pid, &code, 0) == pid && WIFEXITED(code) && WEXITSTATUS(code) == 0,
          "subprocess reconstructed expected Windows HANDLE semantics");
}

int main(int argc, char **argv)
{
    if (argc == 3) {
        char *end = NULL;
        const unsigned long long parsed = strtoull(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0')
            return 30;
        if (strcmp(argv[1], "--inherited") == 0)
            return run_child(true, (uintptr_t)parsed);
        if (strcmp(argv[1], "--not-inherited") == 0)
            return run_child(false, (uintptr_t)parsed);
        if (strcmp(argv[1], "--pipe-child") == 0)
            return run_pipe_child((uintptr_t)parsed);
        return 29;
    }
    check(argc == 1, "test arguments");
    char temporary[] = "/tmp/nt-stage4-file-handles-XXXXXX";
    char *root = mkdtemp(temporary);
    check(root != NULL && nt_runtime_set_file_root(root), "private guest volume");
    struct {uint32_t size; const void *descriptor; int32_t inherit;} security =
        {24u, NULL, 1};
    uintptr_t file = nt_CreateFileA("C:\\inherited.bin", UINT32_C(0xc0000000), 3,
                                    &security, 2, 0x80, 0);
    check(file != UINTPTR_MAX, "open inheritable guest file");
    uint32_t done = 0;
    check(nt_WriteFile(file, "HELLO", 5, &done, NULL) && done == 5,
          "write baseline in parent");
    check(nt_SetFilePointerEx(file, 0, NULL, 0), "reset shared offset");
    struct nt_child_file_transfer files;
    check(nt_runtime_snapshot_inherited_files(true, &files, NULL) == NT_ERROR_SUCCESS &&
          files.count == 1 && files.source_fds[0] >= 128,
          "capture exact inheritable file handle with duplicated host FD");
    spawn_child(argv[0], "--inherited", file, &files);
    nt_runtime_release_inherited_files(&files);
    int64_t current = -1;
    check(nt_SetFilePointerEx(file, 0, &current, 1) && current == 5,
          "child advanced parent's shared open-file description");
    check(nt_runtime_snapshot_inherited_files(false, &files, NULL) == NT_ERROR_SUCCESS &&
          files.count == 0,
          "bInheritHandles=FALSE excludes even explicitly inheritable file handles");
    spawn_child(argv[0], "--not-inherited", file, &files);
    nt_runtime_release_inherited_files(&files);
    int pipe_fds[2];
    check(pipe(pipe_fds) == 0, "make unsealed descriptor");
    size_t count = 0;
    check(!nt_runtime_inherited_file_count(pipe_fds[0], &count),
          "reject non-sealed or forged handle manifests");
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    /* An event/sem HANDLE is NOT a Linux fd. Process creation must reject
     * it rather than silently pretending it was inherited. */
    uintptr_t event = nt_CreateEventW(NULL, 1, 0, NULL);
    check(event != 0 && nt_runtime_set_waitable_flags(event, 1u, 1u),
          "mark event inheritable");
    check(nt_runtime_snapshot_inherited_files(true, &files, NULL) ==
          NT_ERROR_NOT_SUPPORTED, "reject unsupported cross-process event inheritance");
    check(nt_runtime_close_waitable(event), "close event");
    /* Anonymous pipes use real kernel pipes and the same inherited file
     * protocol. The child's WriteFile reaches the parent's ReadFile. */
    uintptr_t read_end = 0, write_end = 0;
    check(nt_CreatePipe(&read_end, &write_end, &security, 0) &&
          read_end != 0 && write_end != 0 &&
          nt_GetFileType(read_end) == 3 && nt_GetFileType(write_end) == 3,
          "create real anonymous Win32 pipe");
    check(nt_SetHandleInformation(read_end, 1u, 0) &&
          nt_SetHandleInformation(file, 1u, 0),
          "only the write pipe end is inheritable");
    check(nt_runtime_snapshot_inherited_files(true, &files, NULL) ==
          NT_ERROR_SUCCESS && files.count == 1,
          "snapshot only inheritable pipe writer");
    spawn_child(argv[0], "--pipe-child", write_end, &files);
    nt_runtime_release_inherited_files(&files);
    char pipe_data[8] = {0};
    uint32_t got = 0;
    check(nt_ReadFile(read_end, pipe_data, 7, &got, NULL) &&
          got == 7 && strcmp(pipe_data, "PIPE_OK") == 0,
          "read message from child's inherited pipe writer");
    check(nt_CloseHandle(read_end) && nt_CloseHandle(write_end),
          "close both ends of anonymous pipe");
    check(nt_CloseHandle(file), "close original inherited file HANDLE");
    check(nt_runtime_set_file_root(NULL), "release guest volume");
    char pathname[256];
    snprintf(pathname, sizeof(pathname), "%s/inherited.bin", root);
    check(unlink(pathname) == 0 && rmdir(root) == 0, "delete isolated test files");
    puts("PASS real exec-boundary file HANDLE inheritance, identity, rights, offset and isolation");
    return 0;
}
