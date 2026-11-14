/* SPDX-License-Identifier: MIT */
#ifndef NT_RUNTIME_CHILD_PROCESS_H
#define NT_RUNTIME_CHILD_PROCESS_H

#include "nt_services.h"
#include "winfile.h"

#include <stdint.h>
#include <sys/types.h>

struct nt_process_information {
    uintptr_t process, thread;
    uint32_t process_id, thread_id;
};
struct nt_startup_info {
    uint32_t size;
    const uint16_t *reserved, *desktop, *title;
    uint32_t x, y, width, height, columns, rows, fill, flags;
    uint16_t show_window, reserved_bytes;
    void *reserved_data;
    uintptr_t input, output, error;
};

typedef int (*nt_child_launcher)(int fd, const char *dos_path, const char *command,
                                const char *directory, bool suspended, int environment_fd,
                                const struct nt_child_file_transfer *files,
                                pid_t *pid, int *status_fd, int *start_fd);
void nt_runtime_set_child_launcher(nt_child_launcher launcher);
/* Sealed, bounded UTF-8 guest environment payload; not host envp. */
uint32_t nt_runtime_build_child_environment(const uint16_t *block, int *fd);
bool nt_runtime_read_child_environment(int fd, char ***entries, char **storage);
bool nt_runtime_watch_child(pid_t pid, int status_fd, int start_fd,
                            struct nt_process_information *info);
nt_status nt_runtime_terminate_child_process(uintptr_t handle, uint32_t exit_code);

uint16_t *nt_runtime_ansi_child_environment(const char *source);
int32_t NT_MS_ABI nt_CreateProcessA(const char *application, char *command,
        const void *process_security, const void *thread_security, int32_t inherit_handles,
        uint32_t flags, const void *environment, const char *directory,
        const struct nt_startup_info *startup, struct nt_process_information *info);
int32_t NT_MS_ABI nt_CreateProcessW(const uint16_t *application, const uint16_t *command,
        const void *process_security, const void *thread_security, int32_t inherit_handles,
        uint32_t flags, const void *environment, const uint16_t *directory,
        const struct nt_startup_info *startup, struct nt_process_information *info);
int32_t NT_MS_ABI nt_GetExitCodeProcess(uintptr_t handle, uint32_t *exit_code);

#endif
