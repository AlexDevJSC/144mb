/* SPDX-License-Identifier: MIT */
#include "fixture_abi.h"

#include <stddef.h>

/* The Linux fixture host passes this callback in RCX. A normal Windows
 * entry point has no such argument contract.
 */
int NT_MS_ABI fixture_entry(nt_fixture_query query)
{
    int64_t first;
    int64_t second;
    int64_t frequency;
    int64_t second_frequency;

    if (query(&first, &frequency) != NT_STATUS_SUCCESS ||
        first < 0 || frequency != NT_COUNTER_FREQUENCY)
        return 1;

    if (query(&second, &second_frequency) != NT_STATUS_SUCCESS ||
        second < first || second_frequency != frequency)
        return 2;

    if (query(&first, NULL) != NT_STATUS_SUCCESS || first < second)
        return 3;

    frequency = -1;
    if (query(NULL, &frequency) != NT_STATUS_ACCESS_VIOLATION || frequency != -1)
        return 4;

    return 0;
}
