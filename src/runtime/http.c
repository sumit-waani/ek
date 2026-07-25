#include "runtime/http.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

int eka_http_parse(eka_http_request_t *req, char *data, size_t len) {
    memset(req, 0, sizeof(*req));
    req->raw_buffer = data;
    req->raw_len    = len;

    char *p   = data;
    char *end = data + len;

    /* --- Request line: METHOD SP PATH SP HTTP/1.x CRLF --- */
    req->method = p;
    while (p < end && *p != ' ' && *p != '\r' && *p != '\n') p++;
    if (p >= end || *p != ' ') return -1;
    *p++ = '\0';  /* null-terminate method */

    /* Skip spaces */
    while (p < end && *p == ' ') p++;

    req->path = p;
    /* Parse path until space or ? */
    while (p < end && *p != ' ' && *p != '?' && *p != '\r' && *p != '\n') p++;
    if (p >= end) return -1;

    if (*p == '?') {
        *p++ = '\0';  /* terminate path */
        req->query = p;
        while (p < end && *p != ' ' && *p != '\r' && *p != '\n') p++;
    }

    if (p < end) {
        if (*p == ' ') *p++ = '\0';  /* terminate path or query */
        /* Skip HTTP/1.x */
        while (p < end && *p != '\r' && *p != '\n') p++;
    }

    /* Skip CRLF */
    if (p < end && *p == '\r') p++;
    if (p < end && *p == '\n') p++;

    /* --- Headers --- */
    while (p < end && req->header_count < 32) {
        /* Empty line = end of headers */
        if (*p == '\r') {
            p++;
            if (p < end && *p == '\n') p++;
            break;
        }
        if (*p == '\n') {
            p++;
            break;
        }

        /* Header name */
        const char *name = p;
        while (p < end && *p != ':' && *p != '\r' && *p != '\n') p++;
        if (p >= end || *p != ':') return -1;
        size_t name_len = (size_t)(p - name);
        *p++ = '\0';

        /* Skip optional space after : */
        while (p < end && *p == ' ') p++;

        /* Header value */
        const char *value = p;
        while (p < end && *p != '\r' && *p != '\n') p++;
        size_t value_len = (size_t)(p - value);

        /* Skip CRLF */
        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n') p++;

        /* Store header */
        int h = req->header_count++;
        req->headers[h].name      = name;
        req->headers[h].name_len  = name_len;
        req->headers[h].value     = value;
        req->headers[h].value_len = value_len;
    }

    /* --- Body --- */
    if (p < end) {
        req->body     = p;
        req->body_len = (size_t)(end - p);
    }

    return 0;
}

void eka_http_request_free(eka_http_request_t *req) {
    free(req->raw_buffer);
    memset(req, 0, sizeof(*req));
}

const char *eka_http_get_header(const eka_http_request_t *req,
                                 const char *name) {
    for (int i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0) {
            return req->headers[i].value;
        }
    }
    return NULL;
}
