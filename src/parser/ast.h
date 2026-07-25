#ifndef AST_H
#define AST_H

#include "lexer/token.h"

#include <stdint.h>
#include <stddef.h>

/* ================================================================
 * AST Node types
 * ================================================================ */

typedef enum {
    /* --- Statements --- */
    AST_PROGRAM,          /* top-level: list of statements + method blocks */
    AST_BLOCK,            /* { stmt* } */
    AST_LET_STMT,         /* let name [:type] = expr */
    AST_CONST_STMT,       /* const name [:type] = expr */
    AST_EXPR_STMT,        /* bare expression as statement */
    AST_IF_STMT,          /* if cond then_block [else_block] */
    AST_FOR_STMT,         /* for var in expr body_block [else_block] */
    AST_WHILE_STMT,       /* while cond body_block */
    AST_TRY_STMT,         /* try body_block catch err_var handler_block */
    AST_RETURN_STMT,      /* return [expr] */
    AST_FUNC_DECL,        /* func name(params) body_block */

    /* --- Expressions --- */
    AST_LITERAL,          /* nil, bool, number, string */
    AST_IDENTIFIER,       /* variable reference */
    AST_BINARY,           /* lhs op rhs */
    AST_UNARY,            /* op expr */
    AST_CALL,             /* callee(args) */
    AST_INDEX,            /* obj[index] */
    AST_PROPERTY,         /* obj.prop */
    AST_NULL_SAFE,        /* obj?.prop */
    AST_NULL_COALESCE,    /* lhs ?? rhs */
    AST_MAP_LITERAL,      /* {key: val, ...} */
    AST_LIST_LITERAL,     /* [item, ...] */
    AST_ASSIGN,           /* target = expr */
    AST_INTERPOLATION,    /* "text ${expr} text" — string with embedded exprs */

    /* --- Template --- */
    AST_METHOD_BLOCK,     /* @get/@post path body */
    AST_TEMPLATE_TEXT,    /* raw HTML text */
    AST_TEMPLATE_EXPR,    /* {{ expr }} */
    AST_TEMPLATE_IF,      /* @if cond body [else_body] */
    AST_TEMPLATE_FOR,     /* @for var in expr body [else_body] */
    AST_TEMPLATE_DO,      /* @do stmts @end */

    /* --- Parameters --- */
    AST_PARAM,            /* name [:type] [= default] */

    AST_COUNT
} ast_node_type_t;

/* Forward declare */
typedef struct ast_node_t ast_node_t;

/* ================================================================
 * AST Node
 * ================================================================ */

struct ast_node_t {
    ast_node_type_t type;
    eka_token_t     token;       /* source location + token info */
    ast_node_t     *next;        /* sibling (for lists: stmts, args, params) */

    /* Value payload (depends on type) */
    union {
        /* AST_LITERAL */
        struct {
            eka_token_type_t literal_type;  /* TOKEN_NIL/TRUE/FALSE/STRING/NUMBER */
        } literal;

        /* AST_IDENTIFIER */
        struct {
            const char *name;
            size_t      name_len;
        } identifier;

        /* AST_BINARY */
        struct {
            ast_node_t     *lhs;
            ast_node_t     *rhs;
            eka_token_type_t op;
        } binary;

        /* AST_UNARY */
        struct {
            ast_node_t     *operand;
            eka_token_type_t op;
        } unary;

        /* AST_CALL */
        struct {
            ast_node_t *callee;
            ast_node_t *args;        /* linked list */
        } call;

        /* AST_INDEX */
        struct {
            ast_node_t *obj;
            ast_node_t *index;
        } index;

        /* AST_PROPERTY / AST_NULL_SAFE */
        struct {
            ast_node_t *obj;
            const char *prop;
            size_t      prop_len;
        } property;

        /* AST_NULL_COALESCE */
        struct {
            ast_node_t *lhs;
            ast_node_t *rhs;
        } null_coalesce;

        /* AST_MAP_LITERAL */
        struct {
            ast_node_t *entries;     /* linked list of key-value pairs */
        } map_literal;

        /* AST_LIST_LITERAL */
        struct {
            ast_node_t *items;       /* linked list */
        } list_literal;

        /* AST_ASSIGN */
        struct {
            ast_node_t *target;
            ast_node_t *value;
        } assign;

        /* AST_INTERPOLATION */
        struct {
            ast_node_t *parts;       /* linked list of string literals and expressions */
        } interpolation;

        /* AST_METHOD_BLOCK */
        struct {
            eka_token_type_t method;  /* TOKEN_AT_GET etc */
            ast_node_t      *path;    /* expression (usually string) */
            ast_node_t      *body;    /* block of template nodes */
        } method_block;

        /* AST_TEMPLATE_IF / AST_FOR_STMT */
        struct {
            ast_node_t *condition;    /* or for: variable */
            ast_node_t *body;
            ast_node_t *else_body;    /* optional */
            ast_node_t *iterable;     /* for: the expression to iterate */
        } control;

        /* AST_TEMPLATE_DO / AST_BLOCK */
        struct {
            ast_node_t *stmts;        /* linked list */
        } block;

        /* AST_LET_STMT / AST_CONST_STMT */
        struct {
            const char  *name;
            size_t       name_len;
            const char  *type_annot;   /* optional */
            size_t       type_annot_len;
            ast_node_t  *value;
        } var_decl;

        /* AST_FUNC_DECL */
        struct {
            const char  *name;
            size_t       name_len;
            const char  *return_type;  /* optional */
            size_t       return_type_len;
            ast_node_t  *params;       /* linked list of AST_PARAM */
            ast_node_t  *body;         /* AST_BLOCK */
        } func_decl;

        /* AST_PARAM */
        struct {
            const char *name;
            size_t      name_len;
            const char *type_annot;    /* optional */
            size_t      type_annot_len;
            ast_node_t *default_value; /* optional */
        } param;

        /* AST_RETURN_STMT */
        struct {
            ast_node_t *value;         /* optional */
        } return_stmt;

        /* AST_TRY_STMT */
        struct {
            ast_node_t *body;
            const char  *error_var;
            size_t       error_var_len;
            ast_node_t  *handler;
        } try_stmt;

        /* AST_EXPR_STMT / AST_TEMPLATE_EXPR */
        struct {
            ast_node_t *expr;
        } expr_stmt;

        /* AST_TEMPLATE_TEXT */
        struct {
            const char *text;
            size_t      text_len;
        } text;
    } as;
};

/* ================================================================
 * AST Node constructors
 * ================================================================ */

ast_node_t *ast_new(ast_node_type_t type, eka_token_t token);
void        ast_free(ast_node_t *node);

/* Append a sibling to the linked list. Returns the head. */
ast_node_t *ast_append(ast_node_t *head, ast_node_t *node);

/* ================================================================
 * Debug / Printing
 * ================================================================ */

void ast_print(ast_node_t *node, int indent);

#endif /* AST_H */
