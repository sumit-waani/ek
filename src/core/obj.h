#ifndef OBJ_H
#define OBJ_H

#include "core/value.h"

#include <stddef.h>
#include <stdint.h>

/* ================================================================
 * Concrete object types
 * ================================================================ */

/* --- String --- */
typedef struct {
    eka_obj_t   header;
    uint32_t    length;     /* byte length (not including null terminator) */
    uint32_t    hash;       /* FNV-1a hash, 0 = not computed yet */
    char        data[];     /* null-terminated UTF-8 */
} eka_string_t;

/* --- List --- */
typedef struct {
    eka_obj_t   header;
    uint32_t    capacity;
    uint32_t    length;
    eka_value_t items[];    /* flexible array */
} eka_list_t;

/* --- Map entry --- */
typedef struct {
    eka_string_t *key;
    eka_value_t   value;
} eka_map_entry_t;

/* --- Map --- */
typedef struct {
    eka_obj_t       header;
    uint32_t        capacity;   /* size of entries array */
    uint32_t        length;     /* number of entries */
    eka_map_entry_t entries[];  /* linear-probed open addressing */
} eka_map_t;

/* --- Function (bytecode) --- */
typedef struct {
    eka_obj_t   header;
    uint32_t    arity;        /* required parameters */
    uint32_t    max_arity;    /* max parameters (incl. defaults) */
    uint32_t    locals_count; /* number of local variables */
    uint32_t    code_length;  /* number of instructions */
    uint32_t    constants_count;
    eka_value_t *constants;   /* constant pool (pointed, not embedded) */
    uint32_t    *code;        /* bytecode instructions (pointed, not embedded) */
    uint32_t    source_line;  /* line number in source (for debugging) */
} eka_func_t;

/* Forward declaration */
typedef struct eka_vm_t eka_vm_t;

/* --- Native function (C builtin) --- */
typedef eka_value_t (*eka_native_fn_t)(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args);

typedef struct {
    eka_obj_t       header;
    eka_native_fn_t fn;
    void           *ctx;
    const char     *name;
} eka_native_t;

/* --- Upvalue (captured variable) --- */
typedef struct {
    eka_obj_t   header;
    eka_value_t *location;   /* points to stack slot or enclosing upvalue */
    eka_value_t  closed;     /* storage for closed-over value */
    struct eka_upvalue_t *next; /* linked list for GC */
} eka_upvalue_t;

/* --- Closure --- */
typedef struct {
    eka_obj_t      header;
    eka_func_t    *func;
    eka_upvalue_t **upvalues; /* array of upvalue pointers */
    uint32_t        upvalue_count;
} eka_closure_t;

/* --- RawString (unescaped HTML, created by html.raw()) --- */
typedef struct {
    eka_obj_t   header;
    uint32_t    length;
    char        data[];
} eka_rawstring_t;

/* ================================================================
 * Object allocation
 * ================================================================ */

/* Low-level arena allocation (used by VM for register arrays, etc.) */
void  *arena_alloc(size_t size);

/* Allocate + initialise an object of given type and variable size.
 * The GC manages these — do NOT free() manually. */
void  *eka_obj_alloc(eka_objtype_t type, size_t extra_bytes);

/* Allocate a string. Makes an owned copy of src. */
eka_string_t *eka_string_new(const char *src, size_t length);

/* Intern a string (returns existing or creates new). */
eka_string_t *eka_string_intern(const char *src, size_t length);

/* Take ownership of an existing malloc'd C string to create an Eka string. */
eka_string_t *eka_string_take(char *data, size_t length);

/* Compute FNV-1a hash lazily. */
uint32_t eka_string_hash(eka_string_t *s);

/* --- List --- */
eka_list_t *eka_list_new(uint32_t initial_capacity);
void        eka_list_push(eka_list_t *list, eka_value_t value);
eka_value_t eka_list_pop(eka_list_t *list);
void        eka_list_insert(eka_list_t *list, uint32_t idx, eka_value_t value);
void        eka_list_remove_at(eka_list_t *list, uint32_t idx);

/* --- Map --- */
eka_map_t  *eka_map_new(uint32_t initial_capacity);
void        eka_map_set(eka_map_t *map, eka_string_t *key, eka_value_t value);
eka_value_t eka_map_get(eka_map_t *map, eka_string_t *key);
bool        eka_map_has(eka_map_t *map, eka_string_t *key);
void        eka_map_delete(eka_map_t *map, eka_string_t *key);
bool        eka_map_entry_is_tombstone(eka_string_t *key);

/* --- Function --- */
eka_func_t *eka_func_new(uint32_t arity, uint32_t max_arity,
                         uint32_t code_length, uint32_t constants_count,
                         uint32_t source_line);

/* --- Conversion --- */
eka_string_t *eka_value_to_string(eka_value_t v);

/* --- Native --- */
eka_native_t *eka_native_new(eka_native_fn_t fn, void *ctx, const char *name);

/* --- Closure --- */
eka_closure_t *eka_closure_new(eka_func_t *func);

/* --- Upvalue --- */
eka_upvalue_t *eka_upvalue_new(eka_value_t *location);

/* --- RawString --- */
eka_rawstring_t *eka_rawstring_new(const char *src, size_t length);

/* ================================================================
 * Convenience: wrap objects as eka_value_t
 * ================================================================ */

static inline eka_value_t eka_string_val(eka_string_t *s)       { return eka_obj(&s->header); }
static inline eka_value_t eka_list_val(eka_list_t *l)           { return eka_obj(&l->header); }
static inline eka_value_t eka_map_val(eka_map_t *m)             { return eka_obj(&m->header); }
static inline eka_value_t eka_func_val(eka_func_t *f)           { return eka_obj(&f->header); }
static inline eka_value_t eka_native_val(eka_native_t *n)       { return eka_obj(&n->header); }
static inline eka_value_t eka_closure_val(eka_closure_t *c)     { return eka_obj(&c->header); }
static inline eka_value_t eka_rawstring_val(eka_rawstring_t *r) { return eka_obj(&r->header); }

/* Cast helpers */
static inline eka_string_t    *eka_as_string(eka_value_t v)    { return (eka_string_t *)eka_as_obj(v); }
static inline eka_list_t      *eka_as_list(eka_value_t v)      { return (eka_list_t *)eka_as_obj(v); }
static inline eka_map_t       *eka_as_map(eka_value_t v)       { return (eka_map_t *)eka_as_obj(v); }
static inline eka_func_t      *eka_as_func(eka_value_t v)      { return (eka_func_t *)eka_as_obj(v); }
static inline eka_native_t    *eka_as_native(eka_value_t v)    { return (eka_native_t *)eka_as_obj(v); }
static inline eka_closure_t   *eka_as_closure(eka_value_t v)   { return (eka_closure_t *)eka_as_obj(v); }
static inline eka_rawstring_t *eka_as_rawstring(eka_value_t v) { return (eka_rawstring_t *)eka_as_obj(v); }

#endif /* OBJ_H */
