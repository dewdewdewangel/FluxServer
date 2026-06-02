#include "test.h"
#include "http_parser.h"

#include <string.h>

static int parse_full(http_request_t *r, const char *s, size_t *consumed) {
    hp_reset(r);
    return hp_parse(r, (const uint8_t *)s, strlen(s), consumed);
}

TEST(simple_get_done) {
    http_request_t r;
    size_t c = 0;
    int rc = parse_full(&r,
        "GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n", &c);
    ASSERT_EQ(rc, HP_DONE);
    ASSERT_EQ(r.method, HP_GET);
    ASSERT_STR_EQ(r.uri, "/index.html");
    ASSERT_EQ(r.version_major, 1);
    ASSERT_EQ(r.version_minor, 1);
    ASSERT_EQ(r.keep_alive, 1);
    ASSERT_STR_EQ(r.host, "x");
}

TEST(http_1_0_defaults_to_close) {
    http_request_t r;
    size_t c = 0;
    int rc = parse_full(&r, "GET / HTTP/1.0\r\n\r\n", &c);
    ASSERT_EQ(rc, HP_DONE);
    ASSERT_EQ(r.keep_alive, 0);
}

TEST(connection_close_header_overrides) {
    http_request_t r;
    size_t c = 0;
    int rc = parse_full(&r,
        "GET / HTTP/1.1\r\nConnection: close\r\n\r\n", &c);
    ASSERT_EQ(rc, HP_DONE);
    ASSERT_EQ(r.keep_alive, 0);
}

TEST(content_length_parsed) {
    http_request_t r;
    size_t c = 0;
    int rc = parse_full(&r,
        "POST /x HTTP/1.1\r\nContent-Length: 42\r\n\r\n", &c);
    ASSERT_EQ(rc, HP_DONE);
    ASSERT_EQ(r.method, HP_POST);
    ASSERT_EQ(r.content_length, 42);
}

TEST(case_insensitive_headers) {
    http_request_t r;
    size_t c = 0;
    int rc = parse_full(&r,
        "GET / HTTP/1.1\r\nHOST: a\r\nconnection: KEEP-ALIVE\r\n\r\n", &c);
    ASSERT_EQ(rc, HP_DONE);
    ASSERT_STR_EQ(r.host, "a");
    ASSERT_EQ(r.keep_alive, 1);
}

TEST(returns_need_more_for_partial_request) {
    http_request_t r;
    hp_reset(&r);
    size_t c = 0;
    const char *part = "GET /index.html HTTP/1.1\r\nHost: x\r\n";
    int rc = hp_parse(&r, (const uint8_t *)part, strlen(part), &c);
    ASSERT_EQ(rc, HP_NEED_MORE);
    ASSERT_EQ(c, strlen(part));

    /* Feed the rest — same parser instance, no reset. */
    const char *rest = "\r\n";
    rc = hp_parse(&r, (const uint8_t *)rest, strlen(rest), &c);
    ASSERT_EQ(rc, HP_DONE);
    ASSERT_STR_EQ(r.uri, "/index.html");
}

TEST(byte_by_byte_feed_works) {
    /* The state machine must give the same answer regardless of chunking. */
    http_request_t r;
    hp_reset(&r);
    const char *msg = "GET / HTTP/1.1\r\nHost: y\r\n\r\n";
    size_t c = 0;
    int rc = HP_NEED_MORE;
    for (size_t i = 0; msg[i]; i++) {
        rc = hp_parse(&r, (const uint8_t *)&msg[i], 1, &c);
        if (rc == HP_DONE) break;
        ASSERT_EQ(rc, HP_NEED_MORE);
    }
    ASSERT_EQ(rc, HP_DONE);
    ASSERT_EQ(r.method, HP_GET);
    ASSERT_STR_EQ(r.host, "y");
}

TEST(bad_method_returns_400) {
    http_request_t r;
    size_t c = 0;
    /* lowercase method = malformed */
    int rc = parse_full(&r, "get / HTTP/1.1\r\n\r\n", &c);
    ASSERT_EQ(rc, HP_ERR_400);
}

TEST(missing_version_returns_400) {
    http_request_t r;
    size_t c = 0;
    int rc = parse_full(&r, "GET /\r\n\r\n", &c);
    ASSERT_EQ(rc, HP_ERR_400);
}

TEST(http_2_returns_400) {
    http_request_t r;
    size_t c = 0;
    int rc = parse_full(&r, "GET / HTTP/2.0\r\n\r\n", &c);
    ASSERT_EQ(rc, HP_ERR_400);
}

TEST(consumed_bytes_equals_request_size_on_done) {
    http_request_t r;
    hp_reset(&r);
    size_t c = 0;
    const char *m = "GET /a HTTP/1.1\r\nHost: x\r\n\r\nextrabytes";
    int rc = hp_parse(&r, (const uint8_t *)m, strlen(m), &c);
    ASSERT_EQ(rc, HP_DONE);
    /* parser should stop at the blank line, leaving "extrabytes" un-consumed */
    ASSERT_EQ(c, strlen("GET /a HTTP/1.1\r\nHost: x\r\n\r\n"));
}

TEST(pipelined_two_requests_each_done) {
    http_request_t r;
    hp_reset(&r);
    const char *both =
        "GET /a HTTP/1.1\r\nHost: x\r\n\r\n"
        "GET /b HTTP/1.1\r\nHost: x\r\n\r\n";
    size_t c1 = 0;
    int rc1 = hp_parse(&r, (const uint8_t *)both, strlen(both), &c1);
    ASSERT_EQ(rc1, HP_DONE);
    ASSERT_STR_EQ(r.uri, "/a");

    /* Caller would now reset and feed the remainder. */
    hp_reset(&r);
    size_t c2 = 0;
    int rc2 = hp_parse(&r, (const uint8_t *)(both + c1),
                       strlen(both) - c1, &c2);
    ASSERT_EQ(rc2, HP_DONE);
    ASSERT_STR_EQ(r.uri, "/b");
}

TEST_MAIN("test_http_parser", {
    RUN_TEST(simple_get_done);
    RUN_TEST(http_1_0_defaults_to_close);
    RUN_TEST(connection_close_header_overrides);
    RUN_TEST(content_length_parsed);
    RUN_TEST(case_insensitive_headers);
    RUN_TEST(returns_need_more_for_partial_request);
    RUN_TEST(byte_by_byte_feed_works);
    RUN_TEST(bad_method_returns_400);
    RUN_TEST(missing_version_returns_400);
    RUN_TEST(http_2_returns_400);
    RUN_TEST(consumed_bytes_equals_request_size_on_done);
    RUN_TEST(pipelined_two_requests_each_done);
})
