/* SPDX-License-Identifier: MIT */
/* Same-source C11 Windows/Linux scheduler observation: two CPU-bound threads
 * pinned to ONE accessible CPU. Not a measurement of NT quantum itself.
 * Reports CPU-time fairness and wake-to-first-run latency, without assuming
 * matching physical CPUs, clock domains, timeslices, or kernel policies. */
#define _GNU_SOURCE
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <time.h>
#include <errno.h>
#endif

struct worker_data {
    volatile uint64_t iterations;
    uint64_t cpu_ns;
    uint64_t first_tick;
    int pin_valid;
};

static struct worker_data workers[2];
#ifdef _WIN32
static volatile LONG ready, start_gate, stop_gate;
static GROUP_AFFINITY chosen = {0};
static LARGE_INTEGER clock_frequency;

static uint64_t tick_now(void)
{
    LARGE_INTEGER t;
    return QueryPerformanceCounter(&t) ? (uint64_t)t.QuadPart : 0;
}
static uint64_t tick_to_ns(uint64_t delta)
{
    return (uint64_t)((double)delta * 1000000000.0 /
                      (double)clock_frequency.QuadPart);
}
static uint64_t ft_100ns(FILETIME ft)
{
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}
static DWORD WINAPI worker_main(void *argument)
{
    struct worker_data *data = (struct worker_data *)argument;
    GROUP_AFFINITY current = {0};
    data->pin_valid = GetThreadGroupAffinity(GetCurrentThread(), &current) &&
                      current.Group == chosen.Group &&
                      current.Mask == chosen.Mask;
    InterlockedIncrement(&ready);
    while (InterlockedCompareExchange(&start_gate, 0, 0) == 0)
        Sleep(0);
    data->first_tick = tick_now();
    while (InterlockedCompareExchange(&stop_gate, 0, 0) == 0)
        ++data->iterations;
    return 0;
}
#else
static atomic_int ready, start_gate, stop_gate;
static int chosen_cpu;
static uint64_t tick_now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
        return 0;
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) +
           (uint64_t)t.tv_nsec;
}
static uint64_t tick_to_ns(uint64_t delta) { return delta; }
static uint64_t thread_cpu_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) != 0)
        return 0;
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) +
           (uint64_t)t.tv_nsec;
}
static void *worker_main(void *argument)
{
    struct worker_data *data = argument;
    cpu_set_t set;
    data->pin_valid = pthread_getaffinity_np(pthread_self(),
                      sizeof(set), &set) == 0 &&
                      CPU_COUNT(&set) == 1 && CPU_ISSET(chosen_cpu, &set);
    atomic_fetch_add_explicit(&ready, 1, memory_order_release);
    while (!atomic_load_explicit(&start_gate, memory_order_acquire))
        sched_yield();
    const uint64_t start_cpu = thread_cpu_ns();
    data->first_tick = tick_now();
    while (!atomic_load_explicit(&stop_gate, memory_order_acquire))
        ++data->iterations;
    const uint64_t end_cpu = thread_cpu_ns();
    data->cpu_ns = end_cpu >= start_cpu ? end_cpu - start_cpu : 0;
    return NULL;
}
#endif

int main(void)
{
    uint64_t t0 = 0, t1 = 0;
    int failed = 0;
#ifdef _WIN32
    if (!QueryPerformanceFrequency(&clock_frequency) ||
        clock_frequency.QuadPart <= 0)
        return 2;
    PROCESSOR_NUMBER number = {0};
    GetCurrentProcessorNumberEx(&number);
    chosen.Group = number.Group;
    chosen.Mask = (KAFFINITY)UINT64_C(1) << number.Number;
    HANDLE threads[2] = {0};
    for (int i = 0; i < 2; ++i) {
        threads[i] = CreateThread(NULL, 0, worker_main, &workers[i],
                                  CREATE_SUSPENDED, NULL);
        if (threads[i] == NULL || !SetThreadGroupAffinity(threads[i], &chosen, NULL))
            return 3;
        if (ResumeThread(threads[i]) != 1)
            return 4;
    }
    for (int i = 0; i < 5000 && ready != 2; ++i)
        Sleep(1);
    if (ready != 2)
        return 5;
    t0 = tick_now();
    InterlockedExchange(&start_gate, 1);
    Sleep(500);
    InterlockedExchange(&stop_gate, 1);
    t1 = tick_now();
    if (WaitForMultipleObjects(2, threads, TRUE, 5000) != WAIT_OBJECT_0)
        return 6;
    for (int i = 0; i < 2; ++i) {
        FILETIME created, exited, kernel, user;
        if (!GetThreadTimes(threads[i], &created, &exited, &kernel, &user))
            failed = 1;
        else
            workers[i].cpu_ns = (ft_100ns(kernel) + ft_100ns(user)) * UINT64_C(100);
        if (!CloseHandle(threads[i]))
            failed = 1;
    }
#else
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
        return 2;
    chosen_cpu = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, &allowed)) {
            chosen_cpu = cpu;
            break;
        }
    if (chosen_cpu < 0)
        return 3;
    cpu_set_t single;
    CPU_ZERO(&single);
    CPU_SET(chosen_cpu, &single);
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0)
        return 4;
    if (pthread_attr_setaffinity_np(&attr, sizeof(single), &single) != 0)
        return 5;
    pthread_t threads[2];
    for (int i = 0; i < 2; ++i)
        if (pthread_create(&threads[i], &attr, worker_main, &workers[i]) != 0)
            return 6;
    pthread_attr_destroy(&attr);
    for (int i = 0; i < 5000 &&
         atomic_load_explicit(&ready, memory_order_acquire) != 2; ++i) {
        const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000L};
        (void)nanosleep(&pause, NULL);
    }
    if (atomic_load_explicit(&ready, memory_order_acquire) != 2)
        return 7;
    t0 = tick_now();
    atomic_store_explicit(&start_gate, 1, memory_order_release);
    const struct timespec duration = {.tv_sec = 0, .tv_nsec = 500000000L};
    (void)nanosleep(&duration, NULL);
    atomic_store_explicit(&stop_gate, 1, memory_order_release);
    t1 = tick_now();
    for (int i = 0; i < 2; ++i)
        if (pthread_join(threads[i], NULL) != 0)
            failed = 1;
#endif
    if (t1 < t0 || t0 == 0 || workers[0].first_tick < t0 ||
        workers[1].first_tick < t0 || !workers[0].pin_valid ||
        !workers[1].pin_valid || workers[0].cpu_ns == 0 ||
        workers[1].cpu_ns == 0 || workers[0].iterations == 0 ||
        workers[1].iterations == 0)
        failed = 1;
    const uint64_t min_cpu = workers[0].cpu_ns < workers[1].cpu_ns ?
                              workers[0].cpu_ns : workers[1].cpu_ns;
    const uint64_t max_cpu = workers[0].cpu_ns > workers[1].cpu_ns ?
                              workers[0].cpu_ns : workers[1].cpu_ns;
    const double fairness = max_cpu != 0 ? (double)min_cpu / (double)max_cpu : 0.0;
    const uint64_t wake0 = workers[0].first_tick >= t0 ?
        tick_to_ns(workers[0].first_tick - t0) / 1000 : 0;
    const uint64_t wake1 = workers[1].first_tick >= t0 ?
        tick_to_ns(workers[1].first_tick - t0) / 1000 : 0;
    printf("SCHEMA=ntnative.stage4.quantum-observation.v1\n");
#ifdef _WIN32
    printf("PLATFORM=WINDOWS_NATIVE\n");
    printf("CPU_GROUP=%u\nCPU_SLOT=%u\n",
           (unsigned)chosen.Group, (unsigned)number.Number);
#else
    printf("PLATFORM=LINUX_HOST\n");
    printf("LINUX_CPU_ID=%d\n", chosen_cpu);
#endif
    printf("PINNED_ONE_CPU=%s\n", !failed ? "YES" : "NO");
    printf("CONTENDERS=2\n");
    printf("WALL_NS=%llu\n", (unsigned long long)tick_to_ns(t1 - t0));
    printf("THREAD0_CPU_NS=%llu\n", (unsigned long long)workers[0].cpu_ns);
    printf("THREAD1_CPU_NS=%llu\n", (unsigned long long)workers[1].cpu_ns);
    printf("WAKE0_US=%llu\nWAKE1_US=%llu\n",
           (unsigned long long)wake0, (unsigned long long)wake1);
    printf("FAIRNESS_RATIO=%.5f\n", fairness);
    printf("NT_QUANTUM_VERIFIED=NO\n");
    printf("SCHEDULER_EQUIVALENCE=NOT_DEMONSTRATED\n");
    printf("RESULT=%s\n", failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
