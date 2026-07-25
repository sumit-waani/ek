#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include <stdint.h>

/*
 * Minimal HTTP/1.1 request parser.
 */

typedef struct eka_http_request_t {
    const char *method;
    const char *path;
    const char *query;       /* everything after ? in path, or NULL */

    /* Headers stored as simple linked list for now */
    struct {
        const char *name;
        size_t      name_len;
        const char *value;
        size_t      value_len;
    } headers[32];
    int header_count;

    /* Body (for POST/PUT) */
    const char *body;
    size_t      body_len;

    /* Raw buffer that owns the data */
    char  *raw_buffer;
    size_t raw_len;
} eka_http_request_t;

/* Parse an HTTP request from raw bytes. Returns 0 on success, -1 on error. */
int eka_http_parse(eka_http_request_t *req, char *data, size_t len);

/* Free request resources. */
void eka_http_request_free(eka_http_request_t *req);

/* Get a header value by name (case-insensitive). Returns NULL if not found. */
const char *eka_http_get_header(const eka_http_request_t *req,
                                const char *name);

#endif /* HTTP_H */
