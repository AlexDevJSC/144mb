#ifndef BRAIN_MOD_DIAGNOSTICS_H
#define BRAIN_MOD_DIAGNOSTICS_H
/* Optional read-only Brain diagnostics. NOT part of Bones ModKit ABI 1.
   Must be called on the same thread as ModKit callbacks. */
#include <stdint.h>
#define BRAIN_MOD_DIAGNOSTICS_VERSION 1u
typedef struct {
    uint32_t size;
    uint32_t version;
    uint64_t commands_accepted;
    uint64_t commands_rejected;
    uint64_t actor_read_failures;
    uint64_t rejected_actor_counts;
    uint64_t actors_skipped_not_owned;
    uint64_t obstacle_recoveries;
} BrainModDiagnostics;
/* Returns 1 on success; 0 for NULL or mismatched out->size. */
int brain_mod_diagnostics_snapshot(BrainModDiagnostics *out);
#endif
