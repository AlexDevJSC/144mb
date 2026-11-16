/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "compat.h"
#include "object_broker.h"
#include "section_memory.h"
#include "sync.h"
#include "winfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void require(int success, const char *what, int line)
{
    if (!success) {
        fprintf(stderr, "FAIL broker test line %d: %s (last error %u)\n",
                line, what, nt_GetLastError());
        exit(1);
    }
}
#define CHECK(e) require((e), #e, __LINE__)
enum { ALL_ACCESS = 0x1f0003u, READ_ACCESS = 0x100004u };
static const uint16_t *section_name = u"broker_café_section";
static const uint16_t *event_name = u"broker_event";
static const uint16_t *semaphore_name = u"broker_sem";

static int child_client(void)
{
    uintptr_t map = nt_OpenFileMappingW(4, 0, section_name);
    uintptr_t event = nt_OpenEventW(ALL_ACCESS, 0, event_name);
    uintptr_t sem = nt_OpenSemaphoreW(ALL_ACCESS, 0, semaphore_name);
    CHECK(map && event && sem);
    unsigned char *data = nt_MapViewOfFile(map, 2, 0, 0, 4096);
    CHECK(data);
    data[0] = 42;
    CHECK(nt_SetEvent(event));
    CHECK(nt_WaitForSingleObject(sem, 10000) == 0);
    data[0] = 93;
    CHECK(nt_UnmapViewOfFile(data));
    CHECK(nt_CloseHandle(map) && nt_CloseHandle(event) && nt_CloseHandle(sem));
    return 0;
}
static int race_client(unsigned index)
{
    uintptr_t mapping = nt_OpenFileMappingW(4, 0, section_name);
    CHECK(mapping);
    unsigned char *data = nt_MapViewOfFile(mapping, 2, 0, 0, 4096);
    CHECK(data);
    data[100 + index] = (unsigned char)(index + 1);
    CHECK(nt_UnmapViewOfFile(data));
    CHECK(nt_CloseHandle(mapping));
    return 0;
}
static int leak_client(void)
{
    uintptr_t event = nt_CreateEventW(NULL, 1, 1, u"broker_dead_client");
    CHECK(event);
    /* Exit without CloseHandle. The broker's lease must be reclaimed. */
    _exit(0);
}
static int dead_server_client(void)
{
    CHECK(nt_runtime_broker_enabled());
    CHECK(nt_OpenEventW(ALL_ACCESS, 0, event_name) == 0 &&
          nt_GetLastError() == NT_ERROR_GEN_FAILURE);
    return 0;
}
static pid_t spawn(const char *binary, const char *mode, const char *arg)
{
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        if (arg)
            execl(binary, binary, mode, arg, (char *)NULL);
        else
            execl(binary, binary, mode, (char *)NULL);
        _exit(127);
    }
    return pid;
}
static void join(pid_t pid)
{
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid &&
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        if (!strcmp(argv[1], "--child"))
            return child_client();
        if (!strcmp(argv[1], "--race") && argc == 3)
            return race_client((unsigned)strtoul(argv[2], NULL, 10));
        if (!strcmp(argv[1], "--leak"))
            return leak_client();
        if (!strcmp(argv[1], "--unavailable"))
            return dead_server_client();
        return 2;
    }
    CHECK(nt_runtime_broker_enabled());
    CHECK(nt_runtime_resolve("kernel32.dll", "CreateFileMappingW") ==
          (nt_guest_proc)nt_CreateFileMappingW);
    CHECK(nt_runtime_resolve("ntdll.dll", "NtCreateSemaphore") != NULL);
    uint32_t before = 0, after = 0;
    CHECK(nt_GetProcessHandleCount(nt_GetCurrentProcess(), &before));

    uintptr_t mapping = nt_CreateFileMappingW(UINTPTR_MAX, NULL, 4, 0,
                                               65536, section_name);
    CHECK(mapping && nt_GetLastError() == NT_ERROR_SUCCESS);
    uintptr_t mapping_again = nt_CreateFileMappingW(UINTPTR_MAX, NULL, 4,
                                                     0, 65536, section_name);
    CHECK(mapping_again && mapping_again != mapping &&
          nt_GetLastError() == NT_ERROR_ALREADY_EXISTS);
    uintptr_t event = nt_CreateEventW(NULL, 0, 0, event_name);
    uintptr_t sem = nt_CreateSemaphoreW(NULL, 0, 3, semaphore_name);
    CHECK(event && sem);
    CHECK(!nt_CreateSemaphoreW(NULL, 0, 1, event_name));
    CHECK(!nt_CreateFileMappingW(UINTPTR_MAX, NULL, 4, 0, 4096, event_name));
    CHECK(nt_GetProcessHandleCount(nt_GetCurrentProcess(), &after) &&
          after == before + 4);

    /* A completely fresh executable opens all three shared objects.
     * No Linux inherited file descriptions are used: SCM_RIGHTS is
     * transmitted by the broker after exec. */
    unsigned char *data = nt_MapViewOfFile(mapping, 2, 0, 0, 65536);
    CHECK(data && data[0] == 0);
    pid_t child = spawn(argv[0], "--child", NULL);
    CHECK(nt_WaitForSingleObject(event, 10000) == 0);
    CHECK(data[0] == 42);
    CHECK(nt_ReleaseSemaphore(sem, 1, NULL));
    join(child);
    CHECK(data[0] == 93);

    /* Distinct handle numbers and lifetime from the mapped view. */
    uintptr_t alias = 0;
    CHECK(nt_DuplicateHandle(nt_GetCurrentProcess(), mapping,
                             nt_GetCurrentProcess(), &alias, 0, 0, 2));
    CHECK(alias && alias != mapping);
    CHECK(nt_CloseHandle(mapping_again) && nt_CloseHandle(mapping));
    CHECK(data[0] == 93 && nt_OpenFileMappingW(4, 0, section_name));
    uintptr_t opened = nt_OpenFileMappingW(4, 0, section_name);
    CHECK(opened && nt_CloseHandle(opened));

    pid_t workers[8];
    for (unsigned i = 0; i < 8; ++i) {
        char index[16];
        snprintf(index, sizeof(index), "%u", i);
        workers[i] = spawn(argv[0], "--race", index);
    }
    for (unsigned i = 0; i < 8; ++i)
        join(workers[i]);
    for (unsigned i = 0; i < 8; ++i)
        CHECK(data[100 + i] == i + 1);

    uintptr_t sem_one = nt_CreateSemaphoreW(NULL, 1, 3, u"broker_waitall_a");
    uintptr_t sem_two = nt_CreateSemaphoreW(NULL, 0, 3, u"broker_waitall_b");
    CHECK(sem_one && sem_two);
    uintptr_t both[2] = {sem_one, sem_two};
    CHECK(nt_WaitForMultipleObjects(2, both, 1, 0) == 258);
    CHECK(nt_ReleaseSemaphore(sem_two, 1, NULL));
    CHECK(nt_WaitForMultipleObjects(2, both, 1, 1000) == 0);
    CHECK(nt_WaitForSingleObject(sem_one, 0) == 258 &&
          nt_WaitForSingleObject(sem_two, 0) == 258);
    CHECK(nt_CloseHandle(sem_one) && nt_CloseHandle(sem_two));

    CHECK(nt_CloseHandle(alias) && nt_CloseHandle(event) && nt_CloseHandle(sem));
    CHECK(nt_UnmapViewOfFile(data));
    /* All live references are gone; the broker reaps the name. */
    int removed = 0;
    for (int i = 0; i < 100; ++i) {
        uintptr_t h = nt_OpenFileMappingW(4, 0, section_name);
        if (!h && nt_GetLastError() == NT_ERROR_FILE_NOT_FOUND) {
            removed = 1;
            break;
        }
        if (h)
            nt_CloseHandle(h);
        usleep(10000);
    }
    CHECK(removed);
    join(spawn(argv[0], "--leak", NULL));
    removed = 0;
    for (int i = 0; i < 100; ++i) {
        uintptr_t h = nt_OpenEventW(ALL_ACCESS, 0, u"broker_dead_client");
        if (!h && nt_GetLastError() == NT_ERROR_FILE_NOT_FOUND) {
            removed = 1;
            break;
        }
        if (h)
            nt_CloseHandle(h);
        usleep(10000);
    }
    CHECK(removed);
    CHECK(nt_GetProcessHandleCount(nt_GetCurrentProcess(), &after) &&
          after == before);
    puts("PASS broker independent Linux exec: named sections, events, semaphores, wait-all, concurrency, refs and crash leases");
    return 0;
}
