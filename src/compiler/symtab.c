#include "compiler/symtab.h"

#include <stdlib.h>
#include <string.h>

#include "core/obj.h"  /* for arena_alloc */

scope_t *symtab_new_scope(scope_t *enclosing, bool is_function) {
    scope_t *scope = arena_alloc(sizeof(scope_t));
    memset(scope, 0, sizeof(*scope));
    scope->enclosing   = enclosing;
    scope->is_function = is_function;
    scope->depth       = enclosing ? enclosing->depth + 1 : 0;
    return scope;
}

sym_t *symtab_add_local(scope_t *scope, const char *name, size_t len) {
    /* Check for duplicate in current scope */
    for (sym_t *s = scope->symbols; s; s = s->next) {
        if (s->name_len == len && memcmp(s->name, name, len) == 0) {
            return NULL;  /* already defined */
        }
    }

    sym_t *sym = arena_alloc(sizeof(sym_t));
    sym->next        = scope->symbols;
    sym->name        = name;
    sym->name_len    = len;
    sym->kind        = SYM_LOCAL;
    sym->index       = scope->local_count++;
    sym->is_captured = false;

    scope->symbols = sym;
    return sym;
}

sym_t *symtab_resolve(scope_t *scope, const char *name, size_t len) {
    for (scope_t *s = scope; s; s = s->enclosing) {
        for (sym_t *sym = s->symbols; sym; sym = sym->next) {
            if (sym->name_len == len && memcmp(sym->name, name, len) == 0) {
                return sym;
            }
        }
        /* Don't cross function boundaries for local resolution */
        if (s->is_function && s != scope) break;
    }
    return NULL;
}

sym_t *symtab_resolve_upvalue(scope_t *scope, const char *name, size_t len) {
    /* Walk enclosing scopes, crossing function boundaries.
     * The first function boundary we cross determines upvalue depth. */
    scope_t *s = scope->enclosing;
    while (s) {
        for (sym_t *sym = s->symbols; sym; sym = sym->next) {
            if (sym->name_len == len && memcmp(sym->name, name, len) == 0) {
                if (sym->kind == SYM_LOCAL) {
                    /* Mark as captured */
                    sym->is_captured = true;
                }
                return sym;
            }
        }
        s = s->enclosing;
    }
    return NULL;
}

void symtab_free_scope(scope_t *scope) {
    /* Arena-allocated — nothing to free individually */
    (void)scope;
}
