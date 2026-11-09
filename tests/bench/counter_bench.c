/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "ntnative/counter.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <time.h>

enum { SAMPLES = 7, CALLS_PER_SAMPLE = 500000 };

typedef nt_status (*counter_fn)(int64_t *, int64_t *);

static nt_status linux_counter(int64_t *counter, int64_t *frequency)
{
    struct timespec value;
    if (clock_gettime(CLOCK_BOOTTIME, &value) != 0)
        return NT_STATUS_UNSUCCESSFUL;

    *counter = (int64_t)value.tv_sec * NT_COUNTER_FREQUENCY + value.tv_nsec / 100;
    *frequency = NT_COUNTER_FREQUENCY;
    return NT_STATUS_SUCCESS;
}

static int64_t now_ns(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &value) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (int64_t)value.tv_sec * INT64_C(1000000000) + value.tv_nsec;
}

static double sample(counter_fn query)
{
    int64_t previous = -1;
    const int64_t start = now_ns();
    for (unsigned i = 0; i < CALLS_PER_SAMPLE; ++i) {
        int64_t current;
        int64_t frequency;
        if (query(&current, &frequency) != NT_STATUS_SUCCESS ||
            current < previous || frequency != NT_COUNTER_FREQUENCY) {
            fputs("counter benchmark failed its correctness checks\n", stderr);
            exit(EXIT_FAILURE);
        }
        previous = current;
    }
    return (double)(now_ns() - start) / CALLS_PER_SAMPLE;
}

static int compare_double(const void *left, const void *right)
{
    const double a = *(const double *)left;
    const double b = *(const double *)right;
    return (a > b) - (a < b);
}

static void report(const char *name, double values[SAMPLES])
{
    qsort(values, SAMPLES, sizeof(values[0]), compare_double);
    printf("%s: %.2f ns/call median, %.2f–%.2f range\n",
           name, values[SAMPLES / 2], values[0], values[SAMPLES - 1]);
}

int main(void)
{
    double baseline[SAMPLES];
    double experiment[SAMPLES];

    /* Warm both paths. Alternate sample order to limit timing bias. */
    sample(linux_counter);
    sample(nt_query_performance_counter);
    for (unsigned i = 0; i < SAMPLES; ++i) {
        if (i % 2 == 0) {
            baseline[i] = sample(linux_counter);
            experiment[i] = sample(nt_query_performance_counter);
        } else {
            experiment[i] = sample(nt_query_performance_counter);
            baseline[i] = sample(linux_counter);
        }
    }
    printf("%d samples of %d calls per path\n", SAMPLES, CALLS_PER_SAMPLE);
    report("Linux clock with conversion", baseline);
    report("NT compatibility function", experiment);

    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        perror("getrusage");
        return EXIT_FAILURE;
    }
    printf("Combined benchmark process peak RSS: %ld KiB\n", usage.ru_maxrss);
    return EXIT_SUCCESS;
}
