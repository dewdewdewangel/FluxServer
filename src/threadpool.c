#include "threadpool.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "conn.h"
#include "http.h"
#include "http_parser.h"
#include "logger.h"
#include "metrics.h"

#define READ_CHUNK 4096

/*
 * Workers NEVER close(fd) or conn_destroy() directly. Two reasons:
 *   1. A close + fd reuse by accept() racing with another worker still holding
 *      a stale pointer is the classic fd-UAF in this architecture.
 *   2. Centralising fd lifecycle on the reactor thread keeps epoll_ctl
 *      ordering correct (ADD/MOD/DEL never interleave from two threads).
 *
 * On error: conn_close_async() (CAS + enqueue), then drop in_worker last
 * so the reactor's reaper can safely take ownership.
 */
static void handle_client(conn_t *c, int epoll_fd) {
    int should_close = 0;

    while (!c->read_closed) {
        if (buf_reserve(&c->rx, READ_CHUNK) < 0) {
            LOG_ERROR("rx buf_reserve OOM fd=%d", c->fd);
            should_close = 1;
            break;
        }
        ssize_t n = read(c->fd, buf_write_ptr(&c->rx), buf_writable(&c->rx));
        if (n > 0) {
            c->rx.len += (size_t)n;
            metrics_add(&g_metrics.bytes_in, (uint64_t)n);
        } else if (n == 0) {
            c->read_closed = 1;
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            LOG_ERROR("read fd=%d: errno=%d", c->fd, errno);
            should_close = 1;
            break;
        }
    }

    while (!should_close && buf_readable(&c->rx) > 0) {
        size_t consumed = 0;
        int rc = hp_parse(&c->req, buf_peek(&c->rx),
                          buf_readable(&c->rx), &consumed);
        buf_consume(&c->rx, consumed);

        if (rc == HP_NEED_MORE) break;

        if (rc == HP_DONE) {
            metrics_inc(&g_metrics.requests_total);
            int keep_alive = 0;
            if (http_handle(&c->req, &c->tx, &keep_alive) < 0) {
                should_close = 1;
                break;
            }
            if (!keep_alive) {
                c->read_closed = 1;
                hp_reset(&c->req);
                break;
            }
            hp_reset(&c->req);
            continue;
        }

        http_error(&c->tx, -rc, 0);
        c->read_closed = 1;
        break;
    }

    while (!should_close && buf_readable(&c->tx) > 0) {
        ssize_t n = write(c->fd, buf_peek(&c->tx), buf_readable(&c->tx));
        if (n > 0) {
            buf_consume(&c->tx, (size_t)n);
            metrics_add(&g_metrics.bytes_out, (uint64_t)n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            LOG_ERROR("write fd=%d: errno=%d", c->fd, errno);
            should_close = 1;
            break;
        }
    }

    if (should_close || atomic_load(&c->state) == CONN_CLOSING) {
        conn_close_async(c);
        atomic_store(&c->in_worker, 0);
        return;
    }

    if (c->read_closed && buf_readable(&c->tx) == 0) {
        conn_close_async(c);
        atomic_store(&c->in_worker, 0);
        return;
    }

    uint32_t evflags = EPOLLET | EPOLLONESHOT;
    if (!c->read_closed)         evflags |= EPOLLIN;
    if (buf_readable(&c->tx) > 0) {
        evflags |= EPOLLOUT;
        c->want_write = 1;
    } else {
        c->want_write = 0;
    }
    struct epoll_event ev = { .events = evflags, .data.fd = c->fd };
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, c->fd, &ev) < 0) {
        LOG_ERROR("epoll_ctl MOD fd=%d: errno=%d", c->fd, errno);
        conn_close_async(c);
    }
    atomic_store(&c->in_worker, 0);
}

/*
 * Per-worker queue + least-loaded dispatch.
 *
 * Each worker pops only from its own slot's FIFO, so the per-slot mutex
 * is only ever contended by the dispatcher + that one worker. submit()
 * picks the slot whose queue is currently shallowest (atomic count read,
 * no lock), which adaptively steers tasks away from a slot whose worker
 * has been preempted by the kernel under oversubscription. That recovers
 * the shared-queue's self-balancing property without the global mutex.
 *
 * Tried first with plain round-robin: it regressed throughput at
 * threads >= 8 because a preempted worker's slot would grow while peers
 * went idle. The least-loaded scan is O(N) per submit, which is cheap
 * for N <= 16 and earns ~2x at threads=12 vs. RR.
 */
typedef struct task {
    conn_t      *conn;
    int          epoll_fd;
    struct task *next;
} task_t;

typedef struct worker_slot {
    pthread_mutex_t       mutex;
    pthread_cond_t        cond;
    task_t               *head;
    task_t               *tail;
    _Atomic int           count;     /* read lock-free by submit() to pick least-loaded */
    /*
     * Lock-free freelist. SPSC: the reactor pops via task_alloc() inside
     * submit(); this slot's worker pushes via task_free(). Single-popper
     * guarantees no ABA. Misses fall back to malloc(); drained in
     * threadpool_destroy() after workers join.
     */
    _Atomic(task_t *)     free_head;
    pthread_t             thread;
    struct threadpool    *pool;      /* back-pointer for shutdown flag */
} worker_slot_t;

struct threadpool {
    worker_slot_t *slots;
    int            thread_count;
    _Atomic int    shutdown;
};

/* Reactor side: try freelist, fall back to malloc. */
static task_t *task_alloc(worker_slot_t *slot) {
    task_t *t = atomic_load_explicit(&slot->free_head, memory_order_acquire);
    while (t != NULL) {
        if (atomic_compare_exchange_weak_explicit(
                &slot->free_head, &t, t->next,
                memory_order_acquire, memory_order_acquire))
            return t;
        /* CAS failed: `t` was rewritten to the new head, loop with that. */
    }
    return malloc(sizeof(task_t));
}

/* Worker side: push onto its own slot's freelist. */
static void task_free(worker_slot_t *slot, task_t *t) {
    task_t *old_head = atomic_load_explicit(&slot->free_head, memory_order_relaxed);
    do {
        t->next = old_head;
    } while (!atomic_compare_exchange_weak_explicit(
                 &slot->free_head, &old_head, t,
                 memory_order_release, memory_order_relaxed));
}

static void *worker(void *arg) {
    worker_slot_t *slot = (worker_slot_t *)arg;
    struct threadpool *pool = slot->pool;

    for (;;) {
        pthread_mutex_lock(&slot->mutex);

        while (atomic_load_explicit(&slot->count, memory_order_relaxed) == 0
               && !atomic_load(&pool->shutdown))
            pthread_cond_wait(&slot->cond, &slot->mutex);

        if (atomic_load(&pool->shutdown)
            && atomic_load_explicit(&slot->count, memory_order_relaxed) == 0) {
            pthread_mutex_unlock(&slot->mutex);
            break;
        }

        task_t *task = slot->head;
        slot->head = task->next;
        if (slot->head == NULL) slot->tail = NULL;
        atomic_fetch_sub_explicit(&slot->count, 1, memory_order_relaxed);

        pthread_mutex_unlock(&slot->mutex);

        handle_client(task->conn, task->epoll_fd);
        task_free(slot, task);
    }

    return NULL;
}

threadpool_t *threadpool_create(int thread_count) {
    if (thread_count <= 0) return NULL;

    threadpool_t *pool = calloc(1, sizeof(*pool));
    if (!pool) return NULL;

    pool->thread_count = thread_count;
    atomic_store(&pool->shutdown, 0);

    pool->slots = calloc((size_t)thread_count, sizeof(worker_slot_t));
    if (!pool->slots) { free(pool); return NULL; }

    for (int i = 0; i < thread_count; i++) {
        worker_slot_t *s = &pool->slots[i];
        pthread_mutex_init(&s->mutex, NULL);
        pthread_cond_init(&s->cond, NULL);
        s->pool = pool;
    }
    for (int i = 0; i < thread_count; i++)
        pthread_create(&pool->slots[i].thread, NULL, worker, &pool->slots[i]);

    return pool;
}

void threadpool_submit(threadpool_t *pool, conn_t *conn, int epoll_fd) {
    /* Least-loaded dispatch: O(N) scan picks the slot with the shallowest
     * queue. Under oversubscription a preempted worker's slot grows and
     * we steer new tasks away — recovers the shared-queue's adaptive load
     * balancing without the single-mutex contention. */
    int best = 0;
    int best_count = atomic_load_explicit(&pool->slots[0].count,
                                          memory_order_relaxed);
    for (int i = 1; i < pool->thread_count; i++) {
        int c = atomic_load_explicit(&pool->slots[i].count,
                                     memory_order_relaxed);
        if (c < best_count) { best_count = c; best = i; }
    }
    worker_slot_t *slot = &pool->slots[best];

    /* Alloc *after* slot is chosen so we always free back to the same
     * slot's freelist (worker that pops will be slot's worker). */
    task_t *task = task_alloc(slot);
    if (!task) {
        LOG_ERROR("threadpool_submit OOM, dropping fd=%d", conn->fd);
        conn_close_async(conn);
        atomic_store(&conn->in_worker, 0);
        return;
    }

    task->conn = conn;
    task->epoll_fd = epoll_fd;
    task->next = NULL;

    pthread_mutex_lock(&slot->mutex);
    if (slot->tail)
        slot->tail->next = task;
    else
        slot->head = task;
    slot->tail = task;
    atomic_fetch_add_explicit(&slot->count, 1, memory_order_relaxed);
    pthread_cond_signal(&slot->cond);
    pthread_mutex_unlock(&slot->mutex);
}

void threadpool_destroy(threadpool_t *pool) {
    if (!pool) return;

    atomic_store(&pool->shutdown, 1);

    /* Lock+broadcast per slot. Holding the slot mutex during broadcast
     * prevents a worker from racing in between its `count==0 && !shutdown`
     * check and pthread_cond_wait. */
    for (int i = 0; i < pool->thread_count; i++) {
        worker_slot_t *s = &pool->slots[i];
        pthread_mutex_lock(&s->mutex);
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->mutex);
    }

    for (int i = 0; i < pool->thread_count; i++)
        pthread_join(pool->slots[i].thread, NULL);

    /* Drain leftover queued tasks. Shutdown path — reactor isn't running,
     * so we must close + destroy conns directly here. */
    for (int i = 0; i < pool->thread_count; i++) {
        worker_slot_t *s = &pool->slots[i];
        task_t *t = s->head;
        while (t) {
            task_t *next = t->next;
            if (t->conn) {
                close(t->conn->fd);
                conn_destroy(t->conn);
            }
            free(t);
            t = next;
        }
        /* Drain the per-slot freelist. Workers have already joined, so the
         * SPSC invariant is no longer needed — a plain load is fine. */
        task_t *f = atomic_load_explicit(&s->free_head, memory_order_relaxed);
        while (f) {
            task_t *next = f->next;
            free(f);
            f = next;
        }
        pthread_mutex_destroy(&s->mutex);
        pthread_cond_destroy(&s->cond);
    }
    free(pool->slots);
    free(pool);
}
