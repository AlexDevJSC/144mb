/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "compat.h"
#include "section_memory.h"
#include "sync.h"
#include "winfile.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void require(int condition, const char *expression, int line)
{
    if (!condition) {
        fprintf(stderr, "FAIL section-memory line %d: %s (last=%u)\n",
                line, expression, nt_GetLastError());
        exit(EXIT_FAILURE);
    }
}
#define CHECK(expr) require((expr), #expr, __LINE__)

int main(void)
{
    /* PE loader and normal Win32 callers resolve the very same API. */
    const char *dll = "kernel32.dll";
    CHECK(nt_runtime_resolve(dll, "CreateFileMappingW") ==
          (nt_guest_proc)nt_CreateFileMappingW);
    CHECK(nt_runtime_resolve(dll, "MapViewOfFile") ==
          (nt_guest_proc)nt_MapViewOfFile);
    CHECK(nt_runtime_resolve(dll, "UnmapViewOfFile") ==
          (nt_guest_proc)nt_UnmapViewOfFile);
    CHECK(nt_runtime_resolve(dll, "OpenFileMappingW") ==
          (nt_guest_proc)nt_OpenFileMappingW);

    CHECK(nt_CreateFileMappingW(0, NULL, 4, 0, 65536, NULL) == 0 &&
          nt_GetLastError() == NT_ERROR_NOT_SUPPORTED);
    CHECK(nt_CreateFileMappingW(UINTPTR_MAX, NULL, 4, 0, 0, NULL) == 0 &&
          nt_GetLastError() == NT_ERROR_INVALID_PARAMETER);
    CHECK(nt_OpenFileMappingW(4, 0, u"absent") == 0 &&
          nt_GetLastError() == NT_ERROR_FILE_NOT_FOUND);

    const uint16_t name[] = u"shared_base_01";
    uint32_t initial = 0, count = 0;
    CHECK(nt_GetProcessHandleCount(nt_GetCurrentProcess(), &initial));
    uintptr_t first = nt_CreateFileMappingW(UINTPTR_MAX, NULL, 4,
                                             0, 65536, name);
    CHECK(first != 0 && nt_GetLastError() == NT_ERROR_SUCCESS);
    uintptr_t second = nt_OpenFileMappingW(4, 0, name);
    CHECK(second != 0 && second != first);
    uintptr_t duplicate = 0;
    CHECK(nt_DuplicateHandle(nt_GetCurrentProcess(), first, nt_GetCurrentProcess(),
                             &duplicate, 0, 0, 2) &&
          duplicate != 0 && duplicate != first && duplicate != second);
    CHECK(nt_GetProcessHandleCount(nt_GetCurrentProcess(), &count) &&
          count == initial + 3);

    unsigned char *writer = nt_MapViewOfFile(first, 2, 0, 0, 65536);
    unsigned char *reader = nt_MapViewOfFile(second, 4, 0, 0, 65536);
    unsigned char *copy = nt_MapViewOfFile(duplicate, 1, 0, 0, 65536);
    CHECK(writer && reader && copy);
    writer[0] = 17;
    writer[65535] = 44;
    CHECK(reader[0] == 17 && reader[65535] == 44 &&
          copy[0] == 17 && copy[65535] == 44);
    copy[0] = 90; /* FILE_MAP_COPY: private COW, never writes the section. */
    CHECK(writer[0] == 17 && reader[0] == 17 && copy[0] == 90);
    CHECK(nt_FlushViewOfFile(writer, 4096));
    struct nt_memory_basic_information info = {0};
    CHECK(nt_VirtualQuery(reader, &info, sizeof(info)) == sizeof(info));
    CHECK(info.type == 0x40000 && info.state == 0x1000 &&
          info.protect == 2 && info.base_address == reader);

    CHECK(!nt_MapViewOfFile(second, 2, 0, 0, 4096) &&
          nt_GetLastError() == NT_ERROR_ACCESS_DENIED);
    CHECK(!nt_MapViewOfFile(first, 2, 0, 4096, 4096) &&
          nt_GetLastError() == NT_ERROR_INVALID_PARAMETER);
    CHECK(!nt_MapViewOfFile(first, 2, 0, 0, 131072) &&
          nt_GetLastError() == NT_ERROR_INVALID_PARAMETER);
    CHECK(!nt_UnmapViewOfFile((char *)reader + 1) &&
          nt_GetLastError() == NT_ERROR_INVALID_ADDRESS);

    /* Real Linux shared pages, not a mocked array. A child that inherited
     * the memory view modifies it; no simulated PE process is involved. */
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        writer[1] = 73;
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0 && reader[1] == 73);

    /* HANDLE flags, duplicate life and mapped view life are separate. */
    CHECK(nt_SetHandleInformation(first, 2, 2));
    CHECK(!nt_CloseHandle(first) && nt_GetLastError() == NT_ERROR_ACCESS_DENIED);
    CHECK(nt_SetHandleInformation(first, 2, 0));
    CHECK(nt_CloseHandle(first) && nt_CloseHandle(second));
    CHECK(nt_CloseHandle(duplicate));
    CHECK(nt_GetProcessHandleCount(nt_GetCurrentProcess(), &count) &&
          count == initial);
    CHECK(reader[0] == 17 && writer[1] == 73);
    CHECK(nt_OpenFileMappingW(4, 0, name) != 0);
    uintptr_t lingering = nt_OpenFileMappingW(4, 0, name);
    CHECK(lingering != 0 && nt_CloseHandle(lingering));
    CHECK(nt_UnmapViewOfFile(copy) && nt_UnmapViewOfFile(reader) &&
          nt_UnmapViewOfFile(writer));
    CHECK(nt_OpenFileMappingW(4, 0, name) == 0 &&
          nt_GetLastError() == NT_ERROR_FILE_NOT_FOUND);

    /* A name stays alive only while an object handle or view exists. */
    uintptr_t only = nt_CreateFileMappingW(UINTPTR_MAX, NULL, 2, 0, 4096, u"readonly");
    CHECK(only != 0);
    CHECK(!nt_MapViewOfFile(only, 2, 0, 0, 4096) &&
          nt_GetLastError() == NT_ERROR_ACCESS_DENIED);
    const uint8_t *read_only = nt_MapViewOfFile(only, 4, 0, 0, 0);
    CHECK(read_only && read_only[0] == 0);
    CHECK(nt_CloseHandle(only) && nt_UnmapViewOfFile(read_only));
    CHECK(!nt_UnmapViewOfFile(read_only));
    puts("PASS core shared sections: Win32 exports, handles, MAP_SHARED, COW, fork and MEM_MAPPED");
    return EXIT_SUCCESS;
}
