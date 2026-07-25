#ifndef SERVER_H
#define SERVER_H

#include "compiler/compiler.h"

#include <stdbool.h>

/*
 * Eka HTTP server backed by libuv.
 */

typedef struct eka_server_t eka_server_t;

/* Create a new server with compiled program and port. */
eka_server_t *eka_server_create(eka_compiled_program_t *prog,
                                 const char *static_dir,
                                 int port);

/* Start the server (blocking — runs the event loop). */
int eka_server_run(eka_server_t *server);

/* Stop the server. */
void eka_server_stop(eka_server_t *server);

/* Free server resources. */
void eka_server_free(eka_server_t *server);

#endif /* SERVER_H */
