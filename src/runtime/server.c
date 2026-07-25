#define _GNU_SOURCE
#include "runtime/server.h"
#include "runtime/http.h"
#include "core/vm.h"
#include "core/obj.h"
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
 * Execute method and get response
 * ================================================================ */

static void handle_request(eka_conn_t *conn, eka_http_request_t *req) {
    eka_server_t *s = conn->server;

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

    /* Execute the method's bytecode */
    eka_closure_t *cl = eka_closure_new(s->prog->methods[midx].func);
    const char *err = NULL;
    eka_value_t result = eka_vm_execute(s->vm, cl, NULL, 0, &err);

    if (err) {
        char err_body[256];
        int len = snprintf(err_body, sizeof(err_body), "500 Internal Server Error: %s", err);
        conn->response_data = build_response(err_body, (size_t)len, 500,
                                              "text/plain", &conn->response_len);
        return;
    }

    /* Build response from result */
    const char *body;
    size_t body_len;
    const char *content_type;

    if (eka_obj_is_type(result, OBJ_STRING)) {
        eka_string_t *s = eka_as_string(result);
        body = s->data;
        body_len = s->length;
        content_type = "text/html; charset=utf-8";
    } else if (eka_is_number(result) || eka_is_int(result)) {
        /* Convert number to string */
        /* For simplicity, use a static buffer — not thread-safe but OK for V1 */
        static char num_buf[64];
        double d = eka_is_number(result) ? eka_as_number(result)
                                         : (double)eka_as_int(result);
        int n = snprintf(num_buf, sizeof(num_buf), "%g", d);
        body = num_buf;
        body_len = (size_t)n;
        content_type = "text/plain";
    } else if (eka_is_bool(result)) {
        body = eka_as_bool(result) ? "true" : "false";
        body_len = eka_as_bool(result) ? 4 : 5;
        content_type = "text/plain";
    } else if (eka_is_nil(result)) {
        body = "";
        body_len = 0;
        content_type = "text/plain";
    } else {
        body = "";
        body_len = 0;
        content_type = "text/plain";
    }

    conn->response_data = build_response(body, body_len, 200,
                                          content_type, &conn->response_len);
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
    /* Close the connection — on_close will free */
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

    /* Create the init VM and execute init code */
    s->vm = arena_alloc(sizeof(eka_vm_t));
    eka_vm_init(s->vm);
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
    if (server->loop) {
        uv_loop_close(server->loop);
        free(server->loop);
    }
    free(server);
}
