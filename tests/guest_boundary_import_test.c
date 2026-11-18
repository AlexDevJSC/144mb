/* SPDX-License-Identifier: MIT */
/* AMD64 Microsoft ABI regression for ALL PE->runtime import gates.
 * Proves 8 argument slots, floating returns, per-thread phase,
 * nested host import and host callback under a real Linux mutex. */
#define _GNU_SOURCE
#include "ntnative/guest_boundary_runtime.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static pthread_mutex_t runtime_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int failures;
static nt_guest_proc nested_wrapper;

static int __attribute__((ms_abi)) host_nested(int a, int b)
{
    if (nt_guest_boundary_current_phase() != NT_GUEST_BOUNDARY_HOST ||
        nt_guest_boundary_host_nesting() != 2 || a != 7 || b != 8)
        atomic_fetch_add(&failures, 1);
    return a * 10 + b;
}

static uint64_t __attribute__((ms_abi)) host_eight(uint64_t a, uint64_t b,
    uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h)
{
    if (pthread_mutex_lock(&runtime_lock) != 0)
        atomic_fetch_add(&failures, 1);
    if (nt_guest_boundary_current_phase() != NT_GUEST_BOUNDARY_HOST ||
        nt_guest_boundary_host_nesting() != 1)
        atomic_fetch_add(&failures, 1);
    /* A guest callback reached from inside an import, while a mutex is
     * retained, must NEVER become suspendible just because its RIP
     * would point at PE. Nested calls are also HOST. */
    nt_guest_boundary_guest_enter();
    if (nt_guest_boundary_current_phase() != NT_GUEST_BOUNDARY_HOST)
        atomic_fetch_add(&failures, 1);
    int (__attribute__((ms_abi)) *nested)(int,int) = NULL;
    memcpy(&nested, &nested_wrapper, sizeof(nested));
    if (!nested || nested(7,8) != 78 ||
        nt_guest_boundary_current_phase() != NT_GUEST_BOUNDARY_HOST ||
        nt_guest_boundary_host_nesting() != 1)
        atomic_fetch_add(&failures, 1);
    nt_guest_boundary_guest_leave();
    if (nt_guest_boundary_current_phase() != NT_GUEST_BOUNDARY_HOST ||
        pthread_mutex_unlock(&runtime_lock) != 0)
        atomic_fetch_add(&failures, 1);
    return a + b*2 + c*3 + d*4 + e*5 + f*6 + g*7 + h*8;
}

static double __attribute__((ms_abi)) host_float(double a, double b,
                                                   double c, double d, double e)
{
    if (nt_guest_boundary_current_phase() != NT_GUEST_BOUNDARY_HOST ||
        nt_guest_boundary_host_nesting() != 1)
        atomic_fetch_add(&failures, 1);
    return a*3.0 + b*5.0 + c*7.0 + d*11.0 + e*13.0;
}

static nt_guest_proc gate_function(void (*native)(void))
{
    nt_guest_proc target;
    memcpy(&target, &native, sizeof(target));
    return nt_guest_boundary_wrap_import(target);
}

static int test_calls(void)
{
    void (*native)(void) = NULL;
    int (__attribute__((ms_abi)) *nested)(int,int) = host_nested;
    memcpy(&native, &nested, sizeof(native));
    nested_wrapper = gate_function(native);
    if (!nested_wrapper) return 1;
    uint64_t (__attribute__((ms_abi)) *eight)(uint64_t,uint64_t,uint64_t,uint64_t,
                                               uint64_t,uint64_t,uint64_t,uint64_t) = host_eight;
    memcpy(&native, &eight, sizeof(native));
    nt_guest_proc wrapped_eight = gate_function(native);
    if (!wrapped_eight) return 2;
    double (__attribute__((ms_abi)) *floater)(double,double,double,double,double) = host_float;
    memcpy(&native, &floater, sizeof(native));
    nt_guest_proc wrapped_float = gate_function(native);
    if (!wrapped_float) return 3;
    memcpy(&eight, &wrapped_eight, sizeof(eight));
    memcpy(&floater, &wrapped_float, sizeof(floater));
    nt_guest_boundary_guest_enter();
    if (nt_guest_boundary_current_phase() != NT_GUEST_BOUNDARY_GUEST ||
        !nt_guest_boundary_special_apc_allowed())
        return 4;
    for (unsigned i = 0; i < 50; ++i) {
        const uint64_t integer = eight(1,2,3,4,5,6,7,8);
        const double floating = floater(1.0,2.0,3.0,4.0,5.0);
        if (integer != 204 || fabs(floating - 143.0) > 0.00001 ||
            nt_guest_boundary_current_phase() != NT_GUEST_BOUNDARY_GUEST ||
            nt_guest_boundary_host_nesting() != 0)
            return 5;
    }
    nt_guest_boundary_guest_leave();
    if (nt_guest_boundary_current_phase() != NT_GUEST_BOUNDARY_HOST ||
        nt_guest_boundary_special_apc_allowed())
        return 6;
    return 0;
}

static void *second_thread(void *arg)
{
    (void)arg;
    if (!nt_guest_boundary_thread_attach(UINT64_C(0x55550000))) {
        atomic_fetch_add(&failures, 1);
        return NULL;
    }
    if (test_calls())
        atomic_fetch_add(&failures, 1);
    nt_guest_boundary_thread_detach();
    return NULL;
}

int main(void)
{
    struct nt_guest_boundary_session *session = mmap(NULL, sizeof(*session),
        PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    if (session == MAP_FAILED) return 1;
    memset(session, 0, sizeof(*session));
    nt_guest_boundary_enable_import_gate();
    nt_guest_boundary_install_child_session(session);
    if (!nt_guest_boundary_thread_attach(UINT64_C(0x44440000))) return 2;
    const struct nt_guest_boundary_slot *slot =
        nt_guest_boundary_find_thread(session, (pid_t)syscall(SYS_gettid));
    if (!slot || atomic_load(&slot->teb) != UINT64_C(0x44440000)) return 3;
    const int first = test_calls();
    if (first) {
        fprintf(stderr, "FAIL import boundary ABI/phase main=%d\n", first);
        return 4;
    }
    pthread_t second;
    if (pthread_create(&second, NULL, second_thread, NULL) != 0 ||
        pthread_join(second, NULL) != 0)
        return 5;
    nt_guest_boundary_mark_nonlocal_transfer();
    if (!nt_guest_boundary_nonlocal_poisoned() ||
        nt_guest_boundary_current_phase() != NT_GUEST_BOUNDARY_HOST)
        return 8;
    nt_guest_boundary_guest_enter();
    if (nt_guest_boundary_current_phase() != NT_GUEST_BOUNDARY_HOST)
        return 9; /* nonlocal VEH/SEH may never reauthorise async stops */
    nt_guest_boundary_guest_leave();
    nt_guest_boundary_thread_detach();
    if (nt_guest_boundary_nonlocal_poisoned() ||
        nt_guest_boundary_find_thread(session, (pid_t)syscall(SYS_gettid)) ||
        atomic_load(&failures) != 0)
        return 6;
    /* A fresh TID generation/attachment is not poisoned by the old one. */
    if (!nt_guest_boundary_thread_attach(UINT64_C(0x66660000))) return 10;
    nt_guest_boundary_guest_enter();
    if (nt_guest_boundary_current_phase() != NT_GUEST_BOUNDARY_GUEST) return 11;
    nt_guest_boundary_guest_leave();
    nt_guest_boundary_thread_detach();
    if (munmap(session, sizeof(*session)) != 0) return 7;
    /* The trusted launcher must only accept disjoint, bounded executable
     * ranges supplied before fork. Failed updates are atomic and the
     * prior manifest remains intact. */
    if (!nt_guest_boundary_set_image_range(0x100000u, 0x102000u))
        return 12;
    const struct nt_guest_code_range startup[] = {
        {0x200000u, 0x201000u}, {0x300000u, 0x303000u}
    };
    if (!nt_guest_boundary_set_startup_code_ranges(startup, 2))
        return 13;
    const struct nt_guest_code_range invalid[] = {
        {0x200000u, 0x201000u}, {0x200800u, 0x202000u}
    };
    if (nt_guest_boundary_set_startup_code_ranges(invalid, 2))
        return 14;
    const struct nt_guest_code_range main_overlap[] = {
        {0x101000u, 0x103000u}
    };
    if (nt_guest_boundary_set_startup_code_ranges(main_overlap, 1))
        return 15;
    struct nt_guest_code_range snapshot[NT_GUEST_ADDITIONAL_CODE_MAX] = {{0}};
    size_t count = 0;
    if (!nt_guest_boundary_get_startup_code_ranges(snapshot,
                  NT_GUEST_ADDITIONAL_CODE_MAX, &count) ||
        count != 2 || memcmp(startup, snapshot, sizeof(startup)) != 0)
        return 16;
    if (!nt_guest_boundary_set_image_range(0x400000u, 0x401000u) ||
        !nt_guest_boundary_get_startup_code_ranges(snapshot,
                  NT_GUEST_ADDITIONAL_CODE_MAX, &count) || count != 0)
        return 17; /* a new PE image revokes every old DLL range */
    /* The normal, non-supervised PE runner has no MAP_SHARED slot;
     * the LOCAL import-return checkpoint still authorises callbacks.
     * It must never pretend this is an external ptrace GUEST phase. */
    nt_guest_boundary_install_child_session(NULL);
    if (nt_guest_boundary_special_apc_allowed())
        return 18;
    nt_guest_boundary_guest_enter();
    if (nt_guest_boundary_current_phase() != NT_GUEST_BOUNDARY_UNREGISTERED ||
        !nt_guest_boundary_special_apc_allowed())
        return 19;
    nt_guest_boundary_guest_leave();
    if (nt_guest_boundary_special_apc_allowed())
        return 20;
    /* A direct, known PE RaiseException import may have its return slot
     * rewritten by our gate. Only one non-nested host frame may be
     * translated back to the loader-proven PE address. No generic ELF
     * stack traversal or callback under a held host mutex is allowed. */
    void (*epilogue)(void) = nt_guest_boundary_import_return;
    uintptr_t synthetic = 0;
    memcpy(&synthetic, &epilogue, sizeof(synthetic));
    if (nt_guest_boundary_seh_caller_pc(synthetic) != synthetic)
        return 21;
    nt_guest_boundary_guest_enter();
    const uintptr_t approved = 0x400100u;
    const uintptr_t unapproved = 0x500100u;
    if (nt_guest_boundary_import_enter(1, approved) != 1 ||
        nt_guest_boundary_seh_caller_pc(synthetic) != approved ||
        nt_guest_boundary_seh_caller_pc(synthetic + 1) != synthetic + 1)
        return 22;
    if (nt_guest_boundary_import_enter(1, approved) != 1 ||
        nt_guest_boundary_seh_caller_pc(synthetic) != synthetic)
        return 23;
    if (nt_guest_boundary_import_leave() != approved ||
        nt_guest_boundary_seh_caller_pc(synthetic) != approved)
        return 24;
    if (nt_guest_boundary_import_leave() != approved ||
        nt_guest_boundary_seh_caller_pc(synthetic) != synthetic)
        return 25;
    if (nt_guest_boundary_import_enter(1, unapproved) != 1 ||
        nt_guest_boundary_seh_caller_pc(synthetic) != synthetic ||
        nt_guest_boundary_import_leave() != unapproved)
        return 26;
    nt_guest_boundary_guest_leave();
    if (nt_guest_boundary_seh_caller_pc(synthetic) != synthetic)
        return 27;
    puts("PASS guest/host import ABI: Microsoft x64 8-argument stack, XMM float, "
         "nested callbacks with host mutex, nonlocal poison, pinned-DLL manifest, per-thread phases and revocation");
    return 0;
}
