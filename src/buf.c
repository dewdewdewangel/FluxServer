#include "buf.h"

#include <stdlib.h>
#include <string.h>

#define BUF_INIT_CAP 4096

void buf_init(buf_t *b) {
    b->data = NULL;
    b->cap = 0;
    b->len = 0;
    b->off = 0;
}

void buf_free(buf_t *b) {
    free(b->data);
    b->data = NULL;
    b->cap = 0;
    b->len = 0;
    b->off = 0;
}

void buf_compact(buf_t *b) {
    if (b->off == 0) return;
    size_t unread = b->len - b->off;
    if (unread > 0) memmove(b->data, b->data + b->off, unread);
    b->len = unread;
    b->off = 0;
}

int buf_reserve(buf_t *b, size_t want) {
    if (b->cap - b->len >= want) return 0;

    /* Try compaction before growing. */
    if (b->off > 0) {
        buf_compact(b);
        if (b->cap - b->len >= want) return 0;
    }

    size_t new_cap = b->cap ? b->cap : BUF_INIT_CAP;
    while (new_cap - b->len < want) {
        if (new_cap > (SIZE_MAX / 2)) return -1;
        new_cap *= 2;
    }
    uint8_t *p = realloc(b->data, new_cap);
    if (!p) return -1;
    b->data = p;
    b->cap = new_cap;
    return 0;
}

int buf_append(buf_t *b, const void *src, size_t n) {
    if (n == 0) return 0;
    if (buf_reserve(b, n) < 0) return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

void buf_consume(buf_t *b, size_t n) {
    if (n > b->len - b->off) n = b->len - b->off;
    b->off += n;
    if (b->off == b->len) {
        b->off = 0;
        b->len = 0;
    }
}

size_t buf_readable(const buf_t *b) { return b->len - b->off; }
size_t buf_writable(const buf_t *b) { return b->cap - b->len; }
uint8_t *buf_write_ptr(buf_t *b)    { return b->data + b->len; }
const uint8_t *buf_peek(const buf_t *b) { return b->data + b->off; }
