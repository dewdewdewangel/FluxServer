#include "test.h"
#include "conn.h"
#include "metrics.h"
#include "timer_heap.h"

#include <netinet/in.h>
#include <string.h>

int g_test_failures = 0;
int g_test_run = 0;

static struct timespec ts(time_t s) { return (struct timespec){ s, 0 }; }

static conn_t *mk_conn(int fake_fd) {
    struct sockaddr_in peer = {0};
    return conn_create(fake_fd, &peer);
}

static void inc_counter(conn_t *c, void *ud) { (void)c; (*(int *)ud)++; }

TEST(push_peek_pop_in_order) {
    timer_heap_t *h = timer_heap_create(16);
    conn_t *a = mk_conn(10), *b = mk_conn(11), *c = mk_conn(12);

    timer_heap_push(h, b, ts(200));
    timer_heap_push(h, a, ts(100));
    timer_heap_push(h, c, ts(300));
    ASSERT_EQ(timer_heap_size(h), 3);

    struct timespec next;
    ASSERT_EQ(timer_heap_peek(h, &next), 0);
    ASSERT_EQ(next.tv_sec, 100);

    int popped = 0;
    timer_heap_pop_expired(h, ts(1000), inc_counter, &popped);
    ASSERT_EQ(popped, 3);
    ASSERT_EQ(timer_heap_size(h), 0);
    ASSERT_EQ(a->heap_idx, -1);
    ASSERT_EQ(b->heap_idx, -1);
    ASSERT_EQ(c->heap_idx, -1);

    conn_destroy(a); conn_destroy(b); conn_destroy(c);
    timer_heap_destroy(h);
}

TEST(update_can_reorder) {
    timer_heap_t *h = timer_heap_create(16);
    conn_t *a = mk_conn(20), *b = mk_conn(21), *c = mk_conn(22);
    timer_heap_push(h, a, ts(100));
    timer_heap_push(h, b, ts(200));
    timer_heap_push(h, c, ts(300));

    timer_heap_update(h, b, ts(50));
    struct timespec next;
    timer_heap_peek(h, &next);
    ASSERT_EQ(next.tv_sec, 50);

    timer_heap_update(h, b, ts(999));
    timer_heap_peek(h, &next);
    ASSERT_EQ(next.tv_sec, 100);

    conn_destroy(a); conn_destroy(b); conn_destroy(c);
    timer_heap_destroy(h);
}

TEST(remove_middle_keeps_invariant) {
    timer_heap_t *h = timer_heap_create(16);
    conn_t *cs[5];
    for (int i = 0; i < 5; i++) {
        cs[i] = mk_conn(30 + i);
        timer_heap_push(h, cs[i], ts(100 + i * 10));
    }
    timer_heap_remove(h, cs[2]);
    ASSERT_EQ(timer_heap_size(h), 4);
    ASSERT_EQ(cs[2]->heap_idx, -1);
    struct timespec next;
    timer_heap_peek(h, &next);
    ASSERT_EQ(next.tv_sec, 100);

    int n = 0;
    timer_heap_pop_expired(h, ts(1000), inc_counter, &n);
    ASSERT_EQ(n, 4);

    for (int i = 0; i < 5; i++) conn_destroy(cs[i]);
    timer_heap_destroy(h);
}

TEST(pop_only_expired) {
    timer_heap_t *h = timer_heap_create(16);
    conn_t *a = mk_conn(40), *b = mk_conn(41);
    timer_heap_push(h, a, ts(100));
    timer_heap_push(h, b, ts(500));
    int popped = 0;
    timer_heap_pop_expired(h, ts(200), inc_counter, &popped);
    ASSERT_EQ(popped, 1);
    ASSERT_EQ(timer_heap_size(h), 1);
    ASSERT_EQ(b->heap_idx, 0);

    conn_destroy(a); conn_destroy(b);
    timer_heap_destroy(h);
}

TEST(push_at_capacity_fails) {
    timer_heap_t *h = timer_heap_create(2);
    conn_t *a = mk_conn(50), *b = mk_conn(51), *c = mk_conn(52);
    ASSERT_EQ(timer_heap_push(h, a, ts(1)), 0);
    ASSERT_EQ(timer_heap_push(h, b, ts(2)), 0);
    ASSERT_EQ(timer_heap_push(h, c, ts(3)), -1);
    conn_destroy(a); conn_destroy(b); conn_destroy(c);
    timer_heap_destroy(h);
}

int main(void) {
    metrics_init();
    printf("test_timer_heap:\n");
    RUN_TEST(push_peek_pop_in_order);
    RUN_TEST(update_can_reorder);
    RUN_TEST(remove_middle_keeps_invariant);
    RUN_TEST(pop_only_expired);
    RUN_TEST(push_at_capacity_fails);
    int passed = g_test_run - g_test_failures;
    printf("  ----------------------------------------------\n");
    printf("  %d passed, %d failed (of %d)\n\n",
           passed, g_test_failures, g_test_run);
    return g_test_failures ? 1 : 0;
}
