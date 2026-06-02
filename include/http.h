#ifndef FLUX_HTTP_H
#define FLUX_HTTP_H

#include "buf.h"
#include "http_parser.h"

/*
 * Response generation.  Given a parsed request and a target buffer, write the
 * full HTTP response (status line + headers + body) into `out`.
 *
 *   *keep_alive_out  is set per the request's Connection header / version
 *                    so the caller can decide whether to close after flush.
 *   returns 0 on success, -1 on internal error (caller should drop conn).
 *
 * Docroot is configurable via http_set_docroot(); defaults to "examples/www".
 */
int http_handle(const http_request_t *req, buf_t *out, int *keep_alive_out);

/* Emit a self-contained error response (status line + minimal body). */
void http_error(buf_t *out, int status, int keep_alive);

void http_set_docroot(const char *path);
const char *http_get_docroot(void);

/* When the server is draining, every response is forced to Connection: close
 * so clients know not to send another request on this connection. */
void http_set_draining(int yes);

#endif
