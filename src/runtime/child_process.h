/* SPDX-License-Identifier: MIT */
#ifndef NT_RUNTIME_CHILD_PROCESS_H
#define NT_RUNTIME_CHILD_PROCESS_H

#include "ntnative/runtime.h"

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
                                const char *directory, pid_t *pid, int *status_fd);
void nt_runtime_set_child_launcher(nt_child_launcher launcher);
bool nt_runtime_watch_child(pid_t pid, int status_fd, struct nt_process_information *info);

int32_t NT_MS_ABI nt_CreateProcessW(const uint16_t *application, const uint16_t *command,
        const void *process_security, const void *thread_security, int32_t inherit_handles,
        uint32_t flags, const void *environment, const uint16_t *directory,
        const struct nt_startup_info *startup, struct nt_process_information *info);
int32_t NT_MS_ABI nt_GetExitCodeProcess(uintptr_t handle, uint32_t *exit_code);

#endif
