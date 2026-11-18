/* SPDX-License-Identifier: MIT */
/* A REAL PE32+ thread spins forever without Windows imports or alertable
 * waits. A special APC MUST run on that exact thread while its RIP
 * remains inside this import-free loop, not at a synthetic safe point.
 * Failure cannot be misreported as a normal QueueUserAPC delivery. */
#include <windows.h>
#include <stdint.h>

struct async_target {
    volatile uint32_t in_guest_spin;
    volatile uint32_t special_calls;
    volatile uint32_t ordinary_calls;
    volatile uint32_t worker_tid, callback_tid;
};
static void CALLBACK ordinary_apc(ULONG_PTR arg)
{
    struct async_target *state = (struct async_target *)arg;
    __atomic_add_fetch(&state->ordinary_calls, 1u, __ATOMIC_SEQ_CST);
}
static void CALLBACK special_apc(ULONG_PTR arg)
{
    struct async_target *state = (struct async_target *)arg;
    __atomic_store_n(&state->callback_tid, GetCurrentThreadId(),
                     __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&state->special_calls, 1u, __ATOMIC_SEQ_CST);
}
static DWORD WINAPI pure_pe_loop(void *arg)
{
    struct async_target *state = arg;
    __atomic_store_n(&state->worker_tid, GetCurrentThreadId(),
                     __ATOMIC_SEQ_CST);
    __atomic_store_n(&state->in_guest_spin, 1u, __ATOMIC_SEQ_CST);
    /* This loop must not contain ANY Win32 import, syscalls or yield.
     * Without a real asynchronous APC dispatch, no callback is possible
     * before returning from this thread function. */
    for (uint64_t spin = 0; spin < UINT64_C(900000000); ++spin) {
        if (__atomic_load_n(&state->special_calls, __ATOMIC_SEQ_CST) != 0)
            break;
        __asm__ __volatile__("pause" ::: "memory");
    }
    if (__atomic_load_n(&state->special_calls, __ATOMIC_SEQ_CST) != 1u ||
        __atomic_load_n(&state->ordinary_calls, __ATOMIC_SEQ_CST) != 0u ||
        __atomic_load_n(&state->callback_tid, __ATOMIC_SEQ_CST) !=
            __atomic_load_n(&state->worker_tid, __ATOMIC_SEQ_CST))
        return 103;
    return 0x1acu;
}
int import_fixture_entry(void)
{
    typedef DWORD (WINAPI *queue2)(PAPCFUNC, HANDLE, ULONG_PTR, DWORD);
    union { FARPROC raw; queue2 api; } resolved = {
        .raw = GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                              "QueueUserAPC2")
    };
    if (!resolved.api)
        return 100;
    struct async_target state = {0};
    DWORD tid = 0, exit_code = 0;
    HANDLE thread = CreateThread(NULL, 0, pure_pe_loop, &state, 0, &tid);
    if (!thread || tid == 0)
        return 101;
    bool entered = false;
    for (uint64_t spin = 0; spin < UINT64_C(900000000); ++spin) {
        if (__atomic_load_n(&state.in_guest_spin, __ATOMIC_SEQ_CST)) {
            entered = true;
            break;
        }
        __asm__ __volatile__("pause" ::: "memory");
    }
    if (!entered)
        return 102;
    /* A normal APC must remain pending: no alertable wait ever occurs. */
    if (!QueueUserAPC(ordinary_apc, thread, (ULONG_PTR)&state) ||
        !resolved.api(special_apc, thread, (ULONG_PTR)&state, 1u))
        return 104;
    if (WaitForSingleObject(thread, 12000) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(thread, &exit_code) || exit_code != 0x1acu ||
        __atomic_load_n(&state.ordinary_calls, __ATOMIC_SEQ_CST) != 0u ||
        __atomic_load_n(&state.special_calls, __ATOMIC_SEQ_CST) != 1u ||
        __atomic_load_n(&state.callback_tid, __ATOMIC_SEQ_CST) != tid ||
        !CloseHandle(thread))
        return 105;
    return 0;
}
