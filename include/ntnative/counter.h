/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_COUNTER_H
#define NTNATIVE_COUNTER_H

#include <stdint.h>

typedef int32_t nt_status;

#define NT_STATUS_SUCCESS INT32_C(0)
#define NT_STATUS_UNSUCCESSFUL (-INT32_C(0x3fffffff))     /* 0xC0000001 */
#define NT_STATUS_ACCESS_VIOLATION (-INT32_C(0x3ffffffb)) /* 0xC0000005 */
#define NT_COUNTER_FREQUENCY INT64_C(10000000)

/* Host C ABI. Counter is required and frequency is optional. Non-null
 * outputs must be writable int64_t objects. Guest pointer faults are
 * outside this interface.
 */
nt_status nt_query_performance_counter(int64_t *counter, int64_t *frequency);

#endif
