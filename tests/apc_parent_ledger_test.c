/* SPDX-License-Identifier: MIT */
/* Parent-only ledger: these synthetic transitions cover replay/order
 * decisions, not a claim that the kernel observed guest APC execution. */
#include "apc_parent_ledger.h"
#include <stdint.h>
#include <stdio.h>
#define ACCEPT(l,t,g,c,k,p) nt_apc_parent_transition((l),(t),(g),(c),(k),(p))
#define EXPECT(expression) do { if (!(expression)) { \
    fprintf(stderr, "FAIL APC parent ledger at line %d\n", __LINE__); \
    return 1; \
} } while (0)
int main(void)
{
    struct nt_apc_parent_ledger ledger = {0};
    const uint32_t tid = 5123;
    const uint64_t generation = 55;
    const uint64_t callback = UINT64_C(0x140001200);
    EXPECT(!ACCEPT(&ledger, tid, generation, callback, 0, NT_APC_PHASE_START));
    EXPECT(!ACCEPT(&ledger, tid, 0, callback, 1, NT_APC_PHASE_START));
    EXPECT(!ACCEPT(&ledger, tid, generation, callback, 1, NT_APC_PHASE_DONE));
    EXPECT(ACCEPT(&ledger, tid, generation, callback, 1, NT_APC_PHASE_START));
    EXPECT(!ACCEPT(&ledger, tid, generation, callback, 1, NT_APC_PHASE_START));
    EXPECT(!ACCEPT(&ledger, tid, generation, callback, 2, NT_APC_PHASE_START));
    EXPECT(!ACCEPT(&ledger, tid, generation, callback + 1, 1, NT_APC_PHASE_DONE));
    EXPECT(!ACCEPT(&ledger, tid, generation + 1, callback, 1, NT_APC_PHASE_DONE));
    EXPECT(ACCEPT(&ledger, tid, generation, callback, 1, NT_APC_PHASE_DONE));
    EXPECT(!ACCEPT(&ledger, tid, generation, callback, 1, NT_APC_PHASE_DONE));
    EXPECT(!ACCEPT(&ledger, tid, generation, callback, 1, NT_APC_PHASE_START));
    EXPECT(ACCEPT(&ledger, tid, generation, callback, 2, NT_APC_PHASE_START));
    EXPECT(ACCEPT(&ledger, tid, generation, callback, 2, NT_APC_PHASE_ABORT));
    EXPECT(!ACCEPT(&ledger, tid, generation, callback, 2, NT_APC_PHASE_DONE));
    EXPECT(ACCEPT(&ledger, tid + 1, generation, callback, 3, NT_APC_PHASE_START));
    EXPECT(ACCEPT(&ledger, tid + 1, generation, callback, 3, NT_APC_PHASE_DONE));
    /* A reused TID is a distinct parent registry generation. The old
     * generation's record is invalidated by the broker before this point. */
    EXPECT(ACCEPT(&ledger, tid, generation + 1, callback, 3, NT_APC_PHASE_START));
    EXPECT(!ACCEPT(&ledger, tid, generation, callback, 3, NT_APC_PHASE_DONE));
    EXPECT(ACCEPT(&ledger, tid, generation + 1, callback, 3, NT_APC_PHASE_DONE));
    puts("PASS parent APC ledger: generation, ordering, replay, abort, single active callback");
    return 0;
}
