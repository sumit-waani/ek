#include "compiler/compiler.h"
#include "compiler/symtab.h"
#include "core/bytecode.h"
#include "core/obj.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================================================================
 * Compiler state
 * ================================================================ */

#define MAX_CODE      4096
#define MAX_CONSTANTS 256

typedef struct {
    scope_t      *current_scope;
    eka_func_t   *current_func;

    /* Code buffer */
    uint32_t  code[MAX_CODE];
    uint32_t  code_count;

    /* Constants */
    eka_value_t constants[MAX_CONSTANTS];
    uint32_t    constant_count;

    /* Error handling */
    bool        had_error;
    const char *error_msg;

    /* Context */
    bool        is_init;       /* true if compiling init (global) scope */
} compiler_t;

/* ================================================================
 * Forward declarations
 * ================================================================ */

static void compile_stmt(compiler_t *c, ast_node_t *node);
static uint8_t compile_expr(compiler_t *c, ast_node_t *node);
static uint8_t compile_template(compiler_t *c, ast_node_t *node);

/* ================================================================
 * Helpers
 * ================================================================ */

static void emit(compiler_t *c, eka_instr_t instr) {
    if (c->code_count >= MAX_CODE) {
        c->had_error = true;
        c->error_msg = "function too large (max 4096 instructions)";
        return;
    }
    c->code[c->code_count++] = instr;
}

static uint32_t add_constant(compiler_t *c, eka_value_t val) {
    if (c->constant_count >= MAX_CONSTANTS) {
        c->had_error = true;
        c->error_msg = "too many constants";
        return 0;
    }
    /* Deduplicate strings */
    if (eka_is_obj(val) && eka_obj_is_type(val, OBJ_STRING)) {
        eka_string_t *new_s = eka_as_string(val);
        for (uint32_t i = 0; i < c->constant_count; i++) {
            if (eka_obj_is_type(c->constants[i], OBJ_STRING)) {
                eka_string_t *existing = eka_as_string(c->constants[i]);
                if (existing == new_s) return i;  /* interning ensures pointer equality */
            }
        }
    }
    uint32_t idx = c->constant_count++;
    c->constants[idx] = val;
    return idx;
}

static uint32_t add_string_constant(compiler_t *c, const char *s, size_t len) {
    eka_string_t *str = eka_string_intern(s, len);
    return add_constant(c, eka_string_val(str));
}

static uint8_t alloc_reg(compiler_t *c) {
    if (c->current_scope->local_count >= 255) {
        c->had_error = true;
        c->error_msg = "too many local variables";
        return 0;
    }
    return (uint8_t)c->current_scope->local_count++;
}

/* ================================================================
 * Variable resolution
 * ================================================================ */

static uint8_t resolve_var(compiler_t *c, const char *name, size_t len, sym_kind_t *out_kind) {
    sym_t *sym = symtab_resolve(c->current_scope, name, len);
    if (sym) {
        *out_kind = sym->kind;
        if (sym->kind == SYM_LOCAL) {
            return (uint8_t)sym->index;
        } else if (sym->kind == SYM_UPVALUE) {
            c->had_error = true;
            c->error_msg = "upvalues not yet implemented in compiler";
            return 0;
        }
    }
    /* Not found locally — treat as global */
    *out_kind = SYM_GLOBAL;
    return 0;
}

/* ================================================================
 * Expression compilation → returns register holding result
 * ================================================================ */

static uint8_t compile_expr(compiler_t *c, ast_node_t *node) {
    if (!node) return 0;

    switch (node->type) {

    case AST_LITERAL: {
        eka_value_t val;
        switch (node->as.literal.literal_type) {
        case TOKEN_NULL:
            val = eka_nil();
            break;
        case TOKEN_TRUE:
            val = eka_bool(true);
            break;
        case TOKEN_FALSE:
            val = eka_bool(false);
            break;
        case TOKEN_NUMBER: {
            /* Parse the number string */
            char buf[64];
            size_t len = node->token.length < 63 ? node->token.length : 63;
            memcpy(buf, node->token.start, len);
            buf[len] = '\0';
            double d = strtod(buf, NULL);
            val = eka_number(d);
            break;
        }
        case TOKEN_STRING: {
            eka_string_t *s = eka_string_intern(node->token.start, node->token.length);
            val = eka_string_val(s);
            break;
        }
        default:
            val = eka_nil();
            break;
        }
        uint32_t const_idx = add_constant(c, val);
        uint8_t reg = alloc_reg(c);
        emit(c, eka_instr_encode(OP_LOAD_CONST, reg, (uint8_t)const_idx, 0));
        return reg;
    }

    case AST_IDENTIFIER: {
        sym_kind_t kind;
        const char *name = node->as.identifier.name;
        size_t len = node->as.identifier.name_len;
        uint8_t idx = resolve_var(c, name, len, &kind);

        uint8_t reg = alloc_reg(c);

        if (kind == SYM_LOCAL) {
            emit(c, eka_instr_encode(OP_MOVE, reg, idx, 0));
        } else if (kind == SYM_GLOBAL) {
            /* Load global by name via OP_GET_GLOBAL */
            uint32_t name_idx = add_string_constant(c, name, len);
            emit(c, eka_instr_encode(OP_GET_GLOBAL, reg, (uint8_t)name_idx, 0));
        }
        return reg;
    }

    case AST_BINARY: {
        uint8_t lhs = compile_expr(c, node->as.binary.lhs);
        uint8_t rhs = compile_expr(c, node->as.binary.rhs);
        uint8_t reg = alloc_reg(c);

        switch (node->as.binary.op) {
        case TOKEN_PLUS:  emit(c, eka_instr_encode(OP_ADD, reg, lhs, rhs)); break;
        case TOKEN_MINUS: emit(c, eka_instr_encode(OP_SUB, reg, lhs, rhs)); break;
        case TOKEN_STAR:  emit(c, eka_instr_encode(OP_MUL, reg, lhs, rhs)); break;
        case TOKEN_SLASH: emit(c, eka_instr_encode(OP_DIV, reg, lhs, rhs)); break;
        case TOKEN_PERCENT: emit(c, eka_instr_encode(OP_MOD, reg, lhs, rhs)); break;
        case TOKEN_EQ:    emit(c, eka_instr_encode(OP_EQ,  reg, lhs, rhs)); break;
        case TOKEN_NEQ: {
            /* != is NOT EQ */
            uint8_t eq_reg = alloc_reg(c);
            emit(c, eka_instr_encode(OP_EQ, eq_reg, lhs, rhs));
            emit(c, eka_instr_encode(OP_NOT, reg, eq_reg, 0));
            break;
        }
        case TOKEN_LT:    emit(c, eka_instr_encode(OP_LT, reg, lhs, rhs)); break;
        case TOKEN_LTE:   emit(c, eka_instr_encode(OP_LE, reg, lhs, rhs)); break;
        case TOKEN_GT:
            /* a > b  ≡  b < a */
            emit(c, eka_instr_encode(OP_LT, reg, rhs, lhs));
            break;
        case TOKEN_GTE:
            /* a >= b  ≡  b <= a */
            emit(c, eka_instr_encode(OP_LE, reg, rhs, lhs));
            break;
        case TOKEN_AND:
            /* Short-circuit: return lhs if falsy, else rhs */
            emit(c, eka_instr_encode(OP_AND, reg, lhs, rhs));
            break;
        case TOKEN_OR:
            emit(c, eka_instr_encode(OP_OR, reg, lhs, rhs));
            break;
        default:
            emit(c, eka_instr_encode(OP_LOAD_NIL, reg, 0, 0));
            break;
        }
        return reg;
    }

    case AST_UNARY: {
        uint8_t operand = compile_expr(c, node->as.unary.operand);
        uint8_t reg = alloc_reg(c);

        switch (node->as.unary.op) {
        case TOKEN_MINUS:
            emit(c, eka_instr_encode(OP_NEG, reg, operand, 0));
            break;
        case TOKEN_NOT:
            emit(c, eka_instr_encode(OP_NOT, reg, operand, 0));
            break;
        default:
            emit(c, eka_instr_encode(OP_LOAD_NIL, reg, 0, 0));
            break;
        }
        return reg;
    }

    case AST_CALL: {
        uint8_t callee = compile_expr(c, node->as.call.callee);

        /* Compile arguments */
        uint8_t arg_regs[16];
        int arg_count = 0;
        for (ast_node_t *a = node->as.call.args; a && arg_count < 16; a = a->next) {
            arg_regs[arg_count++] = compile_expr(c, a);
        }

        /* Place args in consecutive registers starting after callee */
        /* The CALL instruction expects: R(callee) = func, R(callee+1..callee+N) = args */
        /* For simplicity, move args to adjacent registers */
        for (int i = 0; i < arg_count; i++) {
            emit(c, eka_instr_encode(OP_MOVE, callee + 1 + i, arg_regs[i], 0));
        }

        uint8_t result = alloc_reg(c);
        emit(c, eka_instr_encode(OP_CALL, result, callee, (uint8_t)arg_count));
        return result;
    }

    case AST_PROPERTY: {
        uint8_t obj = compile_expr(c, node->as.property.obj);
        uint32_t key_idx = add_string_constant(c, node->as.property.prop,
                                                node->as.property.prop_len);
        uint8_t reg = alloc_reg(c);
        emit(c, eka_instr_encode(OP_GET_PROP, reg, obj, (uint8_t)key_idx));
        return reg;
    }

    case AST_INDEX: {
        uint8_t obj = compile_expr(c, node->as.index.obj);
        uint8_t idx = compile_expr(c, node->as.index.index);
        uint8_t reg = alloc_reg(c);
        emit(c, eka_instr_encode(OP_GET_INDEX, reg, obj, idx));
        return reg;
    }

    case AST_NULL_SAFE: {
        /* obj?.prop → temp = obj; if temp == nil: nil else temp.prop */
        /* For now, just compile as property access (will error on null at runtime) */
        uint8_t obj = compile_expr(c, node->as.property.obj);
        uint32_t key_idx = add_string_constant(c, node->as.property.prop,
                                                node->as.property.prop_len);
        uint8_t reg = alloc_reg(c);
        emit(c, eka_instr_encode(OP_GET_PROP, reg, obj, (uint8_t)key_idx));
        return reg;
    }

    case AST_NULL_COALESCE: {
        /* lhs ?? rhs: evaluate lhs, if not nil return lhs, else rhs */
        uint8_t lhs = compile_expr(c, node->as.null_coalesce.lhs);
        uint8_t reg = alloc_reg(c);

        /* Copy lhs to reg */
        emit(c, eka_instr_encode(OP_MOVE, reg, lhs, 0));

        /* If reg != nil, jump over the rhs */
        /* For now: just emit the rhs, no short-circuit. */
        uint8_t rhs = compile_expr(c, node->as.null_coalesce.rhs);

        /* emit: move rhs into reg — simplistic, doesn't short-circuit properly */
        emit(c, eka_instr_encode(OP_MOVE, reg, rhs, 0));
        return reg;
    }

    case AST_MAP_LITERAL: {
        uint8_t map_reg = alloc_reg(c);
        emit(c, eka_instr_encode(OP_NEW_MAP, map_reg, 8, 0));

        for (ast_node_t *e = node->as.map_literal.entries; e; e = e->next) {
            /* Entry is AST_BINARY with op=COLON, lhs=key, rhs=value */
            if (e->type == AST_BINARY && e->as.binary.op == TOKEN_COLON) {
                /* Key is always a string constant for now */
                ast_node_t *key_node = e->as.binary.lhs;
                uint32_t key_idx;
                if (key_node->type == AST_IDENTIFIER) {
                    key_idx = add_string_constant(c, key_node->as.identifier.name,
                                                   key_node->as.identifier.name_len);
                } else if (key_node->type == AST_LITERAL &&
                           key_node->as.literal.literal_type == TOKEN_STRING) {
                    key_idx = add_string_constant(c, key_node->token.start,
                                                   key_node->token.length);
                } else {
                    continue;
                }

                uint8_t val = compile_expr(c, e->as.binary.rhs);
                emit(c, eka_instr_encode(OP_SET_PROP, map_reg, val, (uint8_t)key_idx));
            }
        }
        return map_reg;
    }

    case AST_LIST_LITERAL: {
        uint8_t list_reg = alloc_reg(c);
        emit(c, eka_instr_encode(OP_NEW_LIST, list_reg, 8, 0));

        int idx = 0;
        for (ast_node_t *item = node->as.list_literal.items; item; item = item->next) {
            uint8_t val = compile_expr(c, item);
            /* For list items, we need to push. Since we don't have OP_PUSH,
             * we'll use SET_INDEX with the current length index. */
            uint8_t idx_reg = alloc_reg(c);
            uint32_t int_const_idx = add_constant(c, eka_int(idx));
            emit(c, eka_instr_encode(OP_LOAD_CONST, idx_reg, (uint8_t)int_const_idx, 0));
            emit(c, eka_instr_encode(OP_SET_INDEX, list_reg, idx_reg, val));
            idx++;
        }
        return list_reg;
    }

    case AST_ASSIGN: {
        /* target = value */
        uint8_t val = compile_expr(c, node->as.assign.value);

        /* Target can be: IDENTIFIER, PROPERTY, INDEX */
        ast_node_t *target = node->as.assign.target;
        if (target->type == AST_IDENTIFIER) {
            sym_kind_t kind;
            const char *name = target->as.identifier.name;
            size_t len = target->as.identifier.name_len;
            uint8_t idx = resolve_var(c, name, len, &kind);

            if (kind == SYM_LOCAL) {
                emit(c, eka_instr_encode(OP_MOVE, idx, val, 0));
                /* In init scope, also sync to global */
                if (c->is_init) {
                    uint32_t name_idx = add_string_constant(c, name, len);
                    emit(c, eka_instr_encode(OP_SET_GLOBAL, (uint8_t)name_idx, val, 0));
                }
            } else if (kind == SYM_GLOBAL) {
                uint32_t name_idx = add_string_constant(c, name, len);
                emit(c, eka_instr_encode(OP_SET_GLOBAL, (uint8_t)name_idx, val, 0));
            }
        } else if (target->type == AST_PROPERTY) {
            uint8_t obj = compile_expr(c, target->as.property.obj);
            uint32_t key_idx = add_string_constant(c, target->as.property.prop,
                                                    target->as.property.prop_len);
            emit(c, eka_instr_encode(OP_SET_PROP, obj, val, (uint8_t)key_idx));
        }
        return val;
    }

    default:
        c->had_error = true;
        c->error_msg = "unsupported expression type";
        return 0;
    }
}

/* ================================================================
 * Statement compilation
 * ================================================================ */

static void compile_stmt(compiler_t *c, ast_node_t *node) {
    if (!node || c->had_error) return;

    switch (node->type) {

    case AST_LET_STMT:
    case AST_CONST_STMT: {
        /* Add local variable */
        sym_t *sym = symtab_add_local(c->current_scope,
                                       node->as.var_decl.name,
                                       node->as.var_decl.name_len);
        if (!sym) {
            c->had_error = true;
            c->error_msg = "duplicate variable";
            return;
        }

        if (node->as.var_decl.value) {
            uint8_t val_reg = compile_expr(c, node->as.var_decl.value);
            emit(c, eka_instr_encode(OP_MOVE, (uint8_t)sym->index, val_reg, 0));

            /* In init scope, also store in globals so methods can see it */
            if (c->is_init) {
                uint32_t name_idx = add_string_constant(c,
                    node->as.var_decl.name, node->as.var_decl.name_len);
                emit(c, eka_instr_encode(OP_SET_GLOBAL,
                    (uint8_t)name_idx, (uint8_t)sym->index, 0));
            }
        }
        break;
    }

    case AST_EXPR_STMT:
        compile_expr(c, node->as.expr_stmt.expr);
        break;

    case AST_RETURN_STMT: {
        uint8_t ret_reg = 0;
        if (node->as.return_stmt.value) {
            ret_reg = compile_expr(c, node->as.return_stmt.value);
        }
        emit(c, eka_instr_encode(OP_RETURN, ret_reg, 0, 0));
        break;
    }

    case AST_IF_STMT: {
        uint8_t cond = compile_expr(c, node->as.control.condition);

        /* JUMP_IF_FALSE with placeholder offset */
        uint32_t jump_idx = c->code_count;
        emit(c, eka_instr_encode(OP_JUMP_IF_FALSE, cond, 0, 0));

        /* Then body */
        compile_stmt(c, node->as.control.body);

        /* If there's an else, add JUMP over it */
        uint32_t else_jump_idx = 0;
        if (node->as.control.else_body) {
            else_jump_idx = c->code_count;
            emit(c, eka_instr_encode(OP_JUMP, 0, 0, 0));
        }

        /* Patch the JUMP_IF_FALSE to jump to here */
        int16_t offset = (int16_t)(c->code_count - jump_idx - 1);
        c->code[jump_idx] = eka_instr_encode(OP_JUMP_IF_FALSE, cond,
                                              (uint8_t)(offset >> 8),
                                              (uint8_t)(offset & 0xFF));

        /* Else body */
        if (node->as.control.else_body) {
            compile_stmt(c, node->as.control.else_body);
            offset = (int16_t)(c->code_count - else_jump_idx - 1);
            c->code[else_jump_idx] = eka_instr_encode(OP_JUMP, 0,
                                                       (uint8_t)(offset >> 8),
                                                       (uint8_t)(offset & 0xFF));
        }
        break;
    }

    case AST_FOR_STMT: {
        /* for var in iterable body [else_body]
         *
         * Compiles to index-based while loop:
         *   R(iter) = compile(iterable)
         *   R(idx) = 0
         *   R(len) = R(iter).length
         *   loop: if idx >= len → break
         *         R(var) = R(iter)[R(idx)]
         *         body
         *         idx++
         *         jump loop
         *   end:
         */

        /* Push new scope for loop variable + temporaries */
        scope_t *loop_scope = symtab_new_scope(c->current_scope, false);
        c->current_scope = loop_scope;

        /* Compile iterable into a register */
        uint8_t iter_reg = compile_expr(c, node->as.control.iterable);

        /* Index counter = 0 */
        uint8_t idx_reg = alloc_reg(c);
        emit(c, eka_instr_encode(OP_LOAD_INT, idx_reg, 0, 0));

        /* Get .length into a register */
        uint8_t len_reg = alloc_reg(c);
        uint32_t length_const = add_string_constant(c, "length", 6);
        emit(c, eka_instr_encode(OP_GET_PROP, len_reg, iter_reg,
                                  (uint8_t)length_const));

        /* Allocate loop variable */
        sym_t *loop_var = symtab_add_local(loop_scope,
                                            node->as.control.condition->as.identifier.name,
                                            node->as.control.condition->as.identifier.name_len);

        /* Jump to condition check */
        uint32_t jmp_to_check = c->code_count;
        emit(c, eka_instr_encode(OP_JUMP, 0, 0, 0));

        /* --- Loop body --- */
        uint32_t body_start = c->code_count;

        /* R(var) = R(iter)[R(idx)] */
        emit(c, eka_instr_encode(OP_GET_INDEX, (uint8_t)loop_var->index,
                                  iter_reg, idx_reg));

        /* Compile body statements */
        compile_stmt(c, node->as.control.body);

        /* idx++ */
        uint8_t one_reg = alloc_reg(c);
        emit(c, eka_instr_encode(OP_LOAD_INT, one_reg, 0, 1));
        emit(c, eka_instr_encode(OP_ADD, idx_reg, idx_reg, one_reg));

        /* --- Condition check (jumped to from before body) --- */
        /* Patch the initial jump to land here */
        int16_t to_check_offset = (int16_t)(c->code_count - jmp_to_check - 1);
        c->code[jmp_to_check] = eka_instr_encode(OP_JUMP, 0,
            (uint8_t)(to_check_offset >> 8), (uint8_t)(to_check_offset & 0xFF));

        /* R(cond) = R(idx) < R(len) */
        uint8_t cond_reg = alloc_reg(c);
        emit(c, eka_instr_encode(OP_LT, cond_reg, idx_reg, len_reg));

        /* Jump back to body if true */
        int16_t back_offset = (int16_t)(body_start - c->code_count - 1);
        emit(c, eka_instr_encode(OP_JUMP_IF_TRUE, cond_reg,
            (uint8_t)(back_offset >> 8), (uint8_t)(back_offset & 0xFF)));

        /* Pop scope */
        c->current_scope = loop_scope->enclosing;
        break;
    }

    case AST_BLOCK: {
        /* Push new scope */
        scope_t *block_scope = symtab_new_scope(c->current_scope, false);
        c->current_scope = block_scope;

        for (ast_node_t *s = node->as.block.stmts; s; s = s->next) {
            compile_stmt(c, s);
        }

        c->current_scope = block_scope->enclosing;
        break;
    }

    case AST_FUNC_DECL: {
        /* Add function name as local */
        sym_t *sym = symtab_add_local(c->current_scope,
                                       node->as.func_decl.name,
                                       node->as.func_decl.name_len);
        if (!sym) break;

        /* Create a new compiler context for the function body */
        compiler_t func_c;
        memset(&func_c, 0, sizeof(func_c));
        func_c.current_scope = symtab_new_scope(NULL, true);

        /* Add parameters as locals */
        int arity = 0;
        for (ast_node_t *p = node->as.func_decl.params; p; p = p->next) {
            symtab_add_local(func_c.current_scope, p->as.param.name, p->as.param.name_len);
            arity++;
        }

        /* Compile body */
        compile_stmt(&func_c, node->as.func_decl.body);

        /* Add implicit return nil if no return at end */
        emit(&func_c, eka_instr_encode(OP_RETURN, 0, 0, 0));

        /* Create func object */
        eka_func_t *f = eka_func_new((uint32_t)arity, (uint32_t)arity,
                                      func_c.code_count, func_c.constant_count,
                                      node->token.line);

        memcpy(f->code, func_c.code, func_c.code_count * sizeof(uint32_t));
        for (uint32_t i = 0; i < func_c.constant_count; i++) {
            f->constants[i] = func_c.constants[i];
        }

        /* Add function to outer compiler's constants */
        uint32_t func_idx = add_constant(c, eka_func_val(f));
        emit(c, eka_instr_encode(OP_CLOSURE, (uint8_t)sym->index, (uint8_t)func_idx, 0));
        break;
    }

    default:
        break;
    }
}

/* ================================================================
 * Template compilation
 * ================================================================ */

static uint8_t compile_template(compiler_t *c, ast_node_t *node) {
    if (!node) return 0;

    /* Start with an empty string accumulator */
    uint8_t accum = alloc_reg(c);
    uint32_t empty_idx = add_string_constant(c, "", 0);
    emit(c, eka_instr_encode(OP_LOAD_CONST, accum, (uint8_t)empty_idx, 0));

    for (ast_node_t *n = node->as.block.stmts; n; n = n->next) {
        switch (n->type) {
        case AST_TEMPLATE_TEXT: {
            uint32_t str_idx = add_string_constant(c,
                n->as.text.text, n->as.text.text_len);
            uint8_t str_reg = alloc_reg(c);
            emit(c, eka_instr_encode(OP_LOAD_CONST, str_reg, (uint8_t)str_idx, 0));
            emit(c, eka_instr_encode(OP_ADD, accum, accum, str_reg));
            break;
        }

        case AST_TEMPLATE_EXPR: {
            uint8_t expr_reg = compile_expr(c, n->as.expr_stmt.expr);
            /* Convert to string by concatenating with empty string */
            uint8_t empty_reg = alloc_reg(c);
            emit(c, eka_instr_encode(OP_LOAD_CONST, empty_reg, (uint8_t)empty_idx, 0));
            uint8_t str_reg = alloc_reg(c);
            emit(c, eka_instr_encode(OP_ADD, str_reg, empty_reg, expr_reg));
            emit(c, eka_instr_encode(OP_ADD, accum, accum, str_reg));
            break;
        }

        case AST_TEMPLATE_IF: {
            uint8_t cond = compile_expr(c, n->as.control.condition);

            uint32_t jump_idx = c->code_count;
            emit(c, eka_instr_encode(OP_JUMP_IF_FALSE, cond, 0, 0));

            /* Then body */
            uint8_t then_accum = compile_template(c, n->as.control.body);
            emit(c, eka_instr_encode(OP_ADD, accum, accum, then_accum));

            uint32_t else_jump_idx = 0;
            if (n->as.control.else_body) {
                else_jump_idx = c->code_count;
                emit(c, eka_instr_encode(OP_JUMP, 0, 0, 0));
            }

            /* Patch jump */
            int16_t offset = (int16_t)(c->code_count - jump_idx - 1);
            c->code[jump_idx] = eka_instr_encode(OP_JUMP_IF_FALSE, cond,
                                                  (uint8_t)(offset >> 8),
                                                  (uint8_t)(offset & 0xFF));

            if (n->as.control.else_body) {
                uint8_t else_accum = compile_template(c, n->as.control.else_body);
                emit(c, eka_instr_encode(OP_ADD, accum, accum, else_accum));
                offset = (int16_t)(c->code_count - else_jump_idx - 1);
                c->code[else_jump_idx] = eka_instr_encode(OP_JUMP, 0,
                                                           (uint8_t)(offset >> 8),
                                                           (uint8_t)(offset & 0xFF));
            }
            break;
        }

        case AST_TEMPLATE_FOR: {
            /* @for var in iterable body [@else else_body] @end
             *
             * Same index-based loop as code for-in, but:
             * 1. If iterable is empty/null → render else_body
             * 2. Otherwise loop, accumulating template output
             */

            /* Compile iterable */
            uint8_t iter_reg = compile_expr(c, n->as.control.iterable);

            /* Index counter = 0 */
            uint8_t idx_reg = alloc_reg(c);
            emit(c, eka_instr_encode(OP_LOAD_INT, idx_reg, 0, 0));

            /* Get .length */
            uint8_t len_reg = alloc_reg(c);
            uint32_t len_const = add_string_constant(c, "length", 6);
            emit(c, eka_instr_encode(OP_GET_PROP, len_reg, iter_reg,
                                      (uint8_t)len_const));

            /* Push scope for loop variable */
            scope_t *for_scope = symtab_new_scope(c->current_scope, false);
            c->current_scope = for_scope;

            sym_t *loop_var = symtab_add_local(for_scope,
                n->as.control.condition->as.identifier.name,
                n->as.control.condition->as.identifier.name_len);

            /* Check if empty: len == 0 */
            uint8_t zero_reg = alloc_reg(c);
            emit(c, eka_instr_encode(OP_LOAD_INT, zero_reg, 0, 0));
            uint8_t empty_cond = alloc_reg(c);
            emit(c, eka_instr_encode(OP_EQ, empty_cond, len_reg, zero_reg));

            uint32_t jmp_past_else = 0;
            if (n->as.control.else_body) {
                /* If empty → render else_body, skip loop */
                jmp_past_else = c->code_count;
                emit(c, eka_instr_encode(OP_JUMP_IF_FALSE, empty_cond, 0, 0));

                /* Render else body */
                uint8_t else_accum = compile_template(c, n->as.control.else_body);
                emit(c, eka_instr_encode(OP_ADD, accum, accum, else_accum));

                /* Jump past loop */
                uint32_t jmp_past_loop = c->code_count;
                emit(c, eka_instr_encode(OP_JUMP, 0, 0, 0));

                /* Patch: if not empty, jump here (to loop) */
                int16_t offset = (int16_t)(c->code_count - jmp_past_else - 1);
                c->code[jmp_past_else] = eka_instr_encode(OP_JUMP_IF_FALSE, empty_cond,
                    (uint8_t)(offset >> 8), (uint8_t)(offset & 0xFF));

                /* Jump to condition check */
                uint32_t jmp_to_check = c->code_count;
                emit(c, eka_instr_encode(OP_JUMP, 0, 0, 0));

                /* --- Loop body --- */
                uint32_t body_start = c->code_count;

                /* R(var) = R(iter)[R(idx)] */
                emit(c, eka_instr_encode(OP_GET_INDEX, (uint8_t)loop_var->index,
                                          iter_reg, idx_reg));

                /* Render body template */
                uint8_t body_accum = compile_template(c, n->as.control.body);
                emit(c, eka_instr_encode(OP_ADD, accum, accum, body_accum));

                /* idx++ */
                uint8_t inc_reg = alloc_reg(c);
                emit(c, eka_instr_encode(OP_LOAD_INT, inc_reg, 0, 1));
                emit(c, eka_instr_encode(OP_ADD, idx_reg, idx_reg, inc_reg));

                /* Condition check */
                int16_t to_check = (int16_t)(c->code_count - jmp_to_check - 1);
                c->code[jmp_to_check] = eka_instr_encode(OP_JUMP, 0,
                    (uint8_t)(to_check >> 8), (uint8_t)(to_check & 0xFF));

                uint8_t loop_cond = alloc_reg(c);
                emit(c, eka_instr_encode(OP_LT, loop_cond, idx_reg, len_reg));
                int16_t back = (int16_t)(body_start - c->code_count - 1);
                emit(c, eka_instr_encode(OP_JUMP_IF_TRUE, loop_cond,
                    (uint8_t)(back >> 8), (uint8_t)(back & 0xFF)));

                /* Patch jump past loop */
                int16_t past_offset = (int16_t)(c->code_count - jmp_past_loop - 1);
                c->code[jmp_past_loop] = eka_instr_encode(OP_JUMP, 0,
                    (uint8_t)(past_offset >> 8), (uint8_t)(past_offset & 0xFF));
            } else {
                /* No else body — skip loop if empty, enter if not empty */
                uint32_t jmp_if_empty = c->code_count;
                emit(c, eka_instr_encode(OP_JUMP_IF_TRUE, empty_cond, 0, 0));

                /* Jump to condition check */
                uint32_t jmp_to_check = c->code_count;
                emit(c, eka_instr_encode(OP_JUMP, 0, 0, 0));

                /* --- Loop body --- */
                uint32_t body_start = c->code_count;
                emit(c, eka_instr_encode(OP_GET_INDEX, (uint8_t)loop_var->index,
                                          iter_reg, idx_reg));
                uint8_t body_accum = compile_template(c, n->as.control.body);
                emit(c, eka_instr_encode(OP_ADD, accum, accum, body_accum));

                uint8_t inc_reg = alloc_reg(c);
                emit(c, eka_instr_encode(OP_LOAD_INT, inc_reg, 0, 1));
                emit(c, eka_instr_encode(OP_ADD, idx_reg, idx_reg, inc_reg));

                /* Condition check */
                int16_t to_check = (int16_t)(c->code_count - jmp_to_check - 1);
                c->code[jmp_to_check] = eka_instr_encode(OP_JUMP, 0,
                    (uint8_t)(to_check >> 8), (uint8_t)(to_check & 0xFF));

                uint8_t loop_cond = alloc_reg(c);
                emit(c, eka_instr_encode(OP_LT, loop_cond, idx_reg, len_reg));
                int16_t back = (int16_t)(body_start - c->code_count - 1);
                emit(c, eka_instr_encode(OP_JUMP_IF_TRUE, loop_cond,
                    (uint8_t)(back >> 8), (uint8_t)(back & 0xFF)));

                /* Patch: if empty, jump past loop */
                int16_t skip_offset = (int16_t)(c->code_count - jmp_if_empty - 1);
                c->code[jmp_if_empty] = eka_instr_encode(OP_JUMP_IF_TRUE, empty_cond,
                    (uint8_t)(skip_offset >> 8), (uint8_t)(skip_offset & 0xFF));
            }

            /* Pop scope */
            c->current_scope = for_scope->enclosing;
            break;
        }

        case AST_TEMPLATE_DO: {
            for (ast_node_t *s = n->as.block.stmts; s; s = s->next) {
                compile_stmt(c, s);
            }
            break;
        }

        default:
            break;
        }
    }

    return accum;
}

/* ================================================================
 * Program compilation
 * ================================================================ */

eka_compiled_program_t *eka_compile(ast_node_t *program) {
    eka_compiled_program_t *prog = arena_alloc(sizeof(eka_compiled_program_t));
    memset(prog, 0, sizeof(*prog));

    if (!program || program->type != AST_PROGRAM) {
        prog->had_error = true;
        prog->error_msg = "invalid AST";
        return prog;
    }

    /* Phase 1: Init code (top-level statements, no method blocks) */
    compiler_t init_c;
    memset(&init_c, 0, sizeof(init_c));
    init_c.current_scope = symtab_new_scope(NULL, true);
    init_c.is_init = true;

    /* Phase 2: Method blocks */
    compiler_t method_c;
    memset(&method_c, 0, sizeof(method_c));

    for (ast_node_t *node = program->as.block.stmts; node; node = node->next) {
        if (node->type == AST_METHOD_BLOCK) {
            /* Compile method */
            memset(&method_c, 0, sizeof(method_c));
            method_c.current_scope = symtab_new_scope(NULL, true);

            /* Compile template body */
            uint8_t result_reg = compile_template(&method_c, node->as.method_block.body);

            /* Return the result */
            emit(&method_c, eka_instr_encode(OP_RETURN, result_reg, 0, 0));

            /* Create function */
            if (!method_c.had_error && prog->method_count < MAX_METHODS) {
                eka_func_t *f = eka_func_new(0, 0, method_c.code_count,
                                              method_c.constant_count,
                                              node->token.line);
                memcpy(f->code, method_c.code, method_c.code_count * sizeof(uint32_t));
                for (uint32_t i = 0; i < method_c.constant_count; i++) {
                    f->constants[i] = method_c.constants[i];
                }

                int m = prog->method_count++;
                prog->methods[m].method = node->as.method_block.method;

                /* Extract path string from method AST.
                 * Intern the path so it's null-terminated — token.start
                 * points into the source buffer which may not be. */
                if (node->as.method_block.path &&
                    node->as.method_block.path->type == AST_LITERAL) {
                    eka_string_t *path_str = eka_string_intern(
                        node->as.method_block.path->token.start,
                        node->as.method_block.path->token.length);
                    prog->methods[m].path = path_str->data;
                } else {
                    prog->methods[m].path = "/";
                }
                prog->methods[m].func = f;
            }
        } else {
            /* Init statement */
            compile_stmt(&init_c, node);
        }
    }

    /* Create init function */
    emit(&init_c, eka_instr_encode(OP_RETURN, 0, 0, 0));

    if (!init_c.had_error) {
        prog->init_func = eka_func_new(0, 0, init_c.code_count,
                                        init_c.constant_count, 1);
        memcpy(prog->init_func->code, init_c.code,
               init_c.code_count * sizeof(uint32_t));
        for (uint32_t i = 0; i < init_c.constant_count; i++) {
            prog->init_func->constants[i] = init_c.constants[i];
        }
    } else {
        prog->had_error = true;
        prog->error_msg = init_c.error_msg;
    }

    return prog;
}

void eka_compile_free(eka_compiled_program_t *prog) {
    /* GC handles this */
    (void)prog;
}
