#ifndef FLUX_HTTP_PARSER_H
#define FLUX_HTTP_PARSER_H

#include <stddef.h>
#include <stdint.h>

/*
 * Incremental HTTP/1.x request parser — pure state machine, no I/O.
 * Designed so it can be unit-tested with raw byte arrays (no network needed).
 *
 * Usage on a connection:
 *   hp_reset(&r);
 *   for each batch of bytes from the socket:
 *       size_t consumed = 0;
 *       int rc = hp_parse(&r, data, len, &consumed);
 *       buf_consume(rx, consumed);
 *       if (rc == HP_DONE)      ... emit response, hp_reset for next request ...
 *       else if (rc == HP_NEED_MORE) ... keep buffering ...
 *       else                    ... rc is a negative HTTP status to return ...
 */

typedef enum {
    HP_METHOD_UNKNOWN = 0,
    HP_GET,
    HP_HEAD,
    HP_POST,
    HP_PUT,
    HP_DELETE,
    HP_OPTIONS,
} hp_method_t;

#define HP_NEED_MORE   0
#define HP_DONE        1
#define HP_ERR_400  (-400)   /* bad request           */
#define HP_ERR_414  (-414)   /* URI too long          */
#define HP_ERR_501  (-501)   /* not implemented       */

#define HP_URI_MAX      2048
#define HP_METHOD_MAX   16
#define HP_VERSION_MAX  16
#define HP_HNAME_MAX    64
#define HP_HVALUE_MAX   1024
#define HP_HOST_MAX     256

typedef struct http_request {
    /* parser internal state */
    int state;

    /* request line */
    hp_method_t method;
    char        method_str[HP_METHOD_MAX];
    int         method_len;
    char        uri[HP_URI_MAX];
    int         uri_len;
    char        version_buf[HP_VERSION_MAX];
    int         version_len;
    int         version_major;
    int         version_minor;

    /* selected headers (only ones we care about) */
    int         keep_alive;       /* defaults: 1 for HTTP/1.1, 0 for 1.0 */
    long        content_length;   /* -1 if header absent                */
    char        host[HP_HOST_MAX];
    int         host_len;

    /* scratch buffers for the currently-being-parsed header */
    char        hname[HP_HNAME_MAX];
    int         hname_len;
    char        hvalue[HP_HVALUE_MAX];
    int         hvalue_len;
} http_request_t;

void hp_reset(http_request_t *r);

/* Returns HP_DONE / HP_NEED_MORE / HP_ERR_*. `*consumed` is the prefix of
 * data that the parser absorbed (always set, even on errors / NEED_MORE). */
int  hp_parse(http_request_t *r,
              const uint8_t *data, size_t len,
              size_t *consumed);

#endif
