/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "ntnative/runtime.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define KERNEL "KERNEL32.dll"
#define VCRUNTIME "VCRUNTIME140.dll"
#define UCRT(group) "api-ms-win-crt-" group "-l1-1-0.dll"

static void check(bool condition, const char *label)
{
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", label);
        exit(EXIT_FAILURE);
    }
}

static nt_guest_proc require_export(const char *module, const char *name)
{
    const nt_guest_proc target = nt_runtime_resolve(module, name);
    if (target == NULL) {
        fprintf(stderr, "FAIL missing %s!%s\n", module, name);
        exit(EXIT_FAILURE);
    }
    return target;
}

#define BIND(target, module, name) do {                                     \
    const nt_guest_proc address = require_export(module, name);            \
    _Static_assert(sizeof(target) == sizeof(address), "guest pointer size"); \
    memcpy(&(target), &address, sizeof(target));                           \
} while (0)

struct error_functions {
    uint32_t (NT_MS_ABI *get)(void);
    void (NT_MS_ABI *set)(uint32_t);
};

static void *check_thread_error(void *context)
{
    const struct error_functions *error = context;
    check(error->get() == 0, "new thread starts with an independent last error");
    error->set(19);
    check(error->get() == 19, "new thread round trips its last error");
    return NULL;
}

static void check_win32(void)
{
    uint32_t (NT_MS_ABI *process_id)(void);
    uint32_t (NT_MS_ABI *thread_id)(void);
    int32_t (NT_MS_ABI *counter)(int64_t *);
    int32_t (NT_MS_ABI *mul_div)(int32_t, int32_t, int32_t);
    struct error_functions error;
    BIND(process_id, KERNEL, "GetCurrentProcessId");
    BIND(thread_id, KERNEL, "GetCurrentThreadId");
    BIND(error.get, KERNEL, "GetLastError");
    BIND(error.set, KERNEL, "SetLastError");
    BIND(counter, KERNEL, "QueryPerformanceCounter");
    BIND(mul_div, KERNEL, "MulDiv");

    error.set(UINT32_C(0xfedcba98));
    check(error.get() == UINT32_C(0xfedcba98), "last error preserves all 32 bits");
    pthread_t thread;
    check(pthread_create(&thread, NULL, check_thread_error, &error) == 0, "create test thread");
    check(pthread_join(thread, NULL) == 0, "join test thread");
    check(error.get() == UINT32_C(0xfedcba98), "last error stays with the calling thread");
    check(process_id() == (uint32_t)getpid(), "process ID");
    check(thread_id() == (uint32_t)gettid(), "thread ID");
    check(nt_runtime_resolve("kernel32.dll", "queryperformancecounter") == NULL,
          "export names are case sensitive");
    check(nt_runtime_resolve("user32.dll", "QueryPerformanceCounter") == NULL,
          "export lookup is scoped to its module");
    check(nt_runtime_resolve("comctl32.dll", NULL) == NULL, "ordinal remains unresolved");
    int64_t first = -1;
    int64_t second = -1;
    check(counter(&first) != 0 && first >= 0, "Win32 counter writes a valid value");
    check(counter(&second) != 0 && second >= first, "Win32 counter is nondecreasing");
    check(mul_div(5, 1, 2) == 3 && mul_div(-5, 1, 2) == -3 &&
              mul_div(5, 1, -2) == -3 && mul_div(6, 2, 4) == 3,
          "MulDiv rounds halves away from zero");
    check(mul_div(1, 2, 0) == -1 && mul_div(INT32_MAX, 2, 1) == -1,
          "MulDiv rejects zero denominator and overflow");
}

static void check_memory(void)
{
    void *(NT_MS_ABI *find_byte)(const void *, int, size_t);
    int (NT_MS_ABI *compare)(const void *, const void *, size_t);
    void *(NT_MS_ABI *copy)(void *, const void *, size_t);
    void *(NT_MS_ABI *move)(void *, const void *, size_t);
    void *(NT_MS_ABI *fill)(void *, int, size_t);
    char *(NT_MS_ABI *first)(const char *, int);
    char *(NT_MS_ABI *last)(const char *, int);
    BIND(find_byte, VCRUNTIME, "memchr");
    BIND(compare, VCRUNTIME, "memcmp");
    BIND(copy, VCRUNTIME, "memcpy");
    BIND(move, VCRUNTIME, "memmove");
    BIND(fill, VCRUNTIME, "memset");
    BIND(first, VCRUNTIME, "strchr");
    BIND(last, VCRUNTIME, "strrchr");

    char data[7];
    check(fill(data, 'z', sizeof(data)) == data && data[6] == 'z', "memset");
    check(copy(data, "abcabc", sizeof(data)) == data && strcmp(data, "abcabc") == 0,
          "memcpy");
    check(find_byte(data, 'b', 6) == data + 1 && find_byte(data, 'z', 6) == NULL,
          "memchr");
    check(compare(data, "abcabd", 6) < 0 && compare(data, "abcabc", 6) == 0,
          "memcmp");
    check(first(data, 'b') == data + 1 && first(data, '\0') == data + 6, "strchr");
    check(last(data, 'b') == data + 4 && last(data, '\0') == data + 6, "strrchr");
    check(move(data + 1, data, 5) == data + 1 && strcmp(data, "aabcab") == 0,
          "memmove with overlapping ranges");
}

static void check_heap(void)
{
    void *(NT_MS_ABI *allocate)(size_t);
    void *(NT_MS_ABI *resize)(void *, size_t);
    void (NT_MS_ABI *release)(void *);
    BIND(allocate, UCRT("heap"), "malloc");
    BIND(resize, UCRT("heap"), "realloc");
    BIND(release, UCRT("heap"), "free");

    unsigned char *memory = allocate(16);
    check(memory != NULL && (uintptr_t)memory % 16 == 0, "malloc alignment");
    memory[0] = 42;
    memory[15] = 95;
    unsigned char *larger = resize(memory, 64);
    check(larger != NULL && larger[0] == 42 && larger[15] == 95, "realloc preserves bytes");
    check(resize(larger, 0) == NULL, "realloc with zero size releases the allocation");
    memory = resize(NULL, 8);
    check(memory != NULL, "realloc with a null pointer allocates");
    release(memory);
    memory = allocate(0);
    check(memory != NULL, "zero-size malloc returns a releasable block");
    release(memory);
    release(NULL);
}

static void check_math(void)
{
    double (NT_MS_ABI *round_up)(double);
    double (NT_MS_ABI *round_down)(double);
    float (NT_MS_ABI *round_single)(float);
    double (NT_MS_ABI *root)(double);
    float (NT_MS_ABI *root_single)(float);
    BIND(round_up, UCRT("math"), "ceil");
    BIND(round_down, UCRT("math"), "floor");
    BIND(round_single, UCRT("math"), "roundf");
    BIND(root, UCRT("math"), "sqrt");
    BIND(root_single, UCRT("math"), "sqrtf");

    check(round_up(-1.25) == -1.0, "ceil");
    check(round_down(-1.25) == -2.0, "floor");
    check(round_single(-2.5f) == -3.0f, "roundf");
    check(root(81.0) == 9.0, "sqrt");
    check(root_single(81.0f) == 9.0f, "sqrtf");
}

static void check_strings(void)
{
    char *(NT_MS_ABI *append)(char *, const char *);
    int (NT_MS_ABI *compare)(const char *, const char *);
    char *(NT_MS_ABI *copy)(char *, const char *);
    size_t (NT_MS_ABI *reject_span)(const char *, const char *);
    size_t (NT_MS_ABI *length)(const char *);
    char *(NT_MS_ABI *copy_padded)(char *, const char *, size_t);
    size_t (NT_MS_ABI *accept_span)(const char *, const char *);
    size_t (NT_MS_ABI *wide_length)(const uint16_t *);
    BIND(append, UCRT("string"), "strcat");
    BIND(compare, UCRT("string"), "strcmp");
    BIND(copy, UCRT("string"), "strcpy");
    BIND(reject_span, UCRT("string"), "strcspn");
    BIND(length, UCRT("string"), "strlen");
    BIND(copy_padded, UCRT("string"), "strncpy");
    BIND(accept_span, UCRT("string"), "strspn");
    BIND(wide_length, UCRT("string"), "wcslen");

    char text[8];
    check(copy(text, "ab") == text && strcmp(text, "ab") == 0, "strcpy");
    check(append(text, "cd") == text && strcmp(text, "abcd") == 0, "strcat");
    check(compare(text, "abcd") == 0 && compare(text, "abce") < 0, "strcmp");
    check(reject_span(text, "cx") == 2, "strcspn");
    check(length(text) == 4, "strlen");
    check(accept_span(text, "abx") == 2, "strspn");
    check(copy_padded(text, "ab", 4) == text && memcmp(text, "ab\0\0", 4) == 0,
          "strncpy pads with nulls");
    const uint16_t wide[] = {0x0041, 0xd83d, 0xde00, 0};
    check(wide_length(wide) == 3, "wcslen counts UTF-16 code units");
}

static int NT_MS_ABI compare_ints(const void *left, const void *right)
{
    const int a = *(const int *)left;
    const int b = *(const int *)right;
    return (a > b) - (a < b);
}

static void check_sort(void)
{
    typedef int (NT_MS_ABI *compare_fn)(const void *, const void *);
    void (NT_MS_ABI *sort)(void *, size_t, size_t, compare_fn);
    BIND(sort, UCRT("utility"), "qsort");

    int values[] = {2, 1, -4, 2, 0};
    const int expected[] = {-4, 0, 1, 2, 2};
    sort(values, sizeof(values) / sizeof(values[0]), sizeof(values[0]), compare_ints);
    check(memcmp(values, expected, sizeof(values)) == 0, "qsort calls the guest comparator");
}

int main(void)
{
    check_win32();
    check_memory();
    check_heap();
    check_math();
    check_strings();
    check_sort();
    puts("PASS 30 Win32 and CRT exports through the Microsoft x64 ABI");
    return EXIT_SUCCESS;
}
