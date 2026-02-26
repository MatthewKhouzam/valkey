/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
/* ==========================================================================
 * trace_shmem.h - support lttng tracing for shared memory connection events.
 * ==========================================================================
 */

#ifdef USE_LTTNG

#undef LTTNG_UST_TRACEPOINT_PROVIDER
#define LTTNG_UST_TRACEPOINT_PROVIDER valkey_shmem

#undef LTTNG_UST_TRACEPOINT_INCLUDE
#define LTTNG_UST_TRACEPOINT_INCLUDE "./trace_shmem.h"

#if !defined(__VALKEY_TRACE_SHMEM_H__) || defined(LTTNG_UST_TRACEPOINT_HEADER_MULTI_READ)
#define __VALKEY_TRACE_SHMEM_H__

#include <lttng/tracepoint.h>

/* Handshake completed (server accepted a new shmem client) */
LTTNG_UST_TRACEPOINT_EVENT(
    valkey_shmem,
    accept,
    LTTNG_UST_TP_ARGS(
        const char *, shm_name,
        uint64_t, duration
    ),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_string(shm_name, shm_name)
        lttng_ust_field_integer(uint64_t, duration, duration)
    )
)

/* Data written to ring buffer */
LTTNG_UST_TRACEPOINT_EVENT(
    valkey_shmem,
    write,
    LTTNG_UST_TP_ARGS(
        const char *, shm_name,
        uint64_t, bytes
    ),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_string(shm_name, shm_name)
        lttng_ust_field_integer(uint64_t, bytes, bytes)
    )
)

/* Data read from ring buffer */
LTTNG_UST_TRACEPOINT_EVENT(
    valkey_shmem,
    read,
    LTTNG_UST_TP_ARGS(
        const char *, shm_name,
        uint64_t, bytes
    ),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_string(shm_name, shm_name)
        lttng_ust_field_integer(uint64_t, bytes, bytes)
    )
)

/* Connection closed */
LTTNG_UST_TRACEPOINT_EVENT(
    valkey_shmem,
    close,
    LTTNG_UST_TP_ARGS(
        const char *, shm_name
    ),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_string(shm_name, shm_name)
    )
)

#define valkey_shmem_trace(...) lttng_ust_tracepoint(__VA_ARGS__)

#endif /* __VALKEY_TRACE_SHMEM_H__ */

#include <lttng/tracepoint-event.h>

#else /* USE_LTTNG */

#ifndef __VALKEY_TRACE_SHMEM_H__
#define __VALKEY_TRACE_SHMEM_H__

static inline void __valkey_shmem_trace(void) {
}

#define valkey_shmem_trace(...) \
    do {                        \
    } while (0)

#endif /* __VALKEY_TRACE_SHMEM_H__ */

#endif /* USE_LTTNG */
