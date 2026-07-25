#include "parser/ast.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Allocator from obj.c — reuse arena for AST nodes */
#include "core/obj.h"

ast_node_t *ast_new(ast_node_type_t type, eka_token_t token) {
    ast_node_t *node = arena_alloc(sizeof(ast_node_t));
    memset(node, 0, sizeof(*node));
    node->type  = type;
    node->token = token;
    return node;
}

void ast_free(ast_node_t *node) {
    /* GC handles this — no-op for now */
    (void)node;
}

ast_node_t *ast_append(ast_node_t *head, ast_node_t *node) {
    if (!head) return node;
    ast_node_t *cur = head;
    while (cur->next) cur = cur->next;
    cur->next = node;
    return head;
}

/* ================================================================
 * Debug printing
 * ================================================================ */

static const char *ast_type_name(ast_node_type_t type) {
    switch (type) {
    case AST_PROGRAM:         return "PROGRAM";
    case AST_BLOCK:           return "BLOCK";
    case AST_LET_STMT:        return "LET";
    case AST_CONST_STMT:      return "CONST";
    case AST_EXPR_STMT:       return "EXPR_STMT";
    case AST_IF_STMT:         return "IF";
    case AST_FOR_STMT:        return "FOR";
    case AST_WHILE_STMT:      return "WHILE";
    case AST_TRY_STMT:        return "TRY";
    case AST_RETURN_STMT:     return "RETURN";
    case AST_FUNC_DECL:       return "FUNC";
    case AST_LITERAL:         return "LITERAL";
    case AST_IDENTIFIER:      return "IDENT";
    case AST_BINARY:          return "BINARY";
    case AST_UNARY:           return "UNARY";
    case AST_CALL:            return "CALL";
    case AST_INDEX:           return "INDEX";
    case AST_PROPERTY:        return "PROPERTY";
    case AST_NULL_SAFE:       return "NULL_SAFE";
    case AST_NULL_COALESCE:   return "NULL_COALESCE";
    case AST_MAP_LITERAL:     return "MAP";
    case AST_LIST_LITERAL:    return "LIST";
    case AST_ASSIGN:          return "ASSIGN";
    case AST_INTERPOLATION:   return "INTERP";
    case AST_METHOD_BLOCK:    return "METHOD";
    case AST_TEMPLATE_TEXT:   return "TEXT";
    case AST_TEMPLATE_EXPR:   return "TMPL_EXPR";
    case AST_TEMPLATE_IF:     return "TMPL_IF";
    case AST_TEMPLATE_FOR:    return "TMPL_FOR";
    case AST_TEMPLATE_DO:     return "TMPL_DO";
    case AST_PARAM:           return "PARAM";
    default:                  return "???";
    }
}

static void print_indent(int indent) {
    for (int i = 0; i < indent; i++) printf("  ");
}

void ast_print(ast_node_t *node, int indent) {
    if (!node) { print_indent(indent); printf("(null)\n"); return; }

    print_indent(indent);
    printf("%s", ast_type_name(node->type));

    switch (node->type) {
    case AST_LITERAL:
        printf(" '%.*s'", (int)node->token.length, node->token.start);
        break;
    case AST_IDENTIFIER:
        printf(" '%.*s'", (int)node->as.identifier.name_len, node->as.identifier.name);
        break;
    case AST_BINARY:
        printf(" %s", eka_token_name(node->as.binary.op));
        break;
    case AST_UNARY:
        printf(" %s", eka_token_name(node->as.unary.op));
        break;
    case AST_LET_STMT:
    case AST_CONST_STMT:
        printf(" %.*s", (int)node->as.var_decl.name_len, node->as.var_decl.name);
        break;
    case AST_FUNC_DECL:
        printf(" %.*s", (int)node->as.func_decl.name_len, node->as.func_decl.name);
        break;
    case AST_METHOD_BLOCK:
        printf(" %s", eka_token_name(node->as.method_block.method));
        break;
    case AST_TEMPLATE_TEXT:
        printf(" '%.*s'", (int)(node->as.text.text_len > 40 ? 40 : node->as.text.text_len),
               node->as.text.text);
        break;
    case AST_RETURN_STMT:
        if (!node->as.return_stmt.value) printf(" void");
        break;
    case AST_PARAM:
        printf(" %.*s", (int)node->as.param.name_len, node->as.param.name);
        break;
    default:
        break;
    }
    printf("\n");

    /* Recurse into children */
    switch (node->type) {
    case AST_PROGRAM:
    case AST_BLOCK:
        for (ast_node_t *s = node->as.block.stmts; s; s = s->next) {
            ast_print(s, indent + 1);
        }
        break;

    case AST_LET_STMT:
    case AST_CONST_STMT:
        if (node->as.var_decl.value) ast_print(node->as.var_decl.value, indent + 1);
        break;

    case AST_BINARY:
        ast_print(node->as.binary.lhs, indent + 1);
        ast_print(node->as.binary.rhs, indent + 1);
        break;

    case AST_UNARY:
        ast_print(node->as.unary.operand, indent + 1);
        break;

    case AST_CALL:
        ast_print(node->as.call.callee, indent + 1);
        for (ast_node_t *a = node->as.call.args; a; a = a->next) {
            ast_print(a, indent + 1);
        }
        break;

    case AST_INDEX:
        ast_print(node->as.index.obj, indent + 1);
        ast_print(node->as.index.index, indent + 1);
        break;

    case AST_PROPERTY:
    case AST_NULL_SAFE:
        ast_print(node->as.property.obj, indent + 1);
        break;

    case AST_ASSIGN:
        ast_print(node->as.assign.target, indent + 1);
        ast_print(node->as.assign.value, indent + 1);
        break;

    case AST_MAP_LITERAL:
        for (ast_node_t *e = node->as.map_literal.entries; e; e = e->next) {
            ast_print(e, indent + 1);
        }
        break;

    case AST_LIST_LITERAL:
        for (ast_node_t *i = node->as.list_literal.items; i; i = i->next) {
            ast_print(i, indent + 1);
        }
        break;

    case AST_IF_STMT:
        ast_print(node->as.control.condition, indent + 1);
        ast_print(node->as.control.body, indent + 1);
        if (node->as.control.else_body) ast_print(node->as.control.else_body, indent + 1);
        break;

    case AST_FOR_STMT:
        ast_print(node->as.control.condition, indent + 1);  /* variable */
        ast_print(node->as.control.iterable, indent + 1);
        ast_print(node->as.control.body, indent + 1);
        if (node->as.control.else_body) ast_print(node->as.control.else_body, indent + 1);
        break;

    case AST_WHILE_STMT:
        ast_print(node->as.control.condition, indent + 1);
        ast_print(node->as.control.body, indent + 1);
        break;

    case AST_TRY_STMT:
        ast_print(node->as.try_stmt.body, indent + 1);
        ast_print(node->as.try_stmt.handler, indent + 1);
        break;

    case AST_RETURN_STMT:
        if (node->as.return_stmt.value) ast_print(node->as.return_stmt.value, indent + 1);
        break;

    case AST_FUNC_DECL:
        for (ast_node_t *p = node->as.func_decl.params; p; p = p->next) {
            ast_print(p, indent + 1);
        }
        if (node->as.func_decl.body) ast_print(node->as.func_decl.body, indent + 1);
        break;

    case AST_METHOD_BLOCK:
        if (node->as.method_block.path) ast_print(node->as.method_block.path, indent + 1);
        if (node->as.method_block.body) ast_print(node->as.method_block.body, indent + 1);
        break;

    case AST_TEMPLATE_IF:
        if (node->as.control.condition) ast_print(node->as.control.condition, indent + 1);
        if (node->as.control.body) ast_print(node->as.control.body, indent + 1);
        if (node->as.control.else_body) ast_print(node->as.control.else_body, indent + 1);
        break;

    case AST_TEMPLATE_FOR:
        if (node->as.control.condition) ast_print(node->as.control.condition, indent + 1);
        if (node->as.control.iterable) ast_print(node->as.control.iterable, indent + 1);
        if (node->as.control.body) ast_print(node->as.control.body, indent + 1);
        if (node->as.control.else_body) ast_print(node->as.control.else_body, indent + 1);
        break;

    case AST_TEMPLATE_DO:
        for (ast_node_t *s = node->as.block.stmts; s; s = s->next) {
            ast_print(s, indent + 1);
        }
        break;

    case AST_TEMPLATE_EXPR:
        if (node->as.expr_stmt.expr) ast_print(node->as.expr_stmt.expr, indent + 1);
        break;

    case AST_NULL_COALESCE:
        ast_print(node->as.null_coalesce.lhs, indent + 1);
        ast_print(node->as.null_coalesce.rhs, indent + 1);
        break;

    case AST_PARAM:
        if (node->as.param.default_value) ast_print(node->as.param.default_value, indent + 1);
        break;

    default:
        break;
    }
}
