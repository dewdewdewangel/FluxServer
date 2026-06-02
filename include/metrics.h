#ifndef FLUX_METRICS_H
#define FLUX_METRICS_H

#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

/*
 * Lock-free server-wide counters. All workers + the reactor share these via
 * relaxed atomic operations — exact-ordering doesn't matter for a /stats
 * snapshot, and we don't want the contention of a mutex on the hot path.
 */
typedef struct {
    _Atomic uint64_t connections_accepted;
    _Atomic uint64_t connections_active;
    _Atomic uint64_t requests_total;
    _Atomic uint64_t responses_2xx;
    _Atomic uint64_t responses_4xx;
    _Atomic uint64_t responses_5xx;
    _Atomic uint64_t bytes_in;
    _Atomic uint64_t bytes_out;
    _Atomic uint64_t timeouts;

    struct timespec  started_at; /* CLOCK_MONOTONIC, written once at startup */
} metrics_t;

extern metrics_t g_metrics;

void metrics_init(void);

/* Counter helpers — relaxed-order increments, plenty fast on the hot path. */
static inline void metrics_inc(_Atomic uint64_t *c) {
    atomic_fetch_add_explicit(c, 1, memory_order_relaxed);
}
static inline void metrics_add(_Atomic uint64_t *c, uint64_t n) {
    atomic_fetch_add_explicit(c, n, memory_order_relaxed);
}
static inline void metrics_dec(_Atomic uint64_t *c) {
    atomic_fetch_sub_explicit(c, 1, memory_order_relaxed);
}
static inline uint64_t metrics_get(const _Atomic uint64_t *c) {
    return atomic_load_explicit(c, memory_order_relaxed);
}

/* Bucket a numeric status into 2xx / 4xx / 5xx. */
void metrics_record_response(int status);

/* Seconds since metrics_init(). */
uint64_t metrics_uptime_seconds(void);

#endif
