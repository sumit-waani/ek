#ifndef COMPILER_H
#define COMPILER_H

#include "parser/ast.h"
#include "core/vm.h"

#include <stdbool.h>

/*
 * Compiler: AST → bytecode.
 *
 * Two-phase compilation:
 *   1. Init phase: top-level code outside method blocks
 *   2. Request phase: each method block
 *
 * The compiler produces eka_func_t objects that the VM can execute.
 */

typedef struct eka_compiled_program_t {
    /* Init function (runs once at startup) */
    eka_func_t   *init_func;

    /* Method blocks: one function per route */
    /* For V1: simple array, max 32 routes */
    #define MAX_METHODS 32
    struct {
        eka_token_type_t method;   /* @get, @post, etc. */
        const char      *path;     /* route path string */
        eka_func_t      *func;     /* compiled handler */
    } methods[MAX_METHODS];
    int method_count;

    /* Error info */
    bool        had_error;
    const char *error_msg;
} eka_compiled_program_t;

/* Compile an AST program. */
eka_compiled_program_t *eka_compile(ast_node_t *program);

/* Free compiled program. */
void eka_compile_free(eka_compiled_program_t *prog);

#endif /* COMPILER_H */
