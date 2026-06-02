#include "http.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "logger.h"
#include "metrics.h"

#define MAX_FILE_BYTES   (8 * 1024 * 1024)  /* refuse >8 MB inline reads */
#define DOCROOT_MAX      512

static char g_docroot[DOCROOT_MAX] = "examples/www";
static _Atomic int g_draining = 0;

void http_set_docroot(const char *path) {
    snprintf(g_docroot, sizeof(g_docroot), "%s", path ? path : "examples/www");
}

const char *http_get_docroot(void) { return g_docroot; }

void http_set_draining(int yes) {
    atomic_store(&g_draining, yes);
}

static const char *status_reason(int code) {
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 414: return "URI Too Long";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

static const char *mime_for(const char *uri) {
    const char *dot = strrchr(uri, '.');
    if (!dot) return "application/octet-stream";
    dot++;
    if (!strcasecmp(dot, "html") || !strcasecmp(dot, "htm")) return "text/html; charset=utf-8";
    if (!strcasecmp(dot, "css"))                              return "text/css; charset=utf-8";
    if (!strcasecmp(dot, "js"))                               return "application/javascript";
    if (!strcasecmp(dot, "json"))                             return "application/json";
    if (!strcasecmp(dot, "png"))                              return "image/png";
    if (!strcasecmp(dot, "jpg") || !strcasecmp(dot, "jpeg")) return "image/jpeg";
    if (!strcasecmp(dot, "gif"))                              return "image/gif";
    if (!strcasecmp(dot, "svg"))                              return "image/svg+xml";
    if (!strcasecmp(dot, "ico"))                              return "image/x-icon";
    if (!strcasecmp(dot, "txt"))                              return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

static void rfc1123_date(char *out, size_t n) {
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(out, n, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

/* Strip everything from '?' onwards and reject any '..' path component. */
static int sanitize_path(const char *uri, char *out, size_t out_cap) {
    /* must start with '/' */
    if (uri[0] != '/') return -1;

    /* find query string boundary */
    const char *q = strchr(uri, '?');
    size_t plen = q ? (size_t)(q - uri) : strlen(uri);

    if (plen >= out_cap) return -1;

    /* copy + reject ".." traversal segments */
    size_t out_i = 0;
    size_t i = 0;
    while (i < plen) {
        if (uri[i] == '/' && i + 2 < plen
            && uri[i+1] == '.' && uri[i+2] == '.'
            && (i + 3 == plen || uri[i+3] == '/')) {
            return -1;
        }
        out[out_i++] = uri[i++];
    }
    out[out_i] = 0;
    return 0;
}

static int append_status_line(buf_t *out, int version_minor, int code) {
    char line[128];
    int n = snprintf(line, sizeof(line),
                     "HTTP/1.%d %d %s\r\n",
                     version_minor, code, status_reason(code));
    return buf_append(out, line, (size_t)n);
}

static int append_common_headers(buf_t *out, const char *ctype,
                                 size_t content_len, int keep_alive) {
    char hdr[512];
    char date[64];
    rfc1123_date(date, sizeof(date));
    int n = snprintf(hdr, sizeof(hdr),
                     "Server: FluxServer/0.1\r\n"
                     "Date: %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: %s\r\n"
                     "\r\n",
                     date,
                     ctype,
                     content_len,
                     keep_alive ? "keep-alive" : "close");
    return buf_append(out, hdr, (size_t)n);
}

/* Inline error body. Always sends Connection: close on hard parse errors. */
void http_error(buf_t *out, int status, int keep_alive) {
    metrics_record_response(status);

    char body[256];
    int n = snprintf(body, sizeof(body),
                     "<!doctype html><html><body>"
                     "<h1>%d %s</h1>"
                     "<p>FluxServer</p>"
                     "</body></html>\n",
                     status, status_reason(status));

    if (append_status_line(out, 1, status) < 0) return;
    if (append_common_headers(out, "text/html; charset=utf-8",
                              (size_t)n, keep_alive) < 0) return;
    buf_append(out, body, (size_t)n);
}

/* Render /stats as JSON. Status 200. */
static int handle_stats(buf_t *out, int keep_alive, int head_only) {
    metrics_record_response(200);

    char body[1024];
    int n = snprintf(body, sizeof(body),
        "{\n"
        "  \"uptime_seconds\": %lu,\n"
        "  \"connections_accepted\": %lu,\n"
        "  \"connections_active\": %lu,\n"
        "  \"requests_total\": %lu,\n"
        "  \"responses_2xx\": %lu,\n"
        "  \"responses_4xx\": %lu,\n"
        "  \"responses_5xx\": %lu,\n"
        "  \"bytes_in\": %lu,\n"
        "  \"bytes_out\": %lu,\n"
        "  \"timeouts\": %lu\n"
        "}\n",
        (unsigned long)metrics_uptime_seconds(),
        (unsigned long)metrics_get(&g_metrics.connections_accepted),
        (unsigned long)metrics_get(&g_metrics.connections_active),
        (unsigned long)metrics_get(&g_metrics.requests_total),
        (unsigned long)metrics_get(&g_metrics.responses_2xx),
        (unsigned long)metrics_get(&g_metrics.responses_4xx),
        (unsigned long)metrics_get(&g_metrics.responses_5xx),
        (unsigned long)metrics_get(&g_metrics.bytes_in),
        (unsigned long)metrics_get(&g_metrics.bytes_out),
        (unsigned long)metrics_get(&g_metrics.timeouts));

    if (append_status_line(out, 1, 200) < 0) return -1;
    if (append_common_headers(out, "application/json", (size_t)n, keep_alive) < 0)
        return -1;
    if (!head_only)
        return buf_append(out, body, (size_t)n);
    return 0;
}

int http_handle(const http_request_t *req, buf_t *out, int *keep_alive_out) {
    int keep_alive = req->keep_alive;
    /* During shutdown, never advertise keep-alive — we want this connection
     * to terminate after this response so we can drain to zero. */
    if (atomic_load(&g_draining)) keep_alive = 0;
    *keep_alive_out = keep_alive;

    /* Method gate. */
    if (req->method != HP_GET && req->method != HP_HEAD) {
        http_error(out, 405, keep_alive);
        return 0;
    }

    /* Resolve & sanitize path. */
    char rel[HP_URI_MAX];
    if (sanitize_path(req->uri, rel, sizeof(rel)) < 0) {
        http_error(out, 400, keep_alive);
        return 0;
    }

    /* Built-in /stats endpoint (must come before docroot resolution). */
    if (strcmp(rel, "/stats") == 0) {
        return handle_stats(out, keep_alive, req->method == HP_HEAD);
    }
    /* Directory request -> serve index.html. */
    size_t rl = strlen(rel);
    if (rl == 0 || rel[rl - 1] == '/') {
        if (rl + sizeof("index.html") - 1 >= sizeof(rel)) {
            http_error(out, 414, keep_alive);
            return 0;
        }
        strcat(rel, "index.html");
    }

    char path[DOCROOT_MAX + HP_URI_MAX + 1];
    int pn = snprintf(path, sizeof(path), "%s%s", g_docroot, rel);
    if (pn < 0 || (size_t)pn >= sizeof(path)) {
        http_error(out, 414, keep_alive);
        return 0;
    }

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        http_error(out, errno == EACCES ? 403 : 404, keep_alive);
        return 0;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        http_error(out, 500, keep_alive);
        return 0;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        http_error(out, 403, keep_alive);
        return 0;
    }
    if (st.st_size > MAX_FILE_BYTES) {
        close(fd);
        http_error(out, 500, keep_alive);
        return 0;
    }

    /* Emit response. */
    metrics_record_response(200);
    if (append_status_line(out, 1, 200) < 0) { close(fd); return -1; }
    if (append_common_headers(out, mime_for(rel),
                              (size_t)st.st_size, keep_alive) < 0) {
        close(fd);
        return -1;
    }

    if (req->method == HP_HEAD) {
        close(fd);
        return 0;
    }

    /* Slurp file into out buffer. Bounded by MAX_FILE_BYTES. */
    if (buf_reserve(out, (size_t)st.st_size) < 0) {
        close(fd);
        return -1;
    }
    size_t want = (size_t)st.st_size;
    while (want > 0) {
        ssize_t n = read(fd, out->data + out->len, want);
        if (n > 0) {
            out->len += (size_t)n;
            want     -= (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            LOG_ERROR("read %s: %s", path, strerror(errno));
            close(fd);
            return -1;
        }
    }
    close(fd);
    return 0;
}
