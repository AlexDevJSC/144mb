/* SPDX-License-Identifier: MIT */
#include <stdint.h>

__declspec(dllimport) int32_t QueryPerformanceCounter(int64_t *counter);

int import_fixture_entry(void)
{
    int64_t first = -1;
    int64_t second = -1;
    if (QueryPerformanceCounter(&first) == 0 || first < 0)
        return 1;
    if (QueryPerformanceCounter(&second) == 0 || second < first)
        return 2;
    return 0;
}
