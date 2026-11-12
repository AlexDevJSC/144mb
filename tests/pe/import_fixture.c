/* SPDX-License-Identifier: MIT */
#include "ntnative/counter.h"

#include <stddef.h>

__declspec(dllimport) nt_status NtQueryPerformanceCounter(int64_t *, int64_t *);

/* This is the PE console entry point under the Windows x64 ABI. The
 * counter frequency is platform-dependent, so compare it across calls.
 */
int import_fixture_entry(void)
{
    int64_t first = -1;
    int64_t second = -1;
    int64_t frequency = 0;
    int64_t second_frequency = 0;

    if (NtQueryPerformanceCounter(&first, &frequency) != NT_STATUS_SUCCESS ||
        first < 0 || frequency <= 0)
        return 1;

    if (NtQueryPerformanceCounter(&second, &second_frequency) != NT_STATUS_SUCCESS ||
        second < first || second_frequency != frequency)
        return 2;

    if (NtQueryPerformanceCounter(&first, NULL) != NT_STATUS_SUCCESS || first < second)
        return 3;

    return 0;
}
