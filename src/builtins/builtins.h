#ifndef BUILTINS_H
#define BUILTINS_H

#include "core/vm.h"
#include "runtime/http.h"

#define EKA_SESSION_TTL (7 * 24 * 3600)  /* 7 days in seconds */

/* Set the FS sandbox project root from the .eka file path.
 * All fs.* operations are confined to this directory.
 * Call once at startup before serving requests. */
void eka_fs_set_project_root(const char *filepath);

/*
 * Eka builtins — the 12 Phase 1 builtin functions.
 *
 * Init-scope builtins (registered once at server startup):
 *   print, sqlite.open, json, html, str, crypto, env,
 *   datetime, markdown, cache
 *
 * Per-request builtins (re-registered before each route handler):
 *   request, response
 */

/* Register all init-scope builtins into vm->globals. */
void eka_builtins_register(eka_vm_t *vm);

/* Set up per-request builtins (request + response).
 * Call before executing each route handler. */
void eka_builtins_setup_request(eka_vm_t *vm, eka_http_request_t *req);

/* Reset per-request state. Call after route handler returns. */
void eka_builtins_teardown_request(eka_vm_t *vm);

/* --- Session lifecycle (called by server.c) --- */

/* Initialize the session database (call once at server startup).
 * Creates the eka_sessions table if it doesn't exist. */
void eka_session_init_db(eka_vm_t *vm);

/* Load session from SQLite into vm->session_data.
 * Reads cookie from vm->current_req, loads session row, populates map.
 * Sets vm->session_id, vm->session_is_new. */
void eka_session_load(eka_vm_t *vm);

/* Save vm->session_data back to SQLite.
 * Generates session_id for new sessions. Sets Set-Cookie header if needed.
 * No-op if session is clean and not new. */
void eka_session_save(eka_vm_t *vm);

#endif /* BUILTINS_H */
