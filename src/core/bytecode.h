#ifndef BYTECODE_H
#define BYTECODE_H

#include <stdint.h>

/*
 * Eka bytecode: 32-bit register-based instructions.
 *
 * Layout:  opcode (8) | A (8) | B (8) | C (8)
 *
 * A, B, C are typically register indices (0-255).
 * Some instructions repurpose B or C as immediate values.
 */

typedef uint32_t eka_instr_t;

/* Opcodes */
typedef enum {
    /* --- Constants / Move --- */
    OP_LOAD_CONST,    /* R(A) = constant[B] */
    OP_LOAD_NIL,      /* R(A) = nil */
    OP_LOAD_BOOL,     /* R(A) = (B != 0) */
    OP_LOAD_INT,      /* R(A) = sign-extend(B << 8 | C) as small int */
    OP_MOVE,          /* R(A) = R(B) */

    /* --- Arithmetic --- */
    OP_ADD,           /* R(A) = R(B) + R(C) */
    OP_SUB,           /* R(A) = R(B) - R(C) */
    OP_MUL,           /* R(A) = R(B) * R(C) */
    OP_DIV,           /* R(A) = R(B) / R(C) */
    OP_MOD,           /* R(A) = R(B) % R(C) */
    OP_NEG,           /* R(A) = -R(B) */

    /* --- Comparison --- */
    OP_EQ,            /* R(A) = (R(B) == R(C)) */
    OP_LT,            /* R(A) = (R(B) < R(C)) */
    OP_LE,            /* R(A) = (R(B) <= R(C)) */

    /* --- Logic --- */
    OP_NOT,           /* R(A) = !is_truthy(R(B)) */
    OP_AND,           /* R(A) = is_truthy(R(B)) ? R(C) : R(B) */
    OP_OR,            /* R(A) = is_truthy(R(B)) ? R(B) : R(C) */

    /* --- Control flow --- */
    OP_JUMP,          /* ip += B<<8 | C (signed offset) */
    OP_JUMP_IF_FALSE, /* if !is_truthy(R(A)): ip += B<<8|C */
    OP_JUMP_IF_TRUE,  /* if is_truthy(R(A)): ip += B<<8|C */

    /* --- Functions --- */
    OP_CALL,          /* R(A) = call R(B) with C args starting at R(B+1) */
    OP_RETURN,        /* return R(A) */
    OP_CLOSURE,       /* R(A) = new closure from func constant[B] */

    /* --- Property access --- */
    OP_GET_PROP,      /* R(A) = R(B)[string-constant[C]] */
    OP_SET_PROP,      /* R(A)[string-constant[C]] = R(B) */
    OP_GET_INDEX,     /* R(A) = R(B)[R(C)] */
    OP_SET_INDEX,     /* R(A)[R(B)] = R(C) */

    /* --- Containers --- */
    OP_NEW_LIST,      /* R(A) = new list (capacity = B) */
    OP_NEW_MAP,       /* R(A) = new map (capacity = B) */

    /* --- Upvalues --- */
    OP_GET_UPVAL,     /* R(A) = upvalue[B] */
    OP_SET_UPVAL,     /* upvalue[A] = R(B) */
    OP_CLOSE_UPVAL,   /* close upvalue[A] */

    /* --- Globals --- */
    OP_GET_GLOBAL,    /* R(A) = globals[constant[B]] */
    OP_SET_GLOBAL,    /* globals[constant[A]] = R(B) */

    /* --- Template --- */
    OP_HTML_ESCAPE,   /* R(A) = html_escape(R(A)); RawString passes through */

    OP_COUNT
} eka_opcode_t;

/* Instruction encoding helpers */
static inline eka_instr_t eka_instr_encode(eka_opcode_t op, uint8_t a, uint8_t b, uint8_t c) {
    return ((uint32_t)op << 24) | ((uint32_t)a << 16) | ((uint32_t)b << 8) | (uint32_t)c;
}

static inline eka_opcode_t eka_instr_opcode(eka_instr_t instr) {
    return (eka_opcode_t)(instr >> 24);
}

static inline uint8_t eka_instr_a(eka_instr_t instr) {
    return (uint8_t)((instr >> 16) & 0xFF);
}

static inline uint8_t eka_instr_b(eka_instr_t instr) {
    return (uint8_t)((instr >> 8) & 0xFF);
}

static inline uint8_t eka_instr_c(eka_instr_t instr) {
    return (uint8_t)(instr & 0xFF);
}

/* Signed offset from B|C (for jump instructions) */
static inline int16_t eka_instr_offset(eka_instr_t instr) {
    return (int16_t)(instr & 0xFFFF);
}

/* Small int from B|C (for LOAD_INT) */
static inline int16_t eka_instr_small_int(eka_instr_t instr) {
    return (int16_t)(instr & 0xFFFF);
}

#endif /* BYTECODE_H */
