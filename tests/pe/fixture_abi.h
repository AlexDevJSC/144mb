/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_FIXTURE_ABI_H
#define NTNATIVE_FIXTURE_ABI_H

#include "ntnative/counter.h"

#if !defined(__x86_64__) || !defined(__GNUC__)
#error The PE fixture needs an x86-64 compiler with GNU ms_abi support
#endif

#define NT_MS_ABI __attribute__((ms_abi))

typedef nt_status (NT_MS_ABI *nt_fixture_query)(int64_t *, int64_t *);
typedef int (NT_MS_ABI *nt_fixture_entry)(nt_fixture_query);

#endif
