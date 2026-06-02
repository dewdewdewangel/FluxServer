#ifndef FLUX_BUF_H
#define FLUX_BUF_H

#include <stddef.h>
#include <stdint.h>

/*
 * Growable byte buffer with a read cursor (off) and write cursor (len).
 * Layout:  [ consumed | unread data | free space ]
 *                     ^             ^             ^
 *                     off           len           cap
 *
 * - Append writes at `data + len`, advances `len`.
 * - Consume advances `off`; when off == len the buffer auto-resets.
 * - Reserve auto-compacts and/or grows to fit `want` more bytes.
 */
typedef struct {
    uint8_t *data;
    size_t   cap;
    size_t   len;
    size_t   off;
} buf_t;

void   buf_init(buf_t *b);
void   buf_free(buf_t *b);
int    buf_reserve(buf_t *b, size_t want);
int    buf_append(buf_t *b, const void *src, size_t n);
void   buf_consume(buf_t *b, size_t n);
void   buf_compact(buf_t *b);
size_t buf_readable(const buf_t *b);
size_t buf_writable(const buf_t *b);
uint8_t *buf_write_ptr(buf_t *b);
const uint8_t *buf_peek(const buf_t *b);

#endif
