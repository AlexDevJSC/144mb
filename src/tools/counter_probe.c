/* SPDX-License-Identifier: MIT */
#include "ntnative/counter.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    int64_t counter;
    int64_t frequency;
    const nt_status status = nt_query_performance_counter(&counter, &frequency);
    if (status != NT_STATUS_SUCCESS) {
        fprintf(stderr, "NtQueryPerformanceCounter: 0x%08" PRIX32 "\n",
                (uint32_t)status);
        return EXIT_FAILURE;
    }

    printf("backend=linux-clock-boottime counter=%" PRId64 " frequency=%" PRId64 "\n",
           counter, frequency);
    return EXIT_SUCCESS;
}
