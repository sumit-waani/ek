#include "core/vm.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ================================================================
 * List & Map method dispatch — native functions for .push() etc.
 *
 * These are called via OP_GET_PROP → OP_CALL. The target object
 * (list or map) is passed via the native's ctx pointer.
 * ================================================================ */

/* --- List methods --- */

static eka_value_t native_list_push(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm;
    eka_list_t *list = (eka_list_t *)ctx;
    for (int i = 0; i < argc; i++) {
        eka_list_push(list, args[i]);
    }
    return eka_nil();
}

static eka_value_t native_list_pop(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)argc; (void)args;
    eka_list_t *list = (eka_list_t *)ctx;
    return eka_list_pop(list);
}

static eka_value_t native_list_insert(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm;
    if (argc < 2) return eka_nil();
    eka_list_t *list = (eka_list_t *)ctx;
    int64_t idx = eka_is_int(args[0]) ? eka_as_int(args[0])
                : eka_is_number(args[0]) ? (int64_t)eka_as_number(args[0]) : 0;
    eka_list_insert(list, (uint32_t)idx, args[1]);
    return eka_nil();
}

static eka_value_t native_list_removeAt(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm;
    if (argc < 1) return eka_nil();
    eka_list_t *list = (eka_list_t *)ctx;
    int64_t idx = eka_is_int(args[0]) ? eka_as_int(args[0])
                : eka_is_number(args[0]) ? (int64_t)eka_as_number(args[0]) : 0;
    eka_list_remove_at(list, (uint32_t)idx);
    return eka_nil();
}

static eka_value_t native_list_removeValue(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm;
    if (argc < 1) return eka_nil();
    eka_list_t *list = (eka_list_t *)ctx;
    eka_value_t target = args[0];
    for (uint32_t i = 0; i < list->length; i++) {
        /* Compare: for objects, pointer equality; for values, bitwise */
        bool match = false;
        if (eka_is_obj(target) && eka_is_obj(list->items[i])) {
            match = (eka_as_obj(target) == eka_as_obj(list->items[i]));
        } else {
            match = (target == list->items[i]);
        }
        if (match) {
            eka_list_remove_at(list, i);
            return eka_nil();
        }
    }
    return eka_nil();
}

static eka_value_t native_list_indexOf(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm;
    if (argc < 1) return eka_nil();
    eka_list_t *list = (eka_list_t *)ctx;
    eka_value_t target = args[0];
    for (uint32_t i = 0; i < list->length; i++) {
        bool match = false;
        if (eka_is_obj(target) && eka_is_obj(list->items[i])) {
            match = (eka_as_obj(target) == eka_as_obj(list->items[i]));
        } else {
            match = (target == list->items[i]);
        }
        if (match) return eka_int((int64_t)i);
    }
    return eka_nil();
}

static eka_value_t native_list_contains(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm;
    if (argc < 1) return eka_bool(false);
    eka_list_t *list = (eka_list_t *)ctx;
    eka_value_t target = args[0];
    for (uint32_t i = 0; i < list->length; i++) {
        bool match = false;
        if (eka_is_obj(target) && eka_is_obj(list->items[i])) {
            match = (eka_as_obj(target) == eka_as_obj(list->items[i]));
        } else {
            match = (target == list->items[i]);
        }
        if (match) return eka_bool(true);
    }
    return eka_bool(false);
}

/* --- Map methods --- */

static eka_value_t native_map_keys(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)argc; (void)args;
    eka_map_t *map = (eka_map_t *)ctx;
    eka_list_t *keys = eka_list_new(map->length > 0 ? map->length : 4);
    for (uint32_t i = 0; i < map->capacity; i++) {
        if (map->entries[i].key && !eka_map_entry_is_tombstone(map->entries[i].key)) {
            eka_list_push(keys, eka_string_val(map->entries[i].key));
        }
    }
    return eka_list_val(keys);
}

static eka_value_t native_map_values(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)argc; (void)args;
    eka_map_t *map = (eka_map_t *)ctx;
    eka_list_t *vals = eka_list_new(map->length > 0 ? map->length : 4);
    for (uint32_t i = 0; i < map->capacity; i++) {
        if (map->entries[i].key && !eka_map_entry_is_tombstone(map->entries[i].key)) {
            eka_list_push(vals, map->entries[i].value);
        }
    }
    return eka_list_val(vals);
}

static eka_value_t native_map_has(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm;
    if (argc < 1) return eka_bool(false);
    eka_map_t *map = (eka_map_t *)ctx;
    if (!eka_obj_is_type(args[0], OBJ_STRING)) return eka_bool(false);
    return eka_bool(eka_map_has(map, eka_as_string(args[0])));
}

static eka_value_t native_map_delete_fn(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm;
    if (argc < 1) return eka_nil();
    eka_map_t *map = (eka_map_t *)ctx;
    if (!eka_obj_is_type(args[0], OBJ_STRING)) return eka_nil();
    eka_map_delete(map, eka_as_string(args[0]));
    return eka_nil();
}

/* Look up a method on a list object. Returns the bound native, or nil. */
static eka_value_t list_method_dispatch(eka_list_t *list, eka_string_t *name) {
    const char *n = name->data;
    uint32_t len = name->length;

    /* Fast path: length property */
    if (len == 6 && memcmp(n, "length", 6) == 0)
        return eka_int((int64_t)list->length);

    eka_native_fn_t fn = NULL;
    if (len == 4 && memcmp(n, "push", 4) == 0)         fn = native_list_push;
    else if (len == 3 && memcmp(n, "pop", 3) == 0)     fn = native_list_pop;
    else if (len == 6 && memcmp(n, "insert", 6) == 0)  fn = native_list_insert;
    else if (len == 9 && memcmp(n, "removeAt", 9) == 0) fn = native_list_removeAt;
    else if (len == 11 && memcmp(n, "removeValue", 11) == 0) fn = native_list_removeValue;
    else if (len == 7 && memcmp(n, "indexOf", 7) == 0)  fn = native_list_indexOf;
    else if (len == 8 && memcmp(n, "contains", 8) == 0) fn = native_list_contains;

    if (!fn) return eka_nil();
    eka_native_t *nat = eka_native_new(fn, (void *)list, n);
    return eka_native_val(nat);
}

/* Look up a method on a map object. Returns the bound native, or nil. */
static eka_value_t map_method_dispatch(eka_map_t *map, eka_string_t *name) {
    const char *n = name->data;
    uint32_t len = name->length;

    /* Fast path: length property */
    if (len == 6 && memcmp(n, "length", 6) == 0)
        return eka_int((int64_t)map->length);

    eka_native_fn_t fn = NULL;
    if (len == 4 && memcmp(n, "keys", 4) == 0)          fn = native_map_keys;
    else if (len == 6 && memcmp(n, "values", 6) == 0)   fn = native_map_values;
    else if (len == 3 && memcmp(n, "has", 3) == 0)      fn = native_map_has;
    else if (len == 6 && memcmp(n, "delete", 6) == 0)    fn = native_map_delete_fn;

    if (!fn) return eka_nil();
    eka_native_t *nat = eka_native_new(fn, (void *)map, n);
    return eka_native_val(nat);
}

/* ================================================================
 * VM lifecycle
 * ================================================================ */

void eka_vm_init(eka_vm_t *vm) {
    memset(vm, 0, sizeof(*vm));

    /* Set this VM as the current GC target before allocating */
    eka_gc_current_vm = vm;

    /* Init GC state */
    vm->gc_bytes_allocated = 0;
    vm->gc_next_gc = EKA_ARENA_SIZE;
    vm->gc_running = false;
    vm->gc_arenas = NULL;
    vm->gc_current = NULL;
    vm->gc_all_objects = NULL;
    vm->gc_root_count = 0;

    vm->globals = eka_map_new(64);
    vm->cache_store = eka_map_new(64);
    vm->response_state.status = 200;
    vm->response_state.content_type_set = false;
    vm->response_state.is_redirect = false;
    vm->response_state.header_count = 0;
    vm->response_state.body_set = false;
    vm->response_state.body = NULL;
    vm->response_state.body_len = 0;
}

void eka_vm_free(eka_vm_t *vm) {
    /* First: free heap-allocated data arrays for lists/maps.
     * These are malloc'd separately from the arena and must be freed
     * before the arena memory (where the object headers live) is released. */
    eka_obj_free_heap_data(vm);

    /* Free all GC arenas (this drops all object headers allocated by this VM) */
    arena_t *a = vm->gc_arenas;
    while (a) {
        arena_t *next = a->next;
        free(a->base);
        free(a);
        a = next;
    }
    vm->gc_arenas = NULL;
    vm->gc_current = NULL;
    vm->gc_all_objects = NULL;
    vm->gc_bytes_allocated = 0;
}

void eka_vm_clone_globals(eka_vm_t *dest, const eka_vm_t *src) {
    /* Shallow-copy the globals map pointer.
     * Both VMs share the same global objects (in src's arenas).
     * src must outlive dest — typically src is the master VM. */
    eka_vm_t *prev = eka_gc_current_vm;
    eka_gc_current_vm = dest;

    /* Create a fresh globals map in dest's arena, then copy all entries */
    dest->globals = eka_map_new(64);
    eka_map_t *src_globals = src->globals;

    if (src_globals) {
        for (uint32_t i = 0; i < src_globals->capacity; i++) {
            eka_map_entry_t *e = &src_globals->entries[i];
            if (e->key && !eka_map_entry_is_tombstone(e->key)) {
                eka_map_set(dest->globals, e->key, e->value);
            }
        }
    }

    /* Share the cache store (pointer copy — mutations are shared) */
    /* But NOTE: if dest allocates new cache entries, they go into dest's arena.
     * This is acceptable for V1 — cache is volatile and served from
     * the master VM's store via shared pointer. */
    dest->cache_store = src->cache_store;

    /* Share session DB and DB connections (shared state) */
    dest->session_db = src->session_db;
    dest->db_conn_count = src->db_conn_count;
    for (int i = 0; i < src->db_conn_count; i++) {
        dest->db_conns[i] = src->db_conns[i];
    }

    /* Share SSE connection tracking — point to master's live state */
    dest->sse_loop = src->sse_loop;
    dest->sse_master = (eka_vm_t *)src;  /* worker uses master's SSE list */

    eka_gc_current_vm = prev;
}

void eka_vm_set_global(eka_vm_t *vm, const char *name, eka_value_t value) {
    eka_string_t *key = eka_string_intern(name, strlen(name));
    eka_map_set(vm->globals, key, value);
}

eka_value_t eka_vm_get_global(eka_vm_t *vm, const char *name) {
    eka_string_t *key = eka_string_intern(name, strlen(name));
    return eka_map_get(vm->globals, key);
}

/* ================================================================
 * Truthiness
 * ================================================================ */

static bool is_truthy(eka_value_t v) {
    if (eka_is_nil(v)) return false;
    if (eka_is_bool(v)) return eka_as_bool(v);
    if (eka_is_int(v)) return eka_as_int(v) != 0;
    if (eka_is_number(v)) return eka_as_number(v) != 0.0;
    /* All objects are truthy, including empty strings/lists/maps */
    return true;
}

/* ================================================================
 * String concatenation helper
 * ================================================================ */

static eka_value_t string_concat(eka_value_t a, eka_value_t b) {
    const char *sa = eka_as_string(a)->data;
    const char *sb = eka_as_string(b)->data;
    size_t la = eka_as_string(a)->length;
    size_t lb = eka_as_string(b)->length;

    size_t total = la + lb;
    char *buf = malloc(total + 1);
    if (!buf) return eka_nil();

    memcpy(buf, sa, la);
    memcpy(buf + la, sb, lb);
    buf[total] = '\0';

    eka_string_t *result = eka_string_take(buf, total);
    return eka_string_val(result);
}

/* ================================================================
 * Arithmetic helpers
 * ================================================================ */

static double value_to_double(eka_value_t v) {
    if (eka_is_number(v)) return eka_as_number(v);
    if (eka_is_int(v))    return (double)eka_as_int(v);
    return 0.0;
}

/* ================================================================
 * Error helper
 * ================================================================ */

static void set_error(const char **error_ptr, const char *msg) {
    if (error_ptr) *error_ptr = msg;
}

/* ================================================================
 * VM execution
 * ================================================================ */

#define READ_BYTE()   (*frame->ip++)
#define READ_INSTR()  (*frame->ip++)
#define PEEK_INSTR()  (*frame->ip)

/* Inner execution — assumes eka_gc_current_vm is already set. */
static eka_value_t eka_vm_execute_inner(eka_vm_t *vm, eka_closure_t *closure,
                                        eka_value_t *args, int arg_count,
                                        const char **error) {
    eka_func_t *func = closure->func;

    /* Set up the initial call frame */
    eka_call_frame_t *frame = &vm->frames[vm->frame_count++];
    frame->closure   = closure;
    frame->ip        = func->code;
    frame->registers = arena_alloc(sizeof(eka_value_t) * EKA_MAX_REGISTERS);
    frame->stack_top = frame->registers;

    /* Zero-initialise registers */
    for (int i = 0; i < EKA_MAX_REGISTERS; i++) {
        frame->registers[i] = eka_nil();
    }

    /* Copy arguments into registers starting at R(0) */
    int nargs = arg_count < (int)func->max_arity ? arg_count : (int)func->max_arity;
    for (int i = 0; i < nargs; i++) {
        frame->registers[i] = args[i];
    }
    /* Fill missing args with nil (or default values — TODO) */
    for (int i = nargs; i < (int)func->max_arity; i++) {
        frame->registers[i] = eka_nil();
    }

    /* Execution loop */
    for (;;) {
        eka_instr_t instr = READ_INSTR();
        eka_opcode_t op = eka_instr_opcode(instr);
        uint8_t a = eka_instr_a(instr);
        uint8_t b = eka_instr_b(instr);
        uint8_t c = eka_instr_c(instr);

        switch (op) {

        /* --- Constants --- */

        case OP_LOAD_CONST:
            if (b < func->constants_count) {
                frame->registers[a] = func->constants[b];
            }
            break;

        case OP_LOAD_NIL:
            frame->registers[a] = eka_nil();
            break;

        case OP_LOAD_BOOL:
            frame->registers[a] = eka_bool(b != 0);
            break;

        case OP_LOAD_INT: {
            int16_t val = eka_instr_small_int(instr);
            frame->registers[a] = eka_int(val);
            break;
        }

        case OP_MOVE:
            frame->registers[a] = frame->registers[b];
            break;

        /* --- Arithmetic --- */

        case OP_ADD: {
            eka_value_t lhs = frame->registers[b];
            eka_value_t rhs = frame->registers[c];
            /* String concatenation if either operand is a string */
            if (eka_obj_is_type(lhs, OBJ_STRING) || eka_obj_is_type(rhs, OBJ_STRING)) {
                /* Convert both to string */
                eka_string_t *sl = eka_obj_is_type(lhs, OBJ_STRING)
                    ? eka_as_string(lhs) : eka_value_to_string(lhs);
                eka_string_t *sr = eka_obj_is_type(rhs, OBJ_STRING)
                    ? eka_as_string(rhs) : eka_value_to_string(rhs);
                frame->registers[a] = string_concat(
                    eka_string_val(sl), eka_string_val(sr));
            } else {
                double result = value_to_double(lhs) + value_to_double(rhs);
                if (eka_is_int(lhs) && eka_is_int(rhs) && result == floor(result) &&
                    result >= -35184372088832.0 && result <= 35184372088831.0) {
                    frame->registers[a] = eka_int((int64_t)result);
                } else {
                    frame->registers[a] = eka_number(result);
                }
            }
            break;
        }

        case OP_SUB: {
            eka_value_t lhs = frame->registers[b];
            eka_value_t rhs = frame->registers[c];
            double result = value_to_double(lhs) - value_to_double(rhs);
            if (eka_is_int(lhs) && eka_is_int(rhs) && result == floor(result) &&
                result >= -35184372088832.0 && result <= 35184372088831.0) {
                frame->registers[a] = eka_int((int64_t)result);
            } else {
                frame->registers[a] = eka_number(result);
            }
            break;
        }

        case OP_MUL: {
            eka_value_t lhs = frame->registers[b];
            eka_value_t rhs = frame->registers[c];
            double result = value_to_double(lhs) * value_to_double(rhs);
            if (eka_is_int(lhs) && eka_is_int(rhs) && result == floor(result) &&
                result >= -35184372088832.0 && result <= 35184372088831.0) {
                frame->registers[a] = eka_int((int64_t)result);
            } else {
                frame->registers[a] = eka_number(result);
            }
            break;
        }

        case OP_DIV: {
            eka_value_t lhs = frame->registers[b];
            double divisor = value_to_double(frame->registers[c]);
            if (divisor == 0.0) {
                set_error(error, "division by zero");
                return eka_nil();
            }
            double result = value_to_double(lhs) / divisor;
            if (eka_is_int(lhs) && eka_is_int(frame->registers[c]) && result == floor(result) &&
                result >= -35184372088832.0 && result <= 35184372088831.0) {
                frame->registers[a] = eka_int((int64_t)result);
            } else {
                frame->registers[a] = eka_number(result);
            }
            break;
        }

        case OP_MOD: {
            double divisor = value_to_double(frame->registers[c]);
            if (divisor == 0.0) {
                set_error(error, "modulo by zero");
                return eka_nil();
            }
            frame->registers[a] = eka_number(fmod(value_to_double(frame->registers[b]), divisor));
            break;
        }

        case OP_NEG: {
            eka_value_t val = frame->registers[b];
            if (eka_is_int(val)) {
                frame->registers[a] = eka_int(-eka_as_int(val));
            } else {
                frame->registers[a] = eka_number(-value_to_double(val));
            }
            break;
        }

        /* --- Comparison --- */

        case OP_EQ: {
            bool equal = false;
            eka_value_t lhs = frame->registers[b];
            eka_value_t rhs = frame->registers[c];
            if (eka_is_nil(lhs) && eka_is_nil(rhs)) {
                equal = true;
            } else if (eka_is_bool(lhs) && eka_is_bool(rhs)) {
                equal = (eka_as_bool(lhs) == eka_as_bool(rhs));
            } else if (eka_is_int(lhs) && eka_is_int(rhs)) {
                equal = (eka_as_int(lhs) == eka_as_int(rhs));
            } else if (eka_is_number(lhs) && eka_is_number(rhs)) {
                equal = (eka_as_number(lhs) == eka_as_number(rhs));
            } else if (eka_is_obj(lhs) && eka_is_obj(rhs)) {
                equal = (eka_as_obj(lhs) == eka_as_obj(rhs));
            }
            /* Mixed types: just compare the raw bits (cheap and fast) */
            if (!eka_is_obj(lhs) && !eka_is_obj(rhs) &&
                !eka_is_nil(lhs) && !eka_is_nil(rhs) &&
                !eka_is_bool(lhs) && !eka_is_bool(rhs)) {
                equal = (lhs == rhs);
            }
            frame->registers[a] = eka_bool(equal);
            break;
        }

        case OP_LT:
            frame->registers[a] = eka_bool(
                value_to_double(frame->registers[b]) <
                value_to_double(frame->registers[c]));
            break;

        case OP_LE:
            frame->registers[a] = eka_bool(
                value_to_double(frame->registers[b]) <=
                value_to_double(frame->registers[c]));
            break;

        /* --- Logic --- */

        case OP_NOT:
            frame->registers[a] = eka_bool(!is_truthy(frame->registers[b]));
            break;

        case OP_AND:
            /* Short-circuit: if B is falsy → B, else C */
            frame->registers[a] = is_truthy(frame->registers[b])
                                  ? frame->registers[c]
                                  : frame->registers[b];
            break;

        case OP_OR:
            /* Short-circuit: if B is truthy → B, else C */
            frame->registers[a] = is_truthy(frame->registers[b])
                                  ? frame->registers[b]
                                  : frame->registers[c];
            break;

        /* --- Control flow --- */

        case OP_JUMP:
            frame->ip += eka_instr_offset(instr);
            break;

        case OP_JUMP_IF_FALSE:
            if (!is_truthy(frame->registers[a])) {
                frame->ip += eka_instr_offset(instr);
            }
            break;

        case OP_JUMP_IF_TRUE:
            if (is_truthy(frame->registers[a])) {
                frame->ip += eka_instr_offset(instr);
            }
            break;

        /* --- Functions --- */

        case OP_CALL: {
            eka_value_t callee = frame->registers[b];
            int arg_count_call = c;

            if (eka_obj_is_type(callee, OBJ_CLOSURE)) {
                eka_closure_t *cl = eka_as_closure(callee);
                int nargs_to_pass = arg_count_call < (int)cl->func->max_arity
                                    ? arg_count_call : (int)cl->func->max_arity;

                /* Check stack depth */
                if (vm->frame_count >= EKA_MAX_CALL_FRAMES) {
                    set_error(error, "stack overflow");
                    return eka_nil();
                }

                /* Save dest register in current frame before pushing */
                frame->dest_reg = a;

                /* Push new frame */
                eka_call_frame_t *new_frame = &vm->frames[vm->frame_count++];
                new_frame->closure   = cl;
                new_frame->ip        = cl->func->code;
                new_frame->registers = arena_alloc(sizeof(eka_value_t) * EKA_MAX_REGISTERS);
                new_frame->stack_top = new_frame->registers;
                new_frame->dest_reg  = 0;  /* default, overwritten by nested calls */

                for (int i = 0; i < EKA_MAX_REGISTERS; i++) {
                    new_frame->registers[i] = eka_nil();
                }

                /* Pass arguments: they're in registers b+1 to b+arg_count_call */
                for (int i = 0; i < nargs_to_pass; i++) {
                    new_frame->registers[i] = frame->registers[b + 1 + i];
                }
                for (int i = nargs_to_pass; i < (int)cl->func->max_arity; i++) {
                    new_frame->registers[i] = eka_nil();
                }

                frame = new_frame;  /* switch to the new frame */
            } else if (eka_obj_is_type(callee, OBJ_NATIVE)) {
                eka_native_t *nat = eka_as_native(callee);
                eka_value_t native_args[16];
                int native_argc = arg_count_call < 16 ? arg_count_call : 16;
                for (int i = 0; i < native_argc; i++) {
                    native_args[i] = frame->registers[b + 1 + i];
                }
                frame->registers[a] = nat->fn(vm, nat->ctx, native_argc, native_args);
            } else {
                set_error(error, "cannot call non-function");
                return eka_nil();
            }
            break;
        }

        case OP_RETURN: {
            eka_value_t result = frame->registers[a];
            vm->frame_count--;
            if (vm->frame_count == 0) {
                return result;
            }
            /* Return to caller — write result to the CALL instruction's dest register */
            frame = &vm->frames[vm->frame_count - 1];
            frame->registers[frame->dest_reg] = result;
            break;
        }

        case OP_CLOSURE: {
            if (b < func->constants_count &&
                eka_obj_is_type(func->constants[b], OBJ_FUNC)) {
                eka_func_t *inner = eka_as_func(func->constants[b]);
                eka_closure_t *cl = eka_closure_new(inner);
                frame->registers[a] = eka_closure_val(cl);
            }
            break;
        }

        /* --- Property access --- */

        case OP_GET_PROP: {
            eka_value_t obj = frame->registers[b];
            if (c < func->constants_count &&
                eka_obj_is_type(func->constants[c], OBJ_STRING)) {
                eka_string_t *key = eka_as_string(func->constants[c]);
                if (eka_obj_is_type(obj, OBJ_MAP)) {
                    eka_value_t val = eka_map_get(eka_as_map(obj), key);
                    if (!eka_is_nil(val)) {
                        frame->registers[a] = val;
                    } else {
                        /* Not in map data — try map methods */
                        frame->registers[a] = map_method_dispatch(eka_as_map(obj), key);
                    }
                } else if (eka_obj_is_type(obj, OBJ_LIST)) {
                    /* List property/method dispatch */
                    frame->registers[a] = list_method_dispatch(eka_as_list(obj), key);
                } else if (eka_obj_is_type(obj, OBJ_STRING)) {
                    /* String .length property */
                    if (key->length == 6 && memcmp(key->data, "length", 6) == 0) {
                        frame->registers[a] = eka_int((int64_t)eka_as_string(obj)->length);
                    } else {
                        frame->registers[a] = eka_nil();
                    }
                } else {
                    /* Property access on nil/bool/number → null (fault-tolerant) */
                    frame->registers[a] = eka_nil();
                }
            }
            break;
        }

        case OP_SET_PROP: {
            eka_value_t obj = frame->registers[a];
            if (c < func->constants_count &&
                eka_obj_is_type(func->constants[c], OBJ_STRING)) {
                eka_string_t *key = eka_as_string(func->constants[c]);
                if (eka_obj_is_type(obj, OBJ_MAP)) {
                    eka_map_set(eka_as_map(obj), key, frame->registers[b]);
                }
            }
            break;
        }

        case OP_GET_INDEX: {
            eka_value_t obj = frame->registers[b];
            eka_value_t idx = frame->registers[c];
            if (eka_obj_is_type(obj, OBJ_LIST) && eka_is_int(idx)) {
                int64_t i = eka_as_int(idx);
                eka_list_t *list = eka_as_list(obj);
                if (i < 0) i += list->length;  /* negative indexing */
                if (i >= 0 && (uint32_t)i < list->length) {
                    frame->registers[a] = list->items[i];
                } else {
                    frame->registers[a] = eka_nil();
                }
            } else if (eka_obj_is_type(obj, OBJ_MAP) && eka_obj_is_type(idx, OBJ_STRING)) {
                frame->registers[a] = eka_map_get(eka_as_map(obj), eka_as_string(idx));
            } else if (eka_obj_is_type(obj, OBJ_STRING) && eka_is_int(idx)) {
                /* String indexing: "hello"[1] → "e" */
                eka_string_t *s = eka_as_string(obj);
                int64_t i = eka_as_int(idx);
                if (i < 0) i += s->length;
                if (i >= 0 && (uint32_t)i < s->length) {
                    frame->registers[a] = eka_string_val(
                        eka_string_new(&s->data[i], 1));
                } else {
                    frame->registers[a] = eka_nil();
                }
            } else {
                frame->registers[a] = eka_nil();
            }
            break;
        }

        case OP_SET_INDEX: {
            /* R(A)[R(B)] = R(C) */
            eka_value_t obj = frame->registers[a];
            eka_value_t idx = frame->registers[b];
            eka_value_t val = frame->registers[c];
            if (eka_obj_is_type(obj, OBJ_LIST) && eka_is_int(idx)) {
                int64_t i = eka_as_int(idx);
                eka_list_t *list = eka_as_list(obj);
                if (i < 0) i += list->length;
                if (i >= 0 && (uint32_t)i < list->length) {
                    list->items[i] = val;
                } else if (i >= 0 && (uint32_t)i == list->length) {
                    /* Setting at length → append (for list literal construction) */
                    eka_list_push(list, val);
                }
            } else if (eka_obj_is_type(obj, OBJ_MAP) && eka_obj_is_type(idx, OBJ_STRING)) {
                eka_map_set(eka_as_map(obj), eka_as_string(idx), val);
            }
            break;
        }

        /* --- Containers --- */

        case OP_NEW_LIST:
            frame->registers[a] = eka_list_val(eka_list_new(b > 0 ? b : 4));
            break;

        case OP_NEW_MAP:
            frame->registers[a] = eka_map_val(eka_map_new(b > 0 ? b : 8));
            break;

        /* --- Upvalues --- */

        case OP_GET_UPVAL:
            if (b < closure->upvalue_count && closure->upvalues[b]) {
                frame->registers[a] = *closure->upvalues[b]->location;
            }
            break;

        case OP_SET_UPVAL:
            if (a < closure->upvalue_count && closure->upvalues[a]) {
                *closure->upvalues[a]->location = frame->registers[b];
            }
            break;

        case OP_CLOSE_UPVAL:
            if (a < closure->upvalue_count && closure->upvalues[a]) {
                eka_upvalue_t *uv = closure->upvalues[a];
                uv->closed = *uv->location;
                uv->location = &uv->closed;
            }
            break;

        /* --- Globals --- */

        case OP_GET_GLOBAL: {
            /* R(A) = globals[constant[B]] */
            if (b < func->constants_count &&
                eka_obj_is_type(func->constants[b], OBJ_STRING)) {
                eka_string_t *key = eka_as_string(func->constants[b]);
                frame->registers[a] = eka_map_get(vm->globals, key);
            } else {
                frame->registers[a] = eka_nil();
            }
            break;
        }

        case OP_SET_GLOBAL: {
            /* globals[constant[A]] = R(B) */
            if (a < func->constants_count &&
                eka_obj_is_type(func->constants[a], OBJ_STRING)) {
                eka_string_t *key = eka_as_string(func->constants[a]);
                eka_map_set(vm->globals, key, frame->registers[b]);
            }
            break;
        }

        case OP_HTML_ESCAPE: {
            /* R(A) = html_escape(R(A)); RawString passes through unchanged */
            frame->registers[a] = eka_html_escape_value(frame->registers[a]);
            break;
        }

        default:
            set_error(error, "unknown opcode");
            return eka_nil();
        }
    }
}

/* Public entry point — sets GC context, delegates to inner, restores context. */
eka_value_t eka_vm_execute(eka_vm_t *vm, eka_closure_t *closure,
                           eka_value_t *args, int arg_count,
                           const char **error) {
    eka_vm_t *prev_vm = eka_gc_current_vm;
    eka_gc_current_vm = vm;
    eka_value_t result = eka_vm_execute_inner(vm, closure, args, arg_count, error);
    eka_gc_current_vm = prev_vm;
    return result;
}

eka_value_t eka_vm_execute_init(eka_vm_t *vm, eka_closure_t *closure,
                                const char **error) {
    return eka_vm_execute(vm, closure, NULL, 0, error);
}
