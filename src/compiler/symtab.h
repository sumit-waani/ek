#ifndef SYMTAB_H
#define SYMTAB_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Symbol table for the Eka compiler.
 *
 * Tracks local variables within a scope, resolves names to either
 * local register slots, upvalue indices, or global lookups.
 */

typedef enum {
    SYM_LOCAL,    /* local variable in a register slot */
    SYM_UPVALUE,  /* captured from enclosing scope */
    SYM_GLOBAL,   /* global variable (init-scope) */
} sym_kind_t;

typedef struct sym_t {
    struct sym_t *next;      /* linked list within scope */
    const char   *name;
    size_t        name_len;
    sym_kind_t    kind;
    uint32_t      index;     /* register slot or upvalue index */
    bool          is_captured; /* true if enclosed function captures this */
} sym_t;

typedef struct scope_t {
    struct scope_t *enclosing;
    sym_t          *symbols;     /* linked list */
    uint32_t        local_count; /* next available register slot */
    uint32_t        depth;       /* nesting depth (0 = global/function) */
    bool            is_function; /* this scope is a function boundary */
} scope_t;

/* Create a new scope. If enclosing is NULL, this is the global scope. */
scope_t *symtab_new_scope(scope_t *enclosing, bool is_function);

/* Add a local variable. Returns the symbol, or NULL on conflict. */
sym_t *symtab_add_local(scope_t *scope, const char *name, size_t len);

/* Resolve a name. Returns NULL if not found. */
sym_t *symtab_resolve(scope_t *scope, const char *name, size_t len);

/* Resolve for upvalue capture: walk enclosing scopes until found. */
sym_t *symtab_resolve_upvalue(scope_t *scope, const char *name, size_t len);

/* Free scope and all symbols. */
void symtab_free_scope(scope_t *scope);

#endif /* SYMTAB_H */
