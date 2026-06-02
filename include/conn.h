#ifndef FLUX_CONN_H
#define FLUX_CONN_H

#include <netinet/in.h>
#include <stdatomic.h>
#include <stddef.h>
#include <time.h>

#include "buf.h"
#include "http_parser.h"

/* fd is used as the connection-table index, so this bounds it. */
#define FLUX_MAX_CONN 65536

/*
 * Connection lifecycle (Task 4 — fd-UAF fix).
 *
 *   ACTIVE  -- worker / reactor are operating normally on the fd
 *   CLOSING -- exactly one transition; conn is in the close queue;
 *              the reactor thread is the *only* thread that may close(fd)
 *              and free conn_t once `in_worker` reads 0.
 *
 * The CAS in conn_close_async() makes the transition idempotent — a
 * worker discovering EOF and the sweep timing out simultaneously can both
 * race to close; only the winner enqueues, the loser silently returns.
 */
typedef enum {
    CONN_ACTIVE  = 0,
    CONN_CLOSING = 1,
} conn_state_t;

/* Forward decl breaks the conn <-> timer_heap include cycle. */
struct timer_heap;
typedef struct timer_heap timer_heap_t;

typedef struct conn {
    int                    fd;
    _Atomic int            state;       /* conn_state_t */
    _Atomic int            in_worker;   /* 1 while a worker thread owns it */
    struct sockaddr_in     peer;
    struct timespec        last_active; /* CLOCK_MONOTONIC */
    buf_t                  rx;
    buf_t                  tx;
    http_request_t         req;         /* incremental HTTP parser state */
    int                    want_write;  /* EPOLLOUT armed */
    int                    read_closed; /* peer sent FIN; drain tx then close */
    int                    heap_idx;    /* position in timer_heap, -1 if absent */
    struct conn           *next_close;  /* close-queue link */
} conn_t;

/* Table-based registry. Returns NULL on alloc fail / dup / out-of-range fd. */
conn_t *conn_create(int fd, const struct sockaddr_in *peer);

/* Look up by fd. Returns NULL if not registered. */
conn_t *conn_get(int fd);

/* Free conn_t + unregister from the table.
 * Caller is responsible for the fd lifecycle (close + epoll_ctl(DEL)).
 * In normal operation only conn_drain_closed() calls this. */
void    conn_destroy(conn_t *c);

/* Visit every live connection. Stops early if cb returns non-zero. */
void    conn_foreach(int (*cb)(conn_t *c, void *ud), void *ud);

/* Atomically mark for close + enqueue. Idempotent.
 * Returns 1 if this call performed the transition. */
int     conn_close_async(conn_t *c);

/* Main-thread reaper. Drains the close queue:
 *   - conns with in_worker==0  -> remove from timer_heap +
 *                                 epoll_ctl(DEL) + close(fd) + conn_destroy
 *   - conns with in_worker==1  -> re-enqueued for the next iteration
 * `th` may be NULL (used pre-init or in tests). */
void    conn_drain_closed(int epoll_fd, timer_heap_t *th);

size_t  conn_count(void);

#endif
