#ifndef BUILTINS_H
#define BUILTINS_H

#include "core/vm.h"
#include "runtime/http.h"

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

#endif /* BUILTINS_H */
