#include "timer_heap.h"

#include <stdlib.h>
#include <string.h>

#include "conn.h"

struct node {
    struct timespec deadline;
    conn_t         *conn;
};

struct timer_heap {
    struct node *nodes;
    int          cap;
    int          count;
};

static int cmp_ts(struct timespec a, struct timespec b) {
    if (a.tv_sec  != b.tv_sec)  return a.tv_sec  < b.tv_sec  ? -1 : 1;
    if (a.tv_nsec != b.tv_nsec) return a.tv_nsec < b.tv_nsec ? -1 : 1;
    return 0;
}

static void swap_nodes(timer_heap_t *h, int i, int j) {
    struct node tmp = h->nodes[i];
    h->nodes[i] = h->nodes[j];
    h->nodes[j] = tmp;
    h->nodes[i].conn->heap_idx = i;
    h->nodes[j].conn->heap_idx = j;
}

static void sift_up(timer_heap_t *h, int i) {
    while (i > 0) {
        int p = (i - 1) / 2;
        if (cmp_ts(h->nodes[i].deadline, h->nodes[p].deadline) >= 0) break;
        swap_nodes(h, i, p);
        i = p;
    }
}

static void sift_down(timer_heap_t *h, int i) {
    for (;;) {
        int l = 2 * i + 1, r = 2 * i + 2, best = i;
        if (l < h->count && cmp_ts(h->nodes[l].deadline, h->nodes[best].deadline) < 0) best = l;
        if (r < h->count && cmp_ts(h->nodes[r].deadline, h->nodes[best].deadline) < 0) best = r;
        if (best == i) break;
        swap_nodes(h, i, best);
        i = best;
    }
}

timer_heap_t *timer_heap_create(int cap) {
    timer_heap_t *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->nodes = calloc((size_t)cap, sizeof(*h->nodes));
    if (!h->nodes) { free(h); return NULL; }
    h->cap = cap;
    return h;
}

void timer_heap_destroy(timer_heap_t *h) {
    if (!h) return;
    free(h->nodes);
    free(h);
}

int timer_heap_push(timer_heap_t *h, conn_t *c, struct timespec deadline) {
    if (h->count >= h->cap) return -1;
    int i = h->count++;
    h->nodes[i].deadline = deadline;
    h->nodes[i].conn     = c;
    c->heap_idx = i;
    sift_up(h, i);
    return 0;
}

int timer_heap_update(timer_heap_t *h, conn_t *c, struct timespec deadline) {
    int i = c->heap_idx;
    if (i < 0 || i >= h->count) return -1;
    int order = cmp_ts(deadline, h->nodes[i].deadline);
    h->nodes[i].deadline = deadline;
    if (order < 0)      sift_up(h, i);
    else if (order > 0) sift_down(h, i);
    return 0;
}

int timer_heap_remove(timer_heap_t *h, conn_t *c) {
    int i = c->heap_idx;
    if (i < 0 || i >= h->count || h->nodes[i].conn != c) return 0;

    int last = --h->count;
    c->heap_idx = -1;

    if (i == last) return 0;

    h->nodes[i] = h->nodes[last];
    h->nodes[i].conn->heap_idx = i;
    /* The moved node could need to bubble in either direction. */
    if (i > 0 && cmp_ts(h->nodes[i].deadline, h->nodes[(i - 1) / 2].deadline) < 0)
        sift_up(h, i);
    else
        sift_down(h, i);
    return 0;
}

int timer_heap_size(const timer_heap_t *h) { return h->count; }

int timer_heap_peek(const timer_heap_t *h, struct timespec *out) {
    if (h->count == 0) return -1;
    *out = h->nodes[0].deadline;
    return 0;
}

void timer_heap_pop_expired(timer_heap_t *h, struct timespec now,
                            void (*cb)(conn_t *, void *), void *ud) {
    while (h->count > 0 && cmp_ts(h->nodes[0].deadline, now) <= 0) {
        conn_t *c = h->nodes[0].conn;

        int last = --h->count;
        c->heap_idx = -1;
        if (last > 0) {
            h->nodes[0] = h->nodes[last];
            h->nodes[0].conn->heap_idx = 0;
            sift_down(h, 0);
        }
        cb(c, ud);
    }
}
