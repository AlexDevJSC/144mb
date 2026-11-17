/* SPDX-License-Identifier: MIT */
/* The ACTUAL nt_runtime_run_fixture fork owns the supervisor IPC channel.
 * Execute CreateThread in its forked guest, not a stand-in IPC process. */
#define _GNU_SOURCE
#include "compat.h"
#include "sync.h"
#include "thread_broker_ipc.h"

#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

static _Atomic uint32_t actual_worker_tid;
static _Atomic int release_worker;

static uint32_t NT_MS_ABI test_guest_worker(void *unused)
{
    (void)unused;
    atomic_store_explicit(&actual_worker_tid, (uint32_t)syscall(SYS_gettid),
                          memory_order_release);
    while (!atomic_load_explicit(&release_worker, memory_order_acquire))
        sched_yield();
    return 0;
}

static int NT_MS_ABI test_guest_entry(void)
{
    if (!nt_broker_client_is_attached())
        return 10;
    struct nt_broker_thread_info main_identity = {0};
    uint64_t main_cap = 0;
    if (!nt_broker_client_supervisor_open((uint32_t)getpid(),
            NT_BROKER_SUPERVISOR_GET_CONTEXT, &main_identity, &main_cap) ||
        main_identity.guest_process_id != (uint32_t)getpid() ||
        main_identity.guest_thread_id != (uint32_t)getpid() ||
        main_identity.generation == 0 ||
        !nt_broker_client_supervisor_close(main_cap))
        return 11;
    uint32_t thread_id = 0;
    atomic_store(&actual_worker_tid, 0);
    atomic_store(&release_worker, 0);
    uintptr_t thread = nt_CreateThread(NULL, 0, test_guest_worker,
                                        NULL, 0, &thread_id);
    if (!thread || !thread_id)
        return 12;
    for (int i = 0; i < 50000 && atomic_load(&actual_worker_tid) == 0; ++i)
        sched_yield();
    if (atomic_load(&actual_worker_tid) != thread_id)
        return 13;
    struct nt_broker_thread_info identity = {0}, queried = {0};
    uint64_t capability = 0;
    uintptr_t limited = nt_OpenThread(NT_BROKER_SUPERVISOR_GET_CONTEXT, 0, thread_id);
    struct nt_broker_thread_info denied_identity = {0};
    uint64_t denied_capability = UINT64_C(0x5151);
    if (!limited || nt_runtime_supervisor_open_thread_handle(limited,
            NT_BROKER_SUPERVISOR_GET_CONTEXT | NT_BROKER_SUPERVISOR_SUSPEND,
            &denied_identity, &denied_capability) ||
        denied_capability != UINT64_C(0x5151) || !nt_CloseHandle(limited))
        return 18; /* real NT HANDLE access must not be amplified */
    if (!nt_runtime_supervisor_open_thread_handle(thread,
               NT_BROKER_SUPERVISOR_GET_CONTEXT | NT_BROKER_SUPERVISOR_SUSPEND,
               &identity, &capability) ||
        identity.guest_thread_id != thread_id ||
        identity.guest_process_id != (uint32_t)getpid() ||
        !nt_broker_client_supervisor_query(capability,
               NT_BROKER_SUPERVISOR_GET_CONTEXT, &queried) ||
        queried.generation != identity.generation)
        return 14;
    uint64_t rip = UINT64_C(0x5151);
    if (nt_broker_client_supervisor_probe(capability, &rip) ||
        rip != UINT64_C(0x5151))
        return 15; /* forbidden unless trusted parent explicitly opts in */
    if (!nt_broker_client_supervisor_close(capability))
        return 16;
    atomic_store_explicit(&release_worker, 1, memory_order_release);
    if (nt_WaitForSingleObject(thread, 2000) != 0 ||
        !nt_CloseHandle(thread))
        return 17;
    return 0;
}

int main(void)
{
    /* This toggle is exclusively available to trusted host C code, not
     * exported as a Win32 PE procedure or a guest-writeable environment. */
    nt_runtime_enable_supervisor_ipc();
    for (int i = 0; i < 2; ++i) {
        uint32_t result = 0x5151;
        if (!nt_runtime_run_fixture(test_guest_entry, &result) || result != 0) {
            fprintf(stderr, "FAIL actual PE fork supervisor session %d result=%u\n",
                    i, result);
            return 1;
        }
    }
    puts("PASS runtime supervisor bootstrap: authentic direct-fork parent,"
         " 2 independent sessions, CreateThread registration, primary/secondary"
         " owner leases, NT HANDLE rights and clean exit");
    return 0;
}
