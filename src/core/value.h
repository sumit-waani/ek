#ifndef VALUE_H
#define VALUE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Eka value representation: NaN-boxed 64-bit.
 *
 * All Eka tagged values use a custom quiet NaN with bit 48 set.
 * This cleanly separates Eka values from actual IEEE 754 NaN/Infinity.
 *
 *   bits 63-48 = 0x7FF9  → Eka tagged value
 *   bits 63-48 ≠ 0x7FF9  → real IEEE 754 double (or untagged NaN)
 *
 * Within the NaN payload (bits 0-47):
 *
 *   bit 0 = 0 → heap pointer (malloc returns ≥8-byte aligned)
 *               nil/false/true are also in this space (special sentinels)
 *   bit 0 = 1, bit 1 = 1 → 46-bit signed integer
 *
 *   nil:   0x7FF9_0000_0000_0000  (payload = 0)
 *   false: 0x7FF9_0000_0000_0002  (payload = 2)
 *   true:  0x7FF9_0000_0000_0004  (payload = 4)
 *
 * Pointers on x86-64 Linux are 48-bit canonical userspace addresses
 * (top 16 bits = 0x0000), so OR with 0x7FF9_0000_0000_0000 is safe.
 * Extraction: mask lower 48 bits to recover the pointer.
 */

typedef uint64_t eka_value_t;

/* --- Tag constants --- */
#define EKA_TAG      0x7FF9000000000000ULL

#define EKA_NIL_V    (EKA_TAG | 0x0000000000000000ULL)
#define EKA_FALSE_V  (EKA_TAG | 0x0000000000000002ULL)
#define EKA_TRUE_V   (EKA_TAG | 0x0000000000000004ULL)

/* --- Heap object types --- */
typedef enum {
    OBJ_STRING,
    OBJ_LIST,
    OBJ_MAP,
    OBJ_FUNC,       /* bytecode function */
    OBJ_NATIVE,     /* C function (builtin) */
    OBJ_UPVALUE,    /* closure captured variable */
    OBJ_CLOSURE,    /* function + upvalues */
    OBJ_RAWSTRING,  /* unescaped HTML */
} eka_objtype_t;

typedef struct eka_obj_t {
    eka_objtype_t type;
    bool          marked;   /* GC mark bit */
    struct eka_obj_t *next; /* intrusive list for GC */
} eka_obj_t;

/* ================================================================
 * Constructors
 * ================================================================ */

static inline eka_value_t eka_nil(void)   { return EKA_NIL_V; }
static inline eka_value_t eka_bool(bool b) { return b ? EKA_TRUE_V : EKA_FALSE_V; }

static inline eka_value_t eka_number(double d) {
    union { double d; eka_value_t v; } u;
    u.d = d;
    return u.v;
}

/* 46-bit signed integer. Bits 0-1 = tag (11), bits 2-47 = value. */
static inline eka_value_t eka_int(int64_t i) {
    return EKA_TAG | (((uint64_t)i << 2) & 0x0000FFFFFFFFFFFFULL) | 3;
}

static inline eka_value_t eka_obj(eka_obj_t *obj) {
    return EKA_TAG | ((uint64_t)(uintptr_t)obj & 0x0000FFFFFFFFFFFFULL);
}

/* ================================================================
 * Type predicates
 * ================================================================ */

static inline bool eka_is_nil(eka_value_t v)    { return v == EKA_NIL_V; }
static inline bool eka_is_bool(eka_value_t v)   { return v == EKA_TRUE_V || v == EKA_FALSE_V; }
static inline bool eka_is_true(eka_value_t v)   { return v == EKA_TRUE_V; }
static inline bool eka_is_false(eka_value_t v)  { return v == EKA_FALSE_V; }

static inline bool eka_is_obj(eka_value_t v) {
    /* Tagged, bit 0 = 0, not nil, not false, not true. */
    return ((v >> 48) == 0x7FF9) && ((v & 1) == 0)
           && (v != EKA_NIL_V)
           && (v != EKA_FALSE_V)
           && (v != EKA_TRUE_V);
}

static inline bool eka_is_int(eka_value_t v) {
    /* Tagged and bits 0-1 = 11. */
    return ((v >> 48) == 0x7FF9) && ((v & 3) == 3);
}

static inline bool eka_is_number(eka_value_t v) {
    /* NOT an Eka tagged value → it's a real IEEE 754 double. */
    return (v >> 48) != 0x7FF9;
}

/* ================================================================
 * Extractors
 * ================================================================ */

static inline bool eka_as_bool(eka_value_t v) {
    return v == EKA_TRUE_V;
}

static inline double eka_as_number(eka_value_t v) {
    union { eka_value_t v; double d; } u;
    u.v = v;
    return u.d;
}

static inline int64_t eka_as_int(eka_value_t v) {
    /* Extract lower 48 bits, shift off tag bits 0-1, sign-extend from bit 45. */
    uint64_t raw = (v & 0x0000FFFFFFFFFFFFULL) >> 2;
    if (raw & (1ULL << 45)) {
        raw |= 0xFFFFC00000000000ULL;  /* set bits 63-46 */
    }
    return (int64_t)raw;
}

static inline eka_obj_t *eka_as_obj(eka_value_t v) {
    return (eka_obj_t *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

/* ================================================================
 * Object type helpers
 * ================================================================ */

static inline bool eka_obj_is_type(eka_value_t v, eka_objtype_t type) {
    return eka_is_obj(v) && eka_as_obj(v)->type == type;
}

/* HTML-escape a value for safe template interpolation.
 * RawString → returned as-is. String → escaped. Other → stringify then escape. */
eka_value_t eka_html_escape_value(eka_value_t v);

#endif /* VALUE_H */
