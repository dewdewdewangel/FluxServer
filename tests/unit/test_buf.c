#include "test.h"
#include "buf.h"

TEST(init_empty) {
    buf_t b; buf_init(&b);
    ASSERT_EQ(buf_readable(&b), 0);
    ASSERT_EQ(buf_writable(&b), 0);
    buf_free(&b);
}

TEST(append_grows_and_reads_back) {
    buf_t b; buf_init(&b);
    ASSERT_EQ(buf_append(&b, "hello", 5), 0);
    ASSERT_EQ(buf_append(&b, " world", 6), 0);
    ASSERT_EQ(buf_readable(&b), 11);
    ASSERT_MEM_EQ(buf_peek(&b), "hello world", 11);
    buf_free(&b);
}

TEST(consume_advances_and_resets_when_drained) {
    buf_t b; buf_init(&b);
    buf_append(&b, "abcdef", 6);
    buf_consume(&b, 3);
    ASSERT_EQ(buf_readable(&b), 3);
    ASSERT_MEM_EQ(buf_peek(&b), "def", 3);
    /* full drain resets off back to 0 */
    buf_consume(&b, 3);
    ASSERT_EQ(buf_readable(&b), 0);
    ASSERT_EQ(b.off, 0);
    ASSERT_EQ(b.len, 0);
    buf_free(&b);
}

TEST(reserve_compacts_before_growing) {
    buf_t b; buf_init(&b);
    /* Force a real shortage of writable space at the tail: fill near cap,
     * then consume most of it. Compaction alone must satisfy the reserve. */
    buf_reserve(&b, 100);
    size_t cap1 = b.cap;
    char *blob = malloc(cap1);
    memset(blob, 'X', cap1);
    buf_append(&b, blob, cap1);             /* off=0, len=cap, tail=0 */
    /* Leave 4 unread bytes "AB89" by overwriting the consumed prefix marker. */
    memcpy(b.data + cap1 - 4, "AB89", 4);
    buf_consume(&b, cap1 - 4);              /* off=cap-4, len=cap, unread=4 */
    ASSERT_EQ(b.off, cap1 - 4);
    /* cap-len = 0, so reserve(8) must compact (after which cap-len = cap-4). */
    ASSERT_EQ(buf_reserve(&b, 8), 0);
    ASSERT_EQ(b.off, 0);
    ASSERT_EQ(b.len, 4);
    ASSERT_EQ(b.cap, cap1);                 /* no realloc */
    ASSERT_MEM_EQ(buf_peek(&b), "AB89", 4);
    free(blob);
    buf_free(&b);
}

TEST(reserve_doubles_capacity_when_needed) {
    buf_t b; buf_init(&b);
    buf_reserve(&b, 4096);
    size_t cap1 = b.cap;
    /* fill so compaction can't help */
    char *blob = malloc(cap1);
    memset(blob, 'A', cap1);
    buf_append(&b, blob, cap1);
    /* reserve more — must realloc */
    buf_reserve(&b, 4096);
    ASSERT_TRUE(b.cap >= cap1 + 4096);
    free(blob);
    buf_free(&b);
}

TEST(writable_tracks_free_space) {
    buf_t b; buf_init(&b);
    buf_reserve(&b, 100);
    size_t cap = b.cap;
    ASSERT_EQ(buf_writable(&b), cap);
    buf_append(&b, "x", 1);
    ASSERT_EQ(buf_writable(&b), cap - 1);
    buf_free(&b);
}

TEST_MAIN("test_buf", {
    RUN_TEST(init_empty);
    RUN_TEST(append_grows_and_reads_back);
    RUN_TEST(consume_advances_and_resets_when_drained);
    RUN_TEST(reserve_compacts_before_growing);
    RUN_TEST(reserve_doubles_capacity_when_needed);
    RUN_TEST(writable_tracks_free_space);
})
