#ifndef VM_H
#define VM_H

#include "core/value.h"
#include "core/obj.h"
#include "core/bytecode.h"

#include <stdint.h>
#include <stddef.h>

/* Max call frames */
#define EKA_MAX_CALL_FRAMES 256

/* Max registers per frame */
#define EKA_MAX_REGISTERS   256

/* Call frame */
typedef struct {
    eka_closure_t *closure;      /* the function being executed */
    eka_instr_t   *ip;           /* instruction pointer within closure->func->code */
    eka_value_t   *registers;    /* base of register array for this frame */
    eka_value_t   *stack_top;    /* for passing args to next call */
} eka_call_frame_t;

/* VM state */
typedef struct {
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
} eka_vm_t;

/* --- VM lifecycle --- */

void eka_vm_init(eka_vm_t *vm);
void eka_vm_free(eka_vm_t *vm);

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
