#ifndef FLUX_THREADPOOL_H
#define FLUX_THREADPOOL_H

#include "conn.h"

/* Opaque. Internal layout is per-worker slots (mutex + cond + FIFO) so
 * submit() and worker pop() only contend on one slot at a time. See
 * threadpool.c for the implementation. */
typedef struct threadpool threadpool_t;

threadpool_t *threadpool_create(int thread_count);
void          threadpool_submit(threadpool_t *pool, conn_t *conn, int epoll_fd);
void          threadpool_destroy(threadpool_t *pool);

#endif
