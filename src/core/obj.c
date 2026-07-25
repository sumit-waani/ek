#include "core/obj.h"
#include "core/vm.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================================================================
 * Bump allocator with 1 MB arenas — per-VM
 * ================================================================ */


/* The currently active VM for GC allocations.
 * Set by eka_vm_init() and before each eka_vm_execute() call. */
eka_vm_t *eka_gc_current_vm = NULL;

/* String interning table — global, shared across all VMs.
 * Interned strings are allocated from a dedicated permanent arena
 * so they survive individual VM teardowns. */
#define INTERN_INITIAL_CAP 256
#define INTERN_MAX_LOAD    70   /* percent */

static eka_string_t **intern_table = NULL;
static uint32_t       intern_capacity = 0;
static uint32_t       intern_count = 0;

/* Permanent arena pool just for interned strings. Never freed. */
static arena_t *string_pool_arenas = NULL;
static arena_t *string_pool_current = NULL;

/* Forward declarations */
static void gc_sweep(eka_vm_t *vm);
static void gc_mark_value(eka_vm_t *vm, eka_value_t v);
static void gc_mark_object(eka_vm_t *vm, eka_obj_t *obj);

/* ================================================================
 * Arena management
 * ================================================================ */

static arena_t *arena_new(void) {
    arena_t *a = malloc(sizeof(arena_t));
    if (!a) {
        fprintf(stderr, "eka: fatal: out of memory allocating arena\n");
        abort();
    }
    a->base = malloc(EKA_ARENA_SIZE);
    if (!a->base) {
        fprintf(stderr, "eka: fatal: out of memory allocating arena data\n");
        abort();
    }
    a->top  = a->base;
    a->end  = a->base + EKA_ARENA_SIZE;
    a->next = NULL;
    return a;
}

/* Allocate from the currently active VM's arenas. */
void *arena_alloc(size_t size) {
    eka_vm_t *vm = eka_gc_current_vm;
    if (!vm) {
        fprintf(stderr, "eka: fatal: arena_alloc called with no current VM\n");
        abort();
    }
    return eka_vm_arena_alloc(vm, size);
}

/* Allocate from a specific VM's arenas. */
void *eka_vm_arena_alloc(eka_vm_t *vm, size_t size) {
    /* Align to 8 bytes */
    size = (size + 7) & ~((size_t)7);

    if (!vm->gc_current || (size_t)(vm->gc_current->end - vm->gc_current->top) < size) {
        arena_t *a = arena_new();
        a->next = vm->gc_arenas;
        vm->gc_arenas = a;
        vm->gc_current = a;
        if (size > EKA_ARENA_SIZE) {
            fprintf(stderr, "eka: fatal: allocation of %zu bytes exceeds arena size\n", size);
            abort();
        }
    }

    void *ptr = vm->gc_current->top;
    vm->gc_current->top += size;
    vm->gc_bytes_allocated += size;
    return ptr;
}

/* Allocate from the permanent string pool (never freed). */
static void *string_pool_alloc(size_t size) {
    size = (size + 7) & ~((size_t)7);

    if (!string_pool_current ||
        (size_t)(string_pool_current->end - string_pool_current->top) < size) {
        arena_t *a = arena_new();
        a->next = string_pool_arenas;
        string_pool_arenas = a;
        string_pool_current = a;
        if (size > EKA_ARENA_SIZE) {
            fprintf(stderr, "eka: fatal: string pool allocation of %zu bytes exceeds arena size\n", size);
            abort();
        }
    }

    void *ptr = string_pool_current->top;
    string_pool_current->top += size;
    return ptr;
}

/* ================================================================
 * Object allocation
 * ================================================================ */

void *eka_obj_alloc(eka_objtype_t type, size_t extra_bytes) {
    eka_vm_t *vm = eka_gc_current_vm;
    if (!vm) {
        fprintf(stderr, "eka: fatal: eka_obj_alloc called with no current VM\n");
        abort();
    }

    /* GC trigger check */
    if (vm->gc_bytes_allocated >= vm->gc_next_gc && !vm->gc_running) {
        gc_sweep(vm);
        vm->gc_next_gc = vm->gc_bytes_allocated + EKA_ARENA_SIZE;
        if (vm->gc_next_gc < EKA_ARENA_SIZE) vm->gc_next_gc = EKA_ARENA_SIZE;
    }

    size_t total = sizeof(eka_obj_t) + extra_bytes;
    eka_obj_t *obj = eka_vm_arena_alloc(vm, total);

    memset(obj, 0, sizeof(eka_obj_t));
    obj->type   = type;
    obj->marked = false;

    /* Thread into the VM's all-objects list */
    obj->next = vm->gc_all_objects;
    vm->gc_all_objects = obj;

    return obj;
}

/* ================================================================
 * String allocation + interning
 * ================================================================ */

/* FNV-1a hash */
static uint32_t fnv1a(const char *data, size_t len) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)data[i];
        hash *= 16777619u;
    }
    return hash;
}

static void intern_table_resize(uint32_t new_cap) {
    eka_string_t **old_table = intern_table;
    uint32_t old_cap = intern_capacity;

    intern_table = calloc(new_cap, sizeof(eka_string_t *));
    if (!intern_table) {
        fprintf(stderr, "eka: fatal: out of memory for intern table\n");
        abort();
    }
    intern_capacity = new_cap;
    intern_count = 0;

    if (old_table) {
        for (uint32_t i = 0; i < old_cap; i++) {
            if (old_table[i]) {
                eka_string_t *s = old_table[i];
                uint32_t idx = s->hash % new_cap;
                while (intern_table[idx]) {
                    idx = (idx + 1) % new_cap;
                }
                intern_table[idx] = s;
                intern_count++;
            }
        }
        free(old_table);
    }
}

eka_string_t *eka_string_new(const char *src, size_t length) {
    return eka_string_intern(src, length);
}

eka_string_t *eka_string_intern(const char *src, size_t length) {
    uint32_t hash = fnv1a(src, length);

    /* Lazy init intern table */
    if (!intern_table) {
        intern_table_resize(INTERN_INITIAL_CAP);
    }

    /* Look up existing */
    uint32_t idx = hash % intern_capacity;
    while (intern_table[idx]) {
        eka_string_t *existing = intern_table[idx];
        if (existing->hash == hash &&
            existing->length == length &&
            memcmp(existing->data, src, length) == 0) {
            return existing;
        }
        idx = (idx + 1) % intern_capacity;
    }

    /* Grow if needed */
    if (intern_count * 100 / intern_capacity >= INTERN_MAX_LOAD) {
        intern_table_resize(intern_capacity * 2);
        idx = hash % intern_capacity;
        while (intern_table[idx]) {
            idx = (idx + 1) % intern_capacity;
        }
    }

    /* Allocate new string object from the permanent string pool.
     * This ensures interned strings survive individual VM teardowns. */
    size_t alloc_size = sizeof(eka_string_t) + length + 1;
    eka_string_t *s = string_pool_alloc(alloc_size);
    memset(s, 0, sizeof(eka_string_t));
    s->header.type   = OBJ_STRING;
    s->header.marked = false;   /* never collected from per-VM GC */
    s->header.next   = NULL;    /* not in any VM's gc_all_objects list */
    s->length = (uint32_t)length;
    s->hash   = hash;
    memcpy(s->data, src, length);
    s->data[length] = '\0';

    intern_table[idx] = s;
    intern_count++;

    return s;
}

eka_string_t *eka_string_take(char *data, size_t length) {
    /* Check intern table first */
    uint32_t hash = fnv1a(data, length);
    if (intern_table) {
        uint32_t idx = hash % intern_capacity;
        while (intern_table[idx]) {
            eka_string_t *existing = intern_table[idx];
            if (existing->hash == hash &&
                existing->length == length &&
                memcmp(existing->data, data, length) == 0) {
                free(data);
                return existing;
            }
            idx = (idx + 1) % intern_capacity;
        }
    }

    if (!intern_table) intern_table_resize(INTERN_INITIAL_CAP);

    /* Grow if needed */
    if (intern_count * 100 / intern_capacity >= INTERN_MAX_LOAD) {
        intern_table_resize(intern_capacity * 2);
    }

    uint32_t idx = hash % intern_capacity;
    while (intern_table[idx]) idx = (idx + 1) % intern_capacity;

    /* Allocate from string pool so interned strings are permanent */
    size_t alloc_size = sizeof(eka_string_t) + length + 1;
    eka_string_t *s = string_pool_alloc(alloc_size);
    memset(s, 0, sizeof(eka_string_t));
    s->header.type   = OBJ_STRING;
    s->header.marked = false;
    s->header.next   = NULL;
    s->length = (uint32_t)length;
    s->hash   = hash;
    memcpy(s->data, data, length);
    s->data[length] = '\0';
    free(data);

    intern_table[idx] = s;
    intern_count++;
    return s;
}

uint32_t eka_string_hash(eka_string_t *s) {
    if (s->hash == 0) {
        s->hash = fnv1a(s->data, s->length);
    }
    return s->hash;
}

/* ================================================================
 * List operations
 * ================================================================ */

eka_list_t *eka_list_new(uint32_t initial_capacity) {
    if (initial_capacity < 4) initial_capacity = 4;
    /* Allocate only the struct from the arena — no flex array.
     * The data array is heap-allocated so realloc can grow it. */
    eka_list_t *list = eka_obj_alloc(OBJ_LIST, sizeof(eka_list_t) - sizeof(eka_obj_t));
    list->capacity = initial_capacity;
    list->length = 0;
    list->items = malloc(sizeof(eka_value_t) * initial_capacity);
    if (!list->items) {
        fprintf(stderr, "eka: fatal: out of memory allocating list data\n");
        abort();
    }
    return list;
}

static void list_grow(eka_list_t *list) {
    uint32_t new_cap = list->capacity * 2;
    eka_value_t *new_items = realloc(list->items, sizeof(eka_value_t) * new_cap);
    if (!new_items) {
        fprintf(stderr, "eka: fatal: out of memory growing list (capacity %u -> %u)\n",
                list->capacity, new_cap);
        abort();
    }
    list->items = new_items;
    list->capacity = new_cap;
}

void eka_list_push(eka_list_t *list, eka_value_t value) {
    if (list->length >= list->capacity) {
        list_grow(list);
    }
    list->items[list->length++] = value;
}

eka_value_t eka_list_pop(eka_list_t *list) {
    if (list->length == 0) return eka_nil();
    return list->items[--list->length];
}

void eka_list_insert(eka_list_t *list, uint32_t idx, eka_value_t value) {
    if (idx > list->length) idx = list->length;
    if (list->length >= list->capacity) list_grow(list);
    memmove(&list->items[idx + 1], &list->items[idx],
            sizeof(eka_value_t) * (list->length - idx));
    list->items[idx] = value;
    list->length++;
}

void eka_list_remove_at(eka_list_t *list, uint32_t idx) {
    if (idx >= list->length) return;
    memmove(&list->items[idx], &list->items[idx + 1],
            sizeof(eka_value_t) * (list->length - idx - 1));
    list->length--;
}

/* ================================================================
 * Map operations (linear-probed open addressing)
 * ================================================================ */

eka_map_t *eka_map_new(uint32_t initial_capacity) {
    if (initial_capacity < 8) initial_capacity = 8;
    /* Allocate only the struct from the arena — no flex array.
     * The entries array is heap-allocated so realloc can grow it. */
    eka_map_t *map = eka_obj_alloc(OBJ_MAP, sizeof(eka_map_t) - sizeof(eka_obj_t));
    map->capacity = initial_capacity;
    map->length = 0;
    map->entries = calloc(initial_capacity, sizeof(eka_map_entry_t));
    if (!map->entries) {
        fprintf(stderr, "eka: fatal: out of memory allocating map data\n");
        abort();
    }
    return map;
}

/* Sentinel for "this slot was deleted" (tombstone) */
static eka_string_t tombstone_storage;
static eka_string_t *const TOMBSTONE = &tombstone_storage;
static bool tombstone_inited = false;

static void map_grow(eka_map_t *map);

void eka_map_set(eka_map_t *map, eka_string_t *key, eka_value_t value) {
    if (!tombstone_inited) {
        tombstone_inited = true;
        /* TOMBSTONE is never dereferenced as a real string, just compared by pointer */
    }

    if (map->length * 100 / map->capacity >= 70) {
        map_grow(map);
    }

    uint32_t idx = key->hash % map->capacity;
    uint32_t first_tombstone = UINT32_MAX;

    while (true) {
        eka_map_entry_t *entry = &map->entries[idx];
        if (entry->key == NULL) {
            /* Empty slot */
            if (first_tombstone != UINT32_MAX) idx = first_tombstone;
            map->entries[idx].key = key;
            map->entries[idx].value = value;
            map->length++;
            return;
        }
        if (entry->key == TOMBSTONE) {
            if (first_tombstone == UINT32_MAX) first_tombstone = idx;
        } else if (entry->key == key) {
            /* Same string pointer (interning guarantees this) */
            entry->value = value;
            return;
        }
        idx = (idx + 1) % map->capacity;
    }
}

eka_value_t eka_map_get(eka_map_t *map, eka_string_t *key) {
    uint32_t idx = key->hash % map->capacity;
    for (uint32_t i = 0; i < map->capacity; i++) {
        eka_map_entry_t *entry = &map->entries[idx];
        if (entry->key == NULL) return eka_nil();
        if (entry->key == key) return entry->value;
        idx = (idx + 1) % map->capacity;
    }
    return eka_nil();
}

bool eka_map_has(eka_map_t *map, eka_string_t *key) {
    uint32_t idx = key->hash % map->capacity;
    for (uint32_t i = 0; i < map->capacity; i++) {
        eka_map_entry_t *entry = &map->entries[idx];
        if (entry->key == NULL) return false;
        if (entry->key == key) return true;
        idx = (idx + 1) % map->capacity;
    }
    return false;
}

void eka_map_delete(eka_map_t *map, eka_string_t *key) {
    uint32_t idx = key->hash % map->capacity;
    for (uint32_t i = 0; i < map->capacity; i++) {
        eka_map_entry_t *entry = &map->entries[idx];
        if (entry->key == NULL) return;
        if (entry->key == key) {
            entry->key = TOMBSTONE;
            entry->value = eka_nil();
            map->length--;
            return;
        }
        idx = (idx + 1) % map->capacity;
    }
}

static void map_grow(eka_map_t *map) {
    uint32_t old_cap = map->capacity;
    eka_map_entry_t *old_entries = map->entries;

    uint32_t new_cap = old_cap * 2;
    eka_map_entry_t *new_entries = calloc(new_cap, sizeof(eka_map_entry_t));
    if (!new_entries) {
        fprintf(stderr, "eka: fatal: out of memory growing map (capacity %u -> %u)\n",
                old_cap, new_cap);
        abort();
    }

    /* Rehash all live entries into the new array.
     * We also need to reset the length counter since eka_map_set increments it. */
    map->entries = new_entries;
    map->capacity = new_cap;
    uint32_t live_count = 0;

    for (uint32_t i = 0; i < old_cap; i++) {
        if (old_entries[i].key && old_entries[i].key != TOMBSTONE) {
            /* Insert directly into new array (linear probe, no tombstones in fresh array) */
            uint32_t idx = old_entries[i].key->hash % new_cap;
            while (new_entries[idx].key != NULL) {
                idx = (idx + 1) % new_cap;
            }
            new_entries[idx] = old_entries[i];
            live_count++;
        }
    }

    map->length = live_count;
    free(old_entries);
}

bool eka_map_entry_is_tombstone(eka_string_t *key) {
    return key == TOMBSTONE;
}

/* ================================================================
 * Function, Native, Closure, Upvalue
 * ================================================================ */

eka_func_t *eka_func_new(uint32_t arity, uint32_t max_arity,
                         uint32_t code_length, uint32_t constants_count,
                         uint32_t source_line) {
    eka_func_t *f = eka_obj_alloc(OBJ_FUNC, sizeof(eka_func_t) - sizeof(eka_obj_t));
    f->arity           = arity;
    f->max_arity       = max_arity;
    f->locals_count    = 0;
    f->code_length     = code_length;
    f->constants_count = constants_count;
    f->source_line     = source_line;

    /* Allocate code and constants from arena too */
    if (code_length > 0) {
        f->code = arena_alloc(sizeof(uint32_t) * code_length);
        memset(f->code, 0, sizeof(uint32_t) * code_length);
    } else {
        f->code = NULL;
    }

    if (constants_count > 0) {
        f->constants = arena_alloc(sizeof(eka_value_t) * constants_count);
        for (uint32_t i = 0; i < constants_count; i++) {
            f->constants[i] = eka_nil();
        }
    } else {
        f->constants = NULL;
    }

    return f;
}

eka_native_t *eka_native_new(eka_native_fn_t fn, void *ctx, const char *name) {
    eka_native_t *n = eka_obj_alloc(OBJ_NATIVE, sizeof(eka_native_t) - sizeof(eka_obj_t));
    n->fn   = fn;
    n->ctx  = ctx;
    n->name = name;
    return n;
}

/* --- Value to string conversion --- */

eka_string_t *eka_value_to_string(eka_value_t v) {
    if (eka_is_nil(v)) {
        return eka_string_intern("", 0);
    }
    if (eka_is_bool(v)) {
        return eka_string_intern(eka_as_bool(v) ? "true" : "false",
                                 eka_as_bool(v) ? 4 : 5);
    }
    if (eka_is_number(v)) {
        char buf[64];
        int len = snprintf(buf, sizeof(buf), "%g", eka_as_number(v));
        return eka_string_intern(buf, (size_t)len);
    }
    if (eka_is_int(v)) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "%lld", (long long)eka_as_int(v));
        return eka_string_intern(buf, (size_t)len);
    }
    if (eka_obj_is_type(v, OBJ_STRING)) {
        return eka_as_string(v);
    }
    /* Fallback: empty string */
    return eka_string_intern("", 0);
}

eka_closure_t *eka_closure_new(eka_func_t *func) {
    size_t upval_size = sizeof(eka_upvalue_t *) * func->locals_count;
    eka_closure_t *c = eka_obj_alloc(OBJ_CLOSURE,
        sizeof(eka_closure_t) - sizeof(eka_obj_t) + upval_size);
    c->func          = func;
    c->upvalue_count = 0;
    c->upvalues       = (eka_upvalue_t **)((char *)c + sizeof(eka_closure_t));
    /* zero-init upvalues */
    memset(c->upvalues, 0, func->locals_count * sizeof(eka_upvalue_t *));
    return c;
}

eka_upvalue_t *eka_upvalue_new(eka_value_t *location) {
    eka_upvalue_t *uv = eka_obj_alloc(OBJ_UPVALUE,
        sizeof(eka_upvalue_t) - sizeof(eka_obj_t));
    uv->location = location;
    uv->closed   = eka_nil();
    uv->next     = NULL;
    return uv;
}

eka_rawstring_t *eka_rawstring_new(const char *src, size_t length) {
    size_t extra = sizeof(eka_rawstring_t) - sizeof(eka_obj_t) + length + 1;
    eka_rawstring_t *r = eka_obj_alloc(OBJ_RAWSTRING, extra);
    r->length = (uint32_t)length;
    memcpy(r->data, src, length);
    r->data[length] = '\0';
    return r;
}

/* ================================================================
 * Mark-sweep GC — per-VM
 * ================================================================ */

void eka_gc_add_root(eka_value_t v) {
    eka_vm_t *vm = eka_gc_current_vm;
    if (!vm) return;
    if (vm->gc_root_count < EKA_GC_MAX_ROOTS) {
        vm->gc_roots[vm->gc_root_count++] = v;
    }
}

void eka_gc_clear_roots(void) {
    eka_vm_t *vm = eka_gc_current_vm;
    if (!vm) return;
    vm->gc_root_count = 0;
}

static void gc_mark_value(eka_vm_t *vm, eka_value_t v) {
    if (eka_is_obj(v)) {
        gc_mark_object(vm, eka_as_obj(v));
    }
}

static void gc_mark_object(eka_vm_t *vm, eka_obj_t *obj) {
    (void)vm;  /* vm may be used for future per-VM tracking */
    if (!obj || obj->marked) return;
    obj->marked = true;

    switch (obj->type) {
    case OBJ_STRING:
        /* Strings are interned in the global string pool — no outgoing refs to mark.
         * Note: strings allocated in per-VM arenas (if any) are also marked here,
         * but since they're always interned, they're in the string pool. */
        break;

    case OBJ_LIST: {
        eka_list_t *list = (eka_list_t *)obj;
        for (uint32_t i = 0; i < list->length; i++) {
            gc_mark_value(vm, list->items[i]);
        }
        break;
    }

    case OBJ_MAP: {
        eka_map_t *map = (eka_map_t *)obj;
        for (uint32_t i = 0; i < map->capacity; i++) {
            if (map->entries[i].key && map->entries[i].key != TOMBSTONE) {
                gc_mark_object(vm, &map->entries[i].key->header);
                gc_mark_value(vm, map->entries[i].value);
            }
        }
        break;
    }

    case OBJ_FUNC: {
        eka_func_t *func = (eka_func_t *)obj;
        for (uint32_t i = 0; i < func->constants_count; i++) {
            gc_mark_value(vm, func->constants[i]);
        }
        break;
    }

    case OBJ_NATIVE:
    case OBJ_RAWSTRING:
        /* No outgoing references */
        break;

    case OBJ_CLOSURE: {
        eka_closure_t *cl = (eka_closure_t *)obj;
        gc_mark_object(vm, &cl->func->header);
        for (uint32_t i = 0; i < cl->upvalue_count; i++) {
            if (cl->upvalues[i]) {
                gc_mark_object(vm, &cl->upvalues[i]->header);
            }
        }
        break;
    }

    case OBJ_UPVALUE: {
        eka_upvalue_t *uv = (eka_upvalue_t *)obj;
        gc_mark_value(vm, uv->closed);
        break;
    }
    }
}

static void gc_sweep(eka_vm_t *vm) {
    vm->gc_running = true;

    /* Mark phase: mark all roots */
    for (int i = 0; i < vm->gc_root_count; i++) {
        gc_mark_value(vm, vm->gc_roots[i]);
    }
    /* Also mark all interned strings (they're always reachable) */
    if (intern_table) {
        for (uint32_t i = 0; i < intern_capacity; i++) {
            if (intern_table[i]) {
                intern_table[i]->header.marked = true;
            }
        }
    }

    /* Sweep phase: in V1, just reset mark bits for next cycle.
     * Actual memory reclamation happens when the VM is freed
     * (all arenas freed at once). */
    for (eka_obj_t *obj = vm->gc_all_objects; obj; obj = obj->next) {
        obj->marked = false;
    }

    vm->gc_running = false;
}

/* ================================================================
 * GC lifecycle
 * ================================================================ */

void eka_gc_collect(void) {
    eka_vm_t *vm = eka_gc_current_vm;
    if (vm && !vm->gc_running) {
        gc_sweep(vm);
    }
}

/* Get GC stats for debugging */
void eka_gc_stats(size_t *allocated, size_t *next_gc) {
    eka_vm_t *vm = eka_gc_current_vm;
    if (allocated) *allocated = vm ? vm->gc_bytes_allocated : 0;
    if (next_gc) *next_gc = vm ? vm->gc_next_gc : 0;
}

/* ================================================================
 * Heap data cleanup for lists/maps
 *
 * List and map data arrays are heap-allocated (malloc/realloc) so they
 * can grow. They must be freed separately from the arena-managed object
 * headers. Call this before freeing arenas in eka_vm_free.
 * ================================================================ */

void eka_obj_free_heap_data(eka_vm_t *vm) {
    for (eka_obj_t *obj = vm->gc_all_objects; obj; obj = obj->next) {
        switch (obj->type) {
        case OBJ_LIST: {
            eka_list_t *list = (eka_list_t *)obj;
            if (list->items) {
                free(list->items);
                list->items = NULL;
            }
            break;
        }
        case OBJ_MAP: {
            eka_map_t *map = (eka_map_t *)obj;
            if (map->entries) {
                free(map->entries);
                map->entries = NULL;
            }
            break;
        }
        default:
            break;
        }
    }
}
