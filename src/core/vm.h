#ifndef VM_H
#define VM_H

#include "core/value.h"
#include "core/obj.h"
#include "core/bytecode.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Max call frames */
#define EKA_MAX_CALL_FRAMES 256

/* Max registers per frame */
#define EKA_MAX_REGISTERS   256

/* Max DB connections */
#define EKA_MAX_DB_CONNS    8

/* Max response headers set via response.header() */
#define EKA_MAX_RESP_HEADERS 8

/* Forward declarations */
struct sqlite3;
struct arena_t;
typedef struct eka_http_request_t eka_http_request_t;
typedef struct uv_tcp_s         uv_tcp_t;

/* GC arena linked list node */
typedef struct arena_t {
    struct arena_t *next;
    uint8_t        *base;
    uint8_t        *top;
    uint8_t        *end;
} arena_t;

/* Call frame */
typedef struct {
    eka_closure_t *closure;      /* the function being executed */
    eka_instr_t   *ip;           /* instruction pointer within closure->func->code */
    eka_value_t   *registers;    /* base of register array for this frame */
    eka_value_t   *stack_top;    /* for passing args to next call */
} eka_call_frame_t;

/* Per-DB connection state */
typedef struct {
    struct sqlite3  *db;
    int64_t          last_id;
} eka_db_conn_t;

/* Per-request response state (mutated by response.* builtins) */
typedef struct {
    int         status;
    bool        is_redirect;
    char        redirect_location[256];
    int         redirect_status;
    char        content_type[64];
    bool        content_type_set;

    struct {
        char name[64];
        char value[256];
    } extra_headers[EKA_MAX_RESP_HEADERS];
    int         header_count;

    bool        body_set;
    char       *body;          /* owned, arena-allocated pointer */
    size_t      body_len;
} eka_response_state_t;

/* VM state */
typedef struct eka_vm_t {
    eka_call_frame_t frames[EKA_MAX_CALL_FRAMES];
    int              frame_count;      /* current depth */

    /* Global values (init-scope variables, read-only from requests) */
    eka_map_t       *globals;

    /* Open upvalues (linked list per stack slot) */
    eka_upvalue_t   *open_upvalues;

    /* Temporary for storing the return value */
    eka_value_t      return_value;

    /* GC roots tracking */
    bool             gc_roots_dirty;

    /* --- Per-request context --- */
    eka_http_request_t   *current_req;
    eka_response_state_t  response_state;

    /* --- SQLite connections --- */
    eka_db_conn_t    db_conns[EKA_MAX_DB_CONNS];
    int              db_conn_count;

    /* --- Cache --- */
    eka_map_t       *cache_store;

    /* --- SSE connections --- */
    #define EKA_MAX_SSE_CONNS 128
    void           *sse_clients[EKA_MAX_SSE_CONNS];  /* uv_tcp_t* */
    int             sse_client_count;
    void           *sse_loop;                         /* uv_loop_t*, for sse.send */
    void           *current_client;                   /* uv_tcp_t*, set per-request */
    int             sse_current_idx;                  /* index in sse_clients, or -1 */

    /* --- Session store (SQLite) --- */
    void           *session_db;                       /* sqlite3* */

    /* --- GC state (per-VM arenas) --- */
    arena_t        *gc_arenas;          /* linked list of all arenas */
    arena_t        *gc_current;         /* current arena for allocation */
    eka_obj_t      *gc_all_objects;     /* head of allocated objects list */
    size_t          gc_bytes_allocated; /* bytes allocated since last GC */
    size_t          gc_next_gc;         /* threshold for triggering GC */
    bool            gc_running;         /* true during GC */

    /* GC roots */
    #define EKA_GC_MAX_ROOTS 256
    eka_value_t     gc_roots[EKA_GC_MAX_ROOTS];
    int             gc_root_count;
} eka_vm_t;

/* Global: which VM's GC arena is currently active.
 * Set before executing bytecode or allocating objects.
 * The init/server VM keeps this set for its lifetime.
 * Worker VMs set it before execution and restore after. */
extern eka_vm_t *eka_gc_current_vm;

/* --- VM lifecycle --- */

void eka_vm_init(eka_vm_t *vm);
void eka_vm_free(eka_vm_t *vm);

/* Create a worker VM with the same global variables as the master.
 * Globals are shallow-copied (same object pointers) — the master VM's
 * globals must remain alive for the worker's lifetime. */
void eka_vm_clone_globals(eka_vm_t *dest, const eka_vm_t *src);

/* Set a global variable (init phase) */
void eka_vm_set_global(eka_vm_t *vm, const char *name, eka_value_t value);

/* Get a global variable */
eka_value_t eka_vm_get_global(eka_vm_t *vm, const char *name);

/* --- Execution --- */

/*
 * Execute a closure. Returns the result value.
 * On runtime error, returns eka_nil() and sets *error (if error != NULL).
 */
eka_value_t eka_vm_execute(eka_vm_t *vm, eka_closure_t *closure,
                           eka_value_t *args, int arg_count,
                           const char **error);

/*
 * Execute init code (a closure with no args).
 */
eka_value_t eka_vm_execute_init(eka_vm_t *vm, eka_closure_t *closure,
                                const char **error);

#endif /* VM_H */
