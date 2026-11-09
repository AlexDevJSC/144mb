/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "ntnative/counter.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum clock_mode { REAL_CLOCK, FIXED_CLOCK, FAILED_CLOCK };
static enum clock_mode mode;
static struct timespec fixed_time;
static unsigned clock_calls;

int __real_clock_gettime(clockid_t clock, struct timespec *value);

int __wrap_clock_gettime(clockid_t clock, struct timespec *value)
{
    ++clock_calls;
    if (clock != CLOCK_BOOTTIME || mode == FAILED_CLOCK) {
        errno = EINVAL;
        return -1;
    }
    if (mode == FIXED_CLOCK) {
        *value = fixed_time;
        return 0;
    }
    return __real_clock_gettime(clock, value);
}

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
    int64_t first = -1;
    int64_t next = -1;
    int64_t frequency = -1;
    int64_t next_frequency = -1;

    check(nt_query_performance_counter(&first, &frequency) == NT_STATUS_SUCCESS &&
              first >= 0 && frequency == NT_COUNTER_FREQUENCY,
          "valid outputs and implementation frequency");
    check(nt_query_performance_counter(&next, &next_frequency) == NT_STATUS_SUCCESS &&
              next >= first && next_frequency == frequency,
          "nondecreasing counter and stable frequency");
    first = -1;
    check(nt_query_performance_counter(&first, NULL) == NT_STATUS_SUCCESS &&
              first >= next,
          "optional frequency may be null");

    frequency = -1;
    const unsigned calls_before_null = clock_calls;
    check(nt_query_performance_counter(NULL, &frequency) == NT_STATUS_ACCESS_VIOLATION &&
              frequency == -1 && clock_calls == calls_before_null,
          "required null counter leaves frequency untouched");
    check(nt_query_performance_counter(NULL, NULL) == NT_STATUS_ACCESS_VIOLATION,
          "both outputs null");

    mode = FIXED_CLOCK;
    fixed_time = (struct timespec){.tv_sec = 12, .tv_nsec = 123456789};
    check(nt_query_performance_counter(&next, &frequency) == NT_STATUS_SUCCESS &&
              next == INT64_C(121234567) && frequency == NT_COUNTER_FREQUENCY,
          "100 ns conversion rounds down");

    fixed_time = (struct timespec){
        .tv_sec = INT64_MAX / NT_COUNTER_FREQUENCY,
        .tv_nsec = (INT64_MAX % NT_COUNTER_FREQUENCY) * 100
    };
    check(nt_query_performance_counter(&next, NULL) == NT_STATUS_SUCCESS &&
              next == INT64_MAX,
          "largest representable counter");
    fixed_time.tv_nsec += 100;
    frequency = -1;
    check(nt_query_performance_counter(&next, &frequency) == NT_STATUS_UNSUCCESSFUL &&
              next == INT64_MAX && frequency == -1,
          "overflow leaves both outputs untouched");

    mode = FAILED_CLOCK;
    first = -1;
    frequency = -1;
    check(nt_query_performance_counter(&first, &frequency) == NT_STATUS_UNSUCCESSFUL &&
              first == -1 && frequency == -1,
          "clock error leaves both outputs untouched");
    return EXIT_SUCCESS;
}
