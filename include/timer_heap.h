#ifndef FLUX_TIMER_HEAP_H
#define FLUX_TIMER_HEAP_H

#include <time.h>

/*
 * Indexed binary min-heap of connection deadlines.
 *
 *   push   O(log N)   on accept
 *   update O(log N)   on each activity event (last_active refresh)
 *   remove O(log N)   on connection reap
 *   peek   O(1)       to re-arm timerfd
 *   drain  O(K log N) where K = number of expired entries
 *
 * Replaces the naïve `for (fd in 0..65535) check last_active` sweep,
 * which paid O(N) on every tick whether or not anything was expired.
 *
 * Identity is `conn_t *` — each conn stores its `heap_idx` so updates and
 * removals don't have to scan. The `conn_t` <-> `timer_heap_t` cycle is
 * broken with forward declarations here; only timer_heap.c includes conn.h.
 */
struct conn;
typedef struct conn conn_t;

typedef struct timer_heap timer_heap_t;

timer_heap_t *timer_heap_create(int cap);
void          timer_heap_destroy(timer_heap_t *h);

/* Insert. Sets c->heap_idx. Returns -1 if full. */
int  timer_heap_push(timer_heap_t *h, conn_t *c, struct timespec deadline);

/* Re-key an existing entry. Returns -1 if c is not currently in the heap. */
int  timer_heap_update(timer_heap_t *h, conn_t *c, struct timespec deadline);

/* Remove `c` from the heap. Idempotent (returns 0 if not present). */
int  timer_heap_remove(timer_heap_t *h, conn_t *c);

int  timer_heap_size(const timer_heap_t *h);

/* Read the earliest deadline without popping. Returns -1 if empty. */
int  timer_heap_peek(const timer_heap_t *h, struct timespec *out);

/* Pop every entry with deadline <= now, invoking cb for each. */
void timer_heap_pop_expired(timer_heap_t *h, struct timespec now,
                            void (*cb)(conn_t *, void *), void *ud);

#endif
