#include "http_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum {
    S_M = 0,    /* collecting method     */
    S_U,        /* collecting URI        */
    S_V,        /* collecting version    */
    S_VCR,     /* CR seen, expect LF    */
    S_HNS,     /* header-line start: either CR (=> end of headers) or first char of header-name */
    S_HFCR,    /* terminator-CR seen, expect LF and DONE */
    S_HN,       /* in header name        */
    S_HOWS,    /* optional whitespace after ':' */
    S_HV,       /* in header value       */
    S_HVCR,    /* CR after value, expect LF */
    S_DONE,
    S_ERR,
};

void hp_reset(http_request_t *r) {
    memset(r, 0, sizeof(*r));
    r->content_length = -1;
    r->keep_alive     = 1; /* assume 1.1 until VERSION says otherwise */
}

static void classify_method(http_request_t *r) {
    if      (strcmp(r->method_str, "GET")     == 0) r->method = HP_GET;
    else if (strcmp(r->method_str, "HEAD")    == 0) r->method = HP_HEAD;
    else if (strcmp(r->method_str, "POST")    == 0) r->method = HP_POST;
    else if (strcmp(r->method_str, "PUT")     == 0) r->method = HP_PUT;
    else if (strcmp(r->method_str, "DELETE")  == 0) r->method = HP_DELETE;
    else if (strcmp(r->method_str, "OPTIONS") == 0) r->method = HP_OPTIONS;
    else r->method = HP_METHOD_UNKNOWN;
}

static int parse_version(http_request_t *r) {
    int maj = 0, min = 0;
    if (sscanf(r->version_buf, "HTTP/%d.%d", &maj, &min) != 2) return -1;
    if (maj != 1) return -1;
    r->version_major = maj;
    r->version_minor = min;
    /* RFC 7230: HTTP/1.1 defaults to keep-alive, HTTP/1.0 to close. */
    r->keep_alive = (min == 1) ? 1 : 0;
    return 0;
}

/* Commit the just-completed header into r->host / content_length / keep_alive. */
static void commit_header(http_request_t *r) {
    r->hname[r->hname_len] = 0;
    r->hvalue[r->hvalue_len] = 0;

    if (strcasecmp(r->hname, "Host") == 0) {
        int n = r->hvalue_len < HP_HOST_MAX - 1 ? r->hvalue_len : HP_HOST_MAX - 1;
        memcpy(r->host, r->hvalue, (size_t)n);
        r->host[n] = 0;
        r->host_len = n;
    } else if (strcasecmp(r->hname, "Content-Length") == 0) {
        r->content_length = strtol(r->hvalue, NULL, 10);
    } else if (strcasecmp(r->hname, "Connection") == 0) {
        if      (strcasecmp(r->hvalue, "close")      == 0) r->keep_alive = 0;
        else if (strcasecmp(r->hvalue, "keep-alive") == 0) r->keep_alive = 1;
    }
    r->hname_len  = 0;
    r->hvalue_len = 0;
}

int hp_parse(http_request_t *r, const uint8_t *data, size_t len, size_t *consumed) {
    size_t i;
    for (i = 0; i < len; i++) {
        uint8_t b = data[i];
        switch (r->state) {

        case S_M:
            if (b == ' ') {
                if (r->method_len == 0) goto err400;
                r->method_str[r->method_len] = 0;
                classify_method(r);
                r->state = S_U;
            } else if (b >= 'A' && b <= 'Z'
                       && r->method_len < HP_METHOD_MAX - 1) {
                r->method_str[r->method_len++] = (char)b;
            } else {
                goto err400;
            }
            break;

        case S_U:
            if (b == ' ') {
                if (r->uri_len == 0) goto err400;
                r->uri[r->uri_len] = 0;
                r->state = S_V;
            } else if (b == '\r' || b == '\n') {
                goto err400;
            } else if (r->uri_len < HP_URI_MAX - 1) {
                r->uri[r->uri_len++] = (char)b;
            } else {
                *consumed = i; return HP_ERR_414;
            }
            break;

        case S_V:
            if (b == '\r') {
                r->version_buf[r->version_len] = 0;
                if (parse_version(r) < 0) goto err400;
                r->state = S_VCR;
            } else if (r->version_len < HP_VERSION_MAX - 1) {
                r->version_buf[r->version_len++] = (char)b;
            } else {
                goto err400;
            }
            break;

        case S_VCR:
            if (b != '\n') goto err400;
            r->state = S_HNS;
            break;

        case S_HNS:
            if (b == '\r') {
                r->state = S_HFCR;
            } else if (b == '\n') {
                goto err400;
            } else {
                if (r->hname_len >= HP_HNAME_MAX - 1) goto err400;
                r->hname[r->hname_len++] = (char)b;
                r->state = S_HN;
            }
            break;

        case S_HFCR:
            if (b != '\n') goto err400;
            *consumed = i + 1;
            r->state = S_DONE;
            return HP_DONE;

        case S_HN:
            if (b == ':') {
                r->hname[r->hname_len] = 0;
                r->state = S_HOWS;
            } else if (b == '\r' || b == '\n') {
                goto err400;
            } else if (r->hname_len < HP_HNAME_MAX - 1) {
                r->hname[r->hname_len++] = (char)b;
            } else {
                goto err400;
            }
            break;

        case S_HOWS:
            if (b == ' ' || b == '\t') break;
            /* first non-OWS byte is part of the value */
            if (b == '\r') {
                /* empty header value */
                r->state = S_HVCR;
            } else {
                r->hvalue[r->hvalue_len++] = (char)b;
                r->state = S_HV;
            }
            break;

        case S_HV:
            if (b == '\r') {
                /* trim trailing OWS */
                while (r->hvalue_len > 0
                       && (r->hvalue[r->hvalue_len - 1] == ' '
                           || r->hvalue[r->hvalue_len - 1] == '\t'))
                    r->hvalue_len--;
                r->state = S_HVCR;
            } else if (b == '\n') {
                goto err400;
            } else if (r->hvalue_len < HP_HVALUE_MAX - 1) {
                r->hvalue[r->hvalue_len++] = (char)b;
            } else {
                goto err400;
            }
            break;

        case S_HVCR:
            if (b != '\n') goto err400;
            commit_header(r);
            r->state = S_HNS;
            break;

        default:
            goto err400;
        }
    }
    *consumed = i;
    return HP_NEED_MORE;

err400:
    *consumed = i;
    r->state = S_ERR;
    return HP_ERR_400;
}
