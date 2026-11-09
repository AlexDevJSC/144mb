/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "ntnative/counter.h"

#include <stddef.h>
#include <time.h>

nt_status nt_query_performance_counter(int64_t *counter, int64_t *frequency)
{
    if (counter == NULL)
        return NT_STATUS_ACCESS_VIOLATION;

    struct timespec now;
    if (clock_gettime(CLOCK_BOOTTIME, &now) != 0)
        return NT_STATUS_UNSUCCESSFUL;

    const int64_t ticks_within_second = now.tv_nsec / 100;
    if (now.tv_sec < 0 ||
        (uint64_t)now.tv_sec >
            (uint64_t)((INT64_MAX - ticks_within_second) / NT_COUNTER_FREQUENCY))
        return NT_STATUS_UNSUCCESSFUL;

    *counter = (int64_t)now.tv_sec * NT_COUNTER_FREQUENCY + ticks_within_second;
    if (frequency != NULL)
        *frequency = NT_COUNTER_FREQUENCY;

    return NT_STATUS_SUCCESS;
}
