/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_FIXTURE_ABI_H
#define NTNATIVE_FIXTURE_ABI_H

#include "ntnative/counter.h"
#include "ntnative/runtime.h"

typedef nt_status (NT_MS_ABI *nt_fixture_query)(int64_t *, int64_t *);
typedef int (NT_MS_ABI *nt_fixture_entry)(nt_fixture_query);

static inline nt_status NT_MS_ABI query_from_pe(int64_t *counter, int64_t *frequency)
{
    return nt_query_performance_counter(counter, frequency);
}

#endif
