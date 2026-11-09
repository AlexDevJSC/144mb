/* SPDX-License-Identifier: MIT */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef LONG (NTAPI *qpc_fn)(LARGE_INTEGER *, LARGE_INTEGER *);
static const DWORD access_violation = 0xC0000005UL;

static void check(int condition, const char *name)
{
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        exit(EXIT_FAILURE);
    }
    printf("PASS %s\n", name);
}

int main(void)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    FARPROC symbol = ntdll == NULL ? NULL : GetProcAddress(ntdll, "NtQueryPerformanceCounter");
    check(symbol != NULL, "native NtQueryPerformanceCounter export");

    qpc_fn query;
    _Static_assert(sizeof(query) == sizeof(symbol), "Windows function pointer size");
    memcpy(&query, &symbol, sizeof(query));

    LARGE_INTEGER first;
    LARGE_INTEGER next;
    LARGE_INTEGER frequency;
    LARGE_INTEGER next_frequency;
    check(query(&first, &frequency) == 0 && frequency.QuadPart > 0,
          "valid outputs and positive frequency");
    check(query(&next, &next_frequency) == 0 && next.QuadPart >= first.QuadPart &&
              next_frequency.QuadPart == frequency.QuadPart,
          "nondecreasing counter and stable frequency");
    check(query(&first, NULL) == 0 && first.QuadPart >= next.QuadPart,
          "optional frequency may be null");

    frequency.QuadPart = -1;
    check((DWORD)query(NULL, &frequency) == access_violation, "required null counter");
    printf("OBS frequency after null counter=%lld\n", (long long)frequency.QuadPart);
    check((DWORD)query(NULL, NULL) == access_violation, "both outputs null");

    LARGE_INTEGER *inaccessible = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT,
                                                PAGE_NOACCESS);
    check(inaccessible != NULL, "allocate inaccessible page for fault characterization");
    check((DWORD)query(inaccessible, NULL) == access_violation, "inaccessible counter");
    first.QuadPart = -1;
    check((DWORD)query(&first, inaccessible) == access_violation, "inaccessible frequency");
    printf("OBS counter changed on inaccessible frequency=%d\n", first.QuadPart != -1);
    check(VirtualFree(inaccessible, 0, MEM_RELEASE) != 0, "release fault probe page");
    return EXIT_SUCCESS;
}
