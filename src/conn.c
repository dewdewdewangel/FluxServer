#include "conn.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "logger.h"
#include "metrics.h"
#include "timer_heap.h"

static conn_t *g_table[FLUX_MAX_CONN];
static size_t  g_count;

/* Close queue: protected by g_close_mu. Single-producer-multi-consumer in
 * practice (any thread may push via conn_close_async; only the reactor pops). */
static pthread_mutex_t g_close_mu = PTHREAD_MUTEX_INITIALIZER;
static conn_t         *g_close_head;

conn_t *conn_create(int fd, const struct sockaddr_in *peer) {
    if (fd < 0 || fd >= FLUX_MAX_CONN) return NULL;
    if (g_table[fd] != NULL) return NULL;

    conn_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;

    c->fd = fd;
    atomic_store(&c->state, CONN_ACTIVE);
    atomic_store(&c->in_worker, 0);
    c->peer = *peer;
    clock_gettime(CLOCK_MONOTONIC, &c->last_active);
    c->heap_idx = -1;
    buf_init(&c->rx);
    buf_init(&c->tx);
    hp_reset(&c->req);

    g_table[fd] = c;
    g_count++;
    metrics_inc(&g_metrics.connections_accepted);
    metrics_inc(&g_metrics.connections_active);
    return c;
}

conn_t *conn_get(int fd) {
    if (fd < 0 || fd >= FLUX_MAX_CONN) return NULL;
    return g_table[fd];
}

void conn_destroy(conn_t *c) {
    if (!c) return;
    if (c->fd >= 0 && c->fd < FLUX_MAX_CONN && g_table[c->fd] == c) {
        g_table[c->fd] = NULL;
        g_count--;
        metrics_dec(&g_metrics.connections_active);
    }
    buf_free(&c->rx);
    buf_free(&c->tx);
    free(c);
}

void conn_foreach(int (*cb)(conn_t *c, void *ud), void *ud) {
    for (int fd = 0; fd < FLUX_MAX_CONN; fd++) {
        conn_t *c = g_table[fd];
        if (c && cb(c, ud)) return;
    }
}

int conn_close_async(conn_t *c) {
    int expected = CONN_ACTIVE;
    if (!atomic_compare_exchange_strong(&c->state, &expected, CONN_CLOSING))
        return 0; /* lost the race or already closing — fine */

    pthread_mutex_lock(&g_close_mu);
    c->next_close = g_close_head;
    g_close_head = c;
    pthread_mutex_unlock(&g_close_mu);
    return 1;
}

void conn_drain_closed(int epoll_fd, timer_heap_t *th) {
    /* Snapshot the queue so we don't hold the mutex during close()/destroy. */
    pthread_mutex_lock(&g_close_mu);
    conn_t *head = g_close_head;
    g_close_head = NULL;
    pthread_mutex_unlock(&g_close_mu);

    conn_t *deferred = NULL;

    while (head) {
        conn_t *c = head;
        head = c->next_close;
        c->next_close = NULL;

        if (atomic_load(&c->in_worker)) {
            /* Worker hasn't finished — defer to the next drain. */
            c->next_close = deferred;
            deferred = c;
            continue;
        }

        int fd = c->fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0 && errno != ENOENT) {
            LOG_WARN("epoll_ctl DEL fd=%d: %s", fd, strerror(errno));
        }
        if (th) timer_heap_remove(th, c);
        close(fd);
        conn_destroy(c);
    }

    if (!deferred) return;

    /* Splice deferred back to the front of the queue. */
    pthread_mutex_lock(&g_close_mu);
    conn_t *tail = deferred;
    while (tail->next_close) tail = tail->next_close;
    tail->next_close = g_close_head;
    g_close_head = deferred;
    pthread_mutex_unlock(&g_close_mu);
}

size_t conn_count(void) { return g_count; }
