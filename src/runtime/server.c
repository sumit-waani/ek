#define _GNU_SOURCE
#include "runtime/server.h"
#include "runtime/http.h"
#include "core/vm.h"
#include "core/obj.h"
#include "builtins/builtins.h"
#include "client/client_runtime.h"
#include "eka.h"

#include <pthread.h>
#include <uv.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================================================================
 * Server structure
 * ================================================================ */

struct eka_server_t {
    uv_loop_t               *loop;
    eka_compiled_program_t  *prog;
    eka_vm_t                *vm;         /* init VM — globals live here */
    const char              *static_dir;
    int                      port;
    uv_tcp_t                 listener;
    bool                     running;
};

/* ================================================================
 * Connection state
 * ================================================================ */

typedef struct {
    eka_server_t       *server;
    uv_tcp_t            client;
    uv_write_t          write_req;
    char               *read_buf;
    size_t              read_buf_size;
    char               *response_data;
    size_t              response_len;
} eka_conn_t;

/* ================================================================
 * Route matching
 * ================================================================ */

static int find_route(eka_compiled_program_t *prog, const char *method,
                       const char *path) {
    eka_token_type_t expected;
    if (strcmp(method, "GET") == 0)      expected = TOKEN_AT_GET;
    else if (strcmp(method, "POST") == 0) expected = TOKEN_AT_POST;
    else if (strcmp(method, "PUT") == 0)  expected = TOKEN_AT_PUT;
    else if (strcmp(method, "DELETE") == 0) expected = TOKEN_AT_DELETE;
    else if (strcmp(method, "PATCH") == 0) expected = TOKEN_AT_PATCH;
    else return -1;

    /* Simple path match for V1: match the path string */
    for (int i = 0; i < prog->method_count; i++) {
        if (prog->methods[i].method == expected) {
            const char *route_path = prog->methods[i].path;
            /* Simple exact match for now */
            if (strcmp(route_path, path) == 0) {
                return i;
            }
        }
    }
    return -1;
}

/* ================================================================
 * Response building
 * ================================================================ */

static char *build_response(const char *body, size_t body_len,
                            int status, const char *content_type,
                            size_t *out_len) {
    const char *status_text;
    switch (status) {
    case 200: status_text = "200 OK"; break;
    case 302: status_text = "302 Found"; break;
    case 404: status_text = "404 Not Found"; break;
    case 405: status_text = "405 Method Not Allowed"; break;
    case 500: status_text = "500 Internal Server Error"; break;
    default:  status_text = "500 Internal Server Error"; break;
    }

    /* Build HTTP response */
    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_text, content_type, body_len);

    size_t total = (size_t)header_len + body_len;
    char *resp = malloc(total + 1);
    if (!resp) return NULL;
    memcpy(resp, header, (size_t)header_len);
    if (body && body_len > 0) {
        memcpy(resp + header_len, body, body_len);
    }
    resp[total] = '\0';
    *out_len = total;
    return resp;
}

/* ================================================================
 * Client runtime: virtual route /_eka.js
 *
 * Serves the embedded JS runtime with immutable caching.
 * No worker VM needed — direct response from connection handler.
 * ================================================================ */

static bool is_eka_js_request(const char *path) {
    /* Match exactly "/_eka.js" or "/_eka.js?v=X.Y.Z" */
    if (strncmp(path, "/_eka.js", 8) != 0) return false;
    return (path[8] == '\0' || path[8] == '?');
}

static void serve_eka_js(eka_conn_t *conn) {
    char header[256];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/javascript; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: public, max-age=31536000, immutable\r\n"
        "Connection: close\r\n"
        "\r\n",
        EKA_CLIENT_RUNTIME_LEN);

    size_t total = (size_t)header_len + EKA_CLIENT_RUNTIME_LEN;
    char *resp = malloc(total + 1);
    if (!resp) {
        const char *err = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
        size_t err_len = strlen(err);
        conn->response_data = malloc(err_len + 1);
        memcpy(conn->response_data, err, err_len + 1);
        conn->response_len = err_len;
        return;
    }

    memcpy(resp, header, (size_t)header_len);
    memcpy(resp + header_len, EKA_CLIENT_RUNTIME, EKA_CLIENT_RUNTIME_LEN);
    resp[total] = '\0';
    conn->response_data = resp;
    conn->response_len = total;
}

/* ================================================================
 * Auto-injection: scan HTML for e-* attributes, inject <script>
 *
 * Called after building the response body for text/html responses.
 * If the body contains any e-* attribute AND the response doesn't
 * have X-Eka-Runtime: skip, inject the script tag into <head>.
 * ================================================================ */

/* Quick byte-scan: does the HTML body contain any "e-" attribute?
 * Looks for the pattern: whitespace + "e-" + letter
 * This is fast and avoids false positives from "e-" in text content. */
static bool has_e_attributes(const char *html, size_t len) {
    for (size_t i = 0; i + 2 < len; i++) {
        /* Look for whitespace followed by "e-" followed by a letter */
        if ((html[i] == ' ' || html[i] == '\n' || html[i] == '\t' || html[i] == '\r') &&
            html[i + 1] == 'e' && html[i + 2] == '-') {
            /* Check that next char after "e-" is a letter (valid attribute name) */
            if (i + 3 < len && ((html[i + 3] >= 'a' && html[i + 3] <= 'z') ||
                                (html[i + 3] >= 'A' && html[i + 3] <= 'Z'))) {
                return true;
            }
        }
    }
    return false;
}

/* Find the position of the closing '>' of <head ...>.
 * Returns pointer to just after '>', or NULL if not found. */
static const char *find_head_open(const char *html, size_t len) {
    for (size_t i = 0; i + 5 < len; i++) {
        if (html[i] == '<' &&
            (html[i + 1] == 'h' || html[i + 1] == 'H') &&
            (html[i + 2] == 'e' || html[i + 2] == 'E') &&
            (html[i + 3] == 'a' || html[i + 3] == 'A') &&
            (html[i + 4] == 'd' || html[i + 4] == 'D')) {
            /* Check it's followed by whitespace, >, or / */
            char next = html[i + 5];
            if (next == ' ' || next == '>' || next == '/' ||
                next == '\n' || next == '\t' || next == '\r') {
                /* Find the closing > */
                for (size_t j = i + 5; j < len; j++) {
                    if (html[j] == '>') {
                        return &html[j + 1];
                    }
                }
            }
        }
    }
    return NULL;
}

/* Inject the client runtime script tag into an HTML response body.
 * Returns a new malloc'd buffer with the injected HTML, or NULL if no injection.
 * Caller takes ownership of the returned buffer.
 * Also updates *new_len. */
static char *inject_runtime_script(const char *body, size_t body_len,
                                    const char *content_type,
                                    bool has_skip_header,
                                    size_t *new_len) {
    /* Only inject into text/html responses */
    if (!content_type || !strstr(content_type, "text/html")) {
        return NULL;
    }

    /* Opt-out header */
    if (has_skip_header) {
        return NULL;
    }

    /* Must contain e-* attributes */
    if (!has_e_attributes(body, body_len)) {
        return NULL;
    }

    /* Must have a <head> tag */
    const char *head_insert = find_head_open(body, body_len);
    if (!head_insert) {
        return NULL;
    }

    /* Build the script tag */
    const char *script_tag = "<script src=\"/_eka.js?v=" EKA_VERSION "\" defer></script>\n";
    size_t tag_len = strlen(script_tag);

    /* Insert position */
    size_t insert_offset = (size_t)(head_insert - body);

    /* Allocate new buffer */
    char *new_body = malloc(body_len + tag_len + 1);
    if (!new_body) return NULL;

    /* Copy: before head + script tag + after head */
    memcpy(new_body, body, insert_offset);
    memcpy(new_body + insert_offset, script_tag, tag_len);
    memcpy(new_body + insert_offset + tag_len, body + insert_offset, body_len - insert_offset);
    new_body[body_len + tag_len] = '\0';

    *new_len = body_len + tag_len;
    return new_body;
}

/* ================================================================
 * Execute method and get response
 * ================================================================ */

static void handle_request(eka_conn_t *conn, eka_http_request_t *req) {
    eka_server_t *s = conn->server;

    /* Virtual route: /_eka.js — serve embedded client runtime */
    if (is_eka_js_request(req->path)) {
        serve_eka_js(conn);
        return;
    }

    /* Find matching route */
    int midx = find_route(s->prog, req->method, req->path);

    if (midx < 0) {
        /* Check for static file */
        /* TODO: static file serving */
        const char *body = "404 Not Found";
        conn->response_data = build_response(body, strlen(body), 404,
                                              "text/plain", &conn->response_len);
        return;
    }

    /* ---------------------------------------------------------------
     * Worker VM: fresh VM per request with own GC arenas.
     * Globals are shallow-copied from the master VM.
     * This gives full request isolation — no shared mutable VM state.
     * --------------------------------------------------------------- */
    eka_vm_t worker;
    eka_vm_init(&worker);                    /* creates own arenas, sets eka_gc_current_vm */
    eka_vm_clone_globals(&worker, s->vm);   /* copy globals from master */

    /* Set up per-request builtins (request, response) */
    eka_builtins_setup_request(&worker, req);

    /* Set SSE context: current client + event loop */
    worker.current_client = &conn->client;
    worker.sse_loop = s->loop;

    /* Execute the method's bytecode (eKa_vm_execute sets eka_gc_current_vm internally) */
    eka_closure_t *cl = eka_closure_new(s->prog->methods[midx].func);
    const char *err = NULL;
    eka_value_t result = eka_vm_execute(&worker, cl, NULL, 0, &err);

    if (err) {
        char err_body[256];
        int len = snprintf(err_body, sizeof(err_body), "500 Internal Server Error: %s", err);
        conn->response_data = build_response(err_body, (size_t)len, 500,
                                              "text/plain", &conn->response_len);
        eka_builtins_teardown_request(&worker);
        eka_vm_free(&worker);
        return;
    }

    /* Handle redirect (read from worker VM before teardown) */
    if (worker.response_state.is_redirect) {
        char location_hdr[320];
        int loc_len = snprintf(location_hdr, sizeof(location_hdr),
                               "HTTP/1.1 %d Found\r\nLocation: %s\r\n"
                               "Content-Length: 0\r\nConnection: close\r\n\r\n",
                               worker.response_state.redirect_status,
                               worker.response_state.redirect_location);
        conn->response_data = malloc((size_t)loc_len + 1);
        if (conn->response_data) {
            memcpy(conn->response_data, location_hdr, (size_t)loc_len + 1);
            conn->response_len = (size_t)loc_len;
        }
        eka_builtins_teardown_request(&worker);
        eka_vm_free(&worker);
        return;
    }

    /* Build response from result or explicit body.
     * Must happen BEFORE teardown — body may point into worker VM memory. */
    const char *body;
    size_t body_len;
    const char *content_type;
    char *owned_body = NULL;  /* if we need to own a copy */

    if (worker.response_state.body_set) {
        /* response.html/json was called — body is heap-allocated by builtins.
         * Transfer ownership: we take it and will free it ourselves. */
        body = worker.response_state.body;
        body_len = worker.response_state.body_len;
        content_type = worker.response_state.content_type_set
                       ? worker.response_state.content_type
                       : "text/html; charset=utf-8";
        owned_body = worker.response_state.body;  /* take ownership */
        worker.response_state.body = NULL;          /* prevent double-free */
    } else if (eka_obj_is_type(result, OBJ_STRING)) {
        eka_string_t *sres = eka_as_string(result);
        body = sres->data;
        body_len = sres->length;
        content_type = worker.response_state.content_type_set
                       ? worker.response_state.content_type
                       : "text/html; charset=utf-8";
    } else if (eka_obj_is_type(result, OBJ_RAWSTRING)) {
        eka_rawstring_t *raw = eka_as_rawstring(result);
        body = raw->data;
        body_len = raw->length;
        content_type = worker.response_state.content_type_set
                       ? worker.response_state.content_type
                       : "text/html; charset=utf-8";
    } else if (eka_is_number(result) || eka_is_int(result)) {
        static char num_buf[64];
        double d = eka_is_number(result) ? eka_as_number(result)
                                         : (double)eka_as_int(result);
        int n = snprintf(num_buf, sizeof(num_buf), "%g", d);
        body = num_buf;
        body_len = (size_t)n;
        content_type = worker.response_state.content_type_set
                       ? worker.response_state.content_type
                       : "text/plain";
    } else if (eka_is_bool(result)) {
        body = eka_as_bool(result) ? "true" : "false";
        body_len = eka_as_bool(result) ? 4 : 5;
        content_type = worker.response_state.content_type_set
                       ? worker.response_state.content_type
                       : "text/plain";
    } else if (eka_is_nil(result)) {
        body = "";
        body_len = 0;
        content_type = worker.response_state.content_type_set
                       ? worker.response_state.content_type
                       : "text/plain";
    } else {
        /* Map or list — serialize as JSON */
        eka_string_t *json_s = eka_value_to_string(result);
        body = json_s->data;
        body_len = json_s->length;
        content_type = "application/json";
    }

    /* Auto-inject client runtime script if e-* attributes found.
     * Must happen after body is determined but before build_response.
     * We need to check response headers for X-Eka-Runtime: skip. */
    bool has_skip_header = false;
    for (int hi = 0; hi < worker.response_state.header_count; hi++) {
        if (strcasecmp(worker.response_state.extra_headers[hi].name, "X-Eka-Runtime") == 0 &&
            strcasecmp(worker.response_state.extra_headers[hi].value, "skip") == 0) {
            has_skip_header = true;
            break;
        }
    }

    size_t injected_len = 0;
    char *injected_body = inject_runtime_script(body, body_len, content_type,
                                                 has_skip_header, &injected_len);
    if (injected_body) {
        /* Use the injected body (takes precedence over original) */
        if (owned_body) free(owned_body);
        body = injected_body;
        body_len = injected_len;
        owned_body = injected_body;  /* so it gets freed below */
    }

    int resp_status = worker.response_state.status ? worker.response_state.status : 200;
    conn->response_data = build_response(body, body_len, resp_status,
                                          content_type, &conn->response_len);

    /* Teardown now (response already built) — this frees body if we didn't take ownership */
    eka_builtins_teardown_request(&worker);

    /* If we took ownership of the body, free it after building the response */
    if (owned_body) {
        free(owned_body);
    }

    /* Free worker VM and all its GC arenas */
    eka_vm_free(&worker);
}

/* ================================================================
 * libuv callbacks
 * ================================================================ */

static void on_close(uv_handle_t *handle) {
    eka_conn_t *conn = (eka_conn_t *)handle->data;
    if (conn->response_data) free(conn->response_data);
    free(conn);
}

static void on_write(uv_write_t *req, int status) {
    eka_conn_t *conn = (eka_conn_t *)req->data;
    (void)status;

    /* SSE connections stay alive — detect by content type */
    if (conn->response_data) {
        /* Check if this is an SSE response (text/event-stream) */
        if (strstr(conn->response_data, "text/event-stream") != NULL) {
            /* SSE: don't close, keep connection alive for streaming */
            free(req);  /* free the uv_write_t, not the connection */
            return;
        }
    }

    /* Normal HTTP: close the connection */
    uv_close((uv_handle_t *)&conn->client, on_close);
}

static void on_read(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf) {
    eka_conn_t *conn = (eka_conn_t *)client->data;

    if (nread < 0) {
        /* Error or EOF */
        if (conn->read_buf) free(conn->read_buf);
        uv_close((uv_handle_t *)client, NULL);
        return;
    }

    if (nread > 0) {
        /* Parse HTTP request */
        eka_http_request_t req;
        /* Copy buffer for parsing (we own it) */
        char *data = malloc((size_t)nread + 1);
        memcpy(data, buf->base, (size_t)nread);
        data[nread] = '\0';

        if (eka_http_parse(&req, data, (size_t)nread) == 0) {
            /* Handle the request */
            handle_request(conn, &req);

            /* Stop reading, write response */
            uv_read_stop(client);
            uv_buf_t wbuf = uv_buf_init(conn->response_data,
                                         (unsigned int)conn->response_len);
            conn->write_req.data = conn;
            uv_write(&conn->write_req, client, &wbuf, 1, on_write);
        } else {
            const char *err = "HTTP/1.1 400 Bad Request\r\n\r\n";
            size_t err_len = strlen(err);
            conn->response_data = malloc(err_len + 1);
            memcpy(conn->response_data, err, err_len + 1);
            conn->response_len = err_len;
            uv_read_stop(client);
            uv_buf_t wbuf2 = uv_buf_init(conn->response_data,
                                          (unsigned int)conn->response_len);
            conn->write_req.data = conn;
            uv_write(&conn->write_req, client, &wbuf2, 1, on_write);
        }
    }

    /* Free the uv buffer (allocated by us in alloc_cb) */
    if (buf->base) free(buf->base);
}

static void alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    (void)handle;
    *buf = uv_buf_init(malloc(suggested_size), (unsigned int)suggested_size);
}

static void on_connection(uv_stream_t *server, int status) {
    if (status < 0) return;

    eka_server_t *s = (eka_server_t *)server->data;
    eka_conn_t *conn = calloc(1, sizeof(eka_conn_t));
    conn->server = s;

    uv_tcp_init(s->loop, &conn->client);
    conn->client.data = conn;

    if (uv_accept(server, (uv_stream_t *)&conn->client) == 0) {
        uv_read_start((uv_stream_t *)&conn->client, alloc_cb, on_read);
    } else {
        uv_close((uv_handle_t *)&conn->client, NULL);
        free(conn);
    }
}

/* ================================================================
 * Server lifecycle
 * ================================================================ */

eka_server_t *eka_server_create(eka_compiled_program_t *prog,
                                 const char *static_dir, int port) {
    eka_server_t *s = calloc(1, sizeof(eka_server_t));
    s->prog       = prog;
    s->static_dir = static_dir;
    s->port       = port;
    s->loop       = malloc(sizeof(uv_loop_t));
    uv_loop_init(s->loop);

    /* Create the master VM with its own GC arenas.
     * eka_vm_init() sets eka_gc_current_vm so arena_alloc works. */
    s->vm = malloc(sizeof(eka_vm_t));
    eka_vm_init(s->vm);

    /* Register builtins before running init code */
    eka_builtins_register(s->vm);

    if (prog->init_func && prog->init_func->code_length > 0) {
        eka_closure_t *cl = eka_closure_new(prog->init_func);
        const char *err = NULL;
        eka_vm_execute_init(s->vm, cl, &err);
        if (err) {
            fprintf(stderr, "eka: init error: %s\n", err);
        }
    }

    return s;
}

int eka_server_run(eka_server_t *server) {
    struct sockaddr_in addr;
    uv_ip4_addr("0.0.0.0", server->port, &addr);

    uv_tcp_init(server->loop, &server->listener);
    server->listener.data = server;

    int r = uv_tcp_bind(&server->listener, (const struct sockaddr *)&addr, 0);
    if (r) {
        fprintf(stderr, "eka: bind error: %s\n", uv_strerror(r));
        return 1;
    }

    r = uv_listen((uv_stream_t *)&server->listener, 128, on_connection);
    if (r) {
        fprintf(stderr, "eka: listen error: %s\n", uv_strerror(r));
        return 1;
    }

    printf("Eka v%s — http://localhost:%d\n", EKA_VERSION, server->port);
    printf("Press Ctrl+C to stop.\n");

    server->running = true;
    r = uv_run(server->loop, UV_RUN_DEFAULT);
    server->running = false;
    return r;
}

void eka_server_stop(eka_server_t *server) {
    if (server->running && server->loop) {
        uv_stop(server->loop);
    }
}

void eka_server_free(eka_server_t *server) {
    if (server->vm) {
        eka_vm_free(server->vm);
        free(server->vm);
    }
    if (server->loop) {
        uv_loop_close(server->loop);
        free(server->loop);
    }
    free(server);
}
