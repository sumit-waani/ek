#include "parser/parser.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================================================================
 * Token management
 * ================================================================ */

static void advance(eka_parser_t *parser) {
    parser->previous = parser->current;
    for (;;) {
        parser->current = eka_lexer_next(&parser->lexer);
        if (parser->current.type != TOKEN_ERROR) break;
        /* Error token: report and continue */
        parser->had_error = true;
        parser->error_message = "lexer error";
    }
}

static bool check(const eka_parser_t *parser, eka_token_type_t type) {
    return parser->current.type == type;
}

static bool match(eka_parser_t *parser, eka_token_type_t type) {
    if (check(parser, type)) {
        advance(parser);
        return true;
    }
    return false;
}

static bool consume(eka_parser_t *parser, eka_token_type_t type, const char *msg) {
    if (check(parser, type)) {
        advance(parser);
        return true;
    }
    parser->had_error = true;
    parser->error_message = msg;
    return false;
}

static void skip_newlines(eka_parser_t *parser) {
    while (match(parser, TOKEN_NEWLINE));
}

/* Switch lexer mode and re-read the next token in the new mode.
 * This discards parser->current (which is stale from the old mode). */
static void switch_lexer_mode(eka_parser_t *parser, eka_lexer_mode_t mode) {
    if (parser->lexer.mode == mode) return;  /* no-op — don't re-lex */
    eka_lexer_set_mode(&parser->lexer, mode);
    parser->current = eka_lexer_next(&parser->lexer);
}

/* ================================================================
 * Forward declarations
 * ================================================================ */

static ast_node_t *parse_stmt(eka_parser_t *parser);
static ast_node_t *parse_expr(eka_parser_t *parser);
static ast_node_t *parse_template_body(eka_parser_t *parser, eka_token_type_t end_type);
static ast_node_t *parse_block(eka_parser_t *parser);

/* ================================================================
 * Pratt parser — precedence levels
 * ================================================================ */

typedef enum {
    PREC_NONE,
    PREC_ASSIGN,        /* = */
    PREC_COALESCE,      /* ?? */
    PREC_OR,            /* or */
    PREC_AND,           /* and */
    PREC_EQUALITY,      /* == != ~= */
    PREC_COMPARISON,    /* < <= > >= */
    PREC_TERM,          /* + - */
    PREC_FACTOR,        /* * / % */
    PREC_UNARY,         /* - not */
    PREC_CALL,          /* . () [] ?. */
    PREC_PRIMARY,
} precedence_t;

/* Parse function pointer type for prefix/infix */
typedef ast_node_t *(*parse_fn_t)(eka_parser_t *parser, ast_node_t *left);

typedef struct {
    parse_fn_t     prefix;
    parse_fn_t     infix;
    precedence_t   precedence;
} parse_rule_t;

static parse_rule_t rules[TOKEN_COUNT];

/* Forward declare parse functions */
static ast_node_t *parse_literal(eka_parser_t *parser, ast_node_t *left);
static ast_node_t *parse_identifier(eka_parser_t *parser, ast_node_t *left);
static ast_node_t *parse_unary(eka_parser_t *parser, ast_node_t *left);
static ast_node_t *parse_binary(eka_parser_t *parser, ast_node_t *left);
static ast_node_t *parse_call(eka_parser_t *parser, ast_node_t *left);
static ast_node_t *parse_dot(eka_parser_t *parser, ast_node_t *left);
static ast_node_t *parse_index(eka_parser_t *parser, ast_node_t *left);
static ast_node_t *parse_null_safe(eka_parser_t *parser, ast_node_t *left);
static ast_node_t *parse_null_coalesce(eka_parser_t *parser, ast_node_t *left);
static ast_node_t *parse_map_or_group(eka_parser_t *parser, ast_node_t *left);
static ast_node_t *parse_list_literal(eka_parser_t *parser, ast_node_t *left);
static ast_node_t *parse_and(eka_parser_t *parser, ast_node_t *left);
static ast_node_t *parse_or(eka_parser_t *parser, ast_node_t *left);
static ast_node_t *parse_assign(eka_parser_t *parser, ast_node_t *left);

/* Initialize parse rules */
static void init_rules(void) {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    /* Literals */
    rules[TOKEN_NULL].prefix    = parse_literal;
    rules[TOKEN_TRUE].prefix    = parse_literal;
    rules[TOKEN_FALSE].prefix   = parse_literal;
    rules[TOKEN_STRING].prefix  = parse_literal;
    rules[TOKEN_NUMBER].prefix  = parse_literal;

    /* Identifier */
    rules[TOKEN_IDENTIFIER].prefix = parse_identifier;

    /* Unary */
    rules[TOKEN_MINUS].prefix   = parse_unary;
    rules[TOKEN_NOT].prefix     = parse_unary;

    /* Grouping / Map */
    rules[TOKEN_LPAREN].prefix  = parse_map_or_group;
    rules[TOKEN_LBRACE].prefix  = parse_map_or_group;

    /* List */
    rules[TOKEN_LBRACKET].prefix = parse_list_literal;

    /* Binary operators */
    rules[TOKEN_PLUS].infix     = parse_binary;
    rules[TOKEN_PLUS].precedence = PREC_TERM;

    rules[TOKEN_MINUS].infix    = parse_binary;
    rules[TOKEN_MINUS].precedence = PREC_TERM;

    rules[TOKEN_STAR].infix     = parse_binary;
    rules[TOKEN_STAR].precedence = PREC_FACTOR;

    rules[TOKEN_SLASH].infix    = parse_binary;
    rules[TOKEN_SLASH].precedence = PREC_FACTOR;

    rules[TOKEN_PERCENT].infix  = parse_binary;
    rules[TOKEN_PERCENT].precedence = PREC_FACTOR;

    rules[TOKEN_EQ].infix       = parse_binary;
    rules[TOKEN_EQ].precedence   = PREC_EQUALITY;

    rules[TOKEN_NEQ].infix      = parse_binary;
    rules[TOKEN_NEQ].precedence  = PREC_EQUALITY;

    rules[TOKEN_APPROX_EQ].infix = parse_binary;
    rules[TOKEN_APPROX_EQ].precedence = PREC_EQUALITY;

    rules[TOKEN_LT].infix       = parse_binary;
    rules[TOKEN_LT].precedence  = PREC_COMPARISON;

    rules[TOKEN_LTE].infix      = parse_binary;
    rules[TOKEN_LTE].precedence = PREC_COMPARISON;

    rules[TOKEN_GT].infix       = parse_binary;
    rules[TOKEN_GT].precedence  = PREC_COMPARISON;

    rules[TOKEN_GTE].infix      = parse_binary;
    rules[TOKEN_GTE].precedence = PREC_COMPARISON;

    /* Logic */
    rules[TOKEN_AND].infix      = parse_and;
    rules[TOKEN_AND].precedence = PREC_AND;

    rules[TOKEN_OR].infix       = parse_or;
    rules[TOKEN_OR].precedence  = PREC_OR;

    /* Null coalesce */
    rules[TOKEN_NULL_COALESCE].infix = parse_null_coalesce;
    rules[TOKEN_NULL_COALESCE].precedence = PREC_COALESCE;

    /* Call / Property / Index */
    rules[TOKEN_LPAREN].infix   = parse_call;
    rules[TOKEN_LPAREN].precedence = PREC_CALL;

    rules[TOKEN_DOT].infix      = parse_dot;
    rules[TOKEN_DOT].precedence = PREC_CALL;

    rules[TOKEN_NULL_SAFE].infix = parse_null_safe;
    rules[TOKEN_NULL_SAFE].precedence = PREC_CALL;

    rules[TOKEN_LBRACKET].infix = parse_index;
    rules[TOKEN_LBRACKET].precedence = PREC_CALL;

    /* Assignment */
    rules[TOKEN_ASSIGN].infix   = parse_assign;
    rules[TOKEN_ASSIGN].precedence = PREC_ASSIGN;
}

/* Get precedence for current token */
static precedence_t get_precedence(eka_token_type_t type) {
    return rules[type].precedence;
}

static parse_fn_t get_infix(eka_token_type_t type) {
    return rules[type].infix;
}

/* ================================================================
 * Pratt expression parser
 * ================================================================ */

static ast_node_t *parse_precedence(eka_parser_t *parser, precedence_t prec) {
    init_rules();
    skip_newlines(parser);

    eka_token_t token = parser->current;
    parse_fn_t prefix = rules[token.type].prefix;

    if (!prefix) {
        parser->had_error = true;
        parser->error_message = "expected expression";
        return NULL;
    }

    advance(parser);
    ast_node_t *left = prefix(parser, NULL);

    while (prec <= get_precedence(parser->current.type)) {
        skip_newlines(parser);
        token = parser->current;
        parse_fn_t infix = get_infix(token.type);

        if (!infix) break;

        advance(parser);
        left = infix(parser, left);
    }

    return left;
}

static ast_node_t *parse_expr(eka_parser_t *parser) {
    return parse_precedence(parser, PREC_ASSIGN);
}

/* ================================================================
 * Prefix parsers
 * ================================================================ */

static ast_node_t *parse_literal(eka_parser_t *parser, ast_node_t *left) {
    (void)left;
    ast_node_t *node = ast_new(AST_LITERAL, parser->previous);
    node->as.literal.literal_type = parser->previous.type;
    return node;
}

static ast_node_t *parse_identifier(eka_parser_t *parser, ast_node_t *left) {
    (void)left;
    ast_node_t *node = ast_new(AST_IDENTIFIER, parser->previous);
    node->as.identifier.name = parser->previous.start;
    node->as.identifier.name_len = parser->previous.length;
    return node;
}

static ast_node_t *parse_unary(eka_parser_t *parser, ast_node_t *left) {
    (void)left;
    eka_token_t op = parser->previous;
    ast_node_t *operand = parse_precedence(parser, PREC_UNARY);
    ast_node_t *node = ast_new(AST_UNARY, op);
    node->as.unary.op = op.type;
    node->as.unary.operand = operand;
    return node;
}

static ast_node_t *parse_map_or_group(eka_parser_t *parser, ast_node_t *left) {
    (void)left;
    eka_token_type_t open = parser->previous.type;
    eka_token_t open_token = parser->previous;

    /* Empty parentheses / braces */
    if ((open == TOKEN_LPAREN && match(parser, TOKEN_RPAREN)) ||
        (open == TOKEN_LBRACE && match(parser, TOKEN_RBRACE))) {
        if (open == TOKEN_LPAREN) {
            /* Empty () — not valid in Eka, but return nil like Lua */
            return ast_new(AST_LITERAL, open_token);
        }
        /* Empty {} — empty map */
        ast_node_t *map = ast_new(AST_MAP_LITERAL, open_token);
        map->as.map_literal.entries = NULL;
        return map;
    }

    /* Parse first expression */
    ast_node_t *first = parse_expr(parser);

    if (open == TOKEN_LPAREN) {
        /* Parenthesized expression */
        consume(parser, TOKEN_RPAREN, "expected ')'");
        return first;
    }

    /* Map literal: {key: value, ...} or {key, key, ...} shorthand */
    /* Check for : separating key:value */
    /* For V1: only full {key: value} syntax */
    /* Actually, if first is an identifier and next is :, it's a map entry */
    if (first->type == AST_IDENTIFIER && match(parser, TOKEN_COLON)) {
        /* key: value */
        ast_node_t *value = parse_expr(parser);

        /* Build the map with first entry */
        ast_node_t *entry = ast_new(AST_BINARY, first->token);
        entry->as.binary.op = TOKEN_COLON;  /* reuse binary for key:value */
        entry->as.binary.lhs = first;
        entry->as.binary.rhs = value;

        ast_node_t *map = ast_new(AST_MAP_LITERAL, open_token);
        map->as.map_literal.entries = entry;

        /* More entries */
        while (match(parser, TOKEN_COMMA)) {
            skip_newlines(parser);
            if (check(parser, TOKEN_RBRACE)) break;  /* trailing comma OK */

            /* key */
            ast_node_t *key;
            if (match(parser, TOKEN_IDENTIFIER)) {
                key = ast_new(AST_IDENTIFIER, parser->previous);
                key->as.identifier.name = parser->previous.start;
                key->as.identifier.name_len = parser->previous.length;
            } else if (match(parser, TOKEN_STRING)) {
                key = ast_new(AST_LITERAL, parser->previous);
                key->as.literal.literal_type = TOKEN_STRING;
            } else {
                parser->had_error = true;
                parser->error_message = "expected key in map literal";
                return map;
            }

            consume(parser, TOKEN_COLON, "expected ':' in map literal");
            ast_node_t *val = parse_expr(parser);

            ast_node_t *e = ast_new(AST_BINARY, key->token);
            e->as.binary.op = TOKEN_COLON;
            e->as.binary.lhs = key;
            e->as.binary.rhs = val;
            map->as.map_literal.entries = ast_append(map->as.map_literal.entries, e);
        }

        consume(parser, TOKEN_RBRACE, "expected '}'");
        return map;
    }

    /* Not key:value — it's a map with a single expression value? No.
     * Could be: {expression} — treat as map literal with expression as key? 
     * For now: error */
    parser->had_error = true;
    parser->error_message = "expected ':' after key in map literal";
    return first;
}

static ast_node_t *parse_list_literal(eka_parser_t *parser, ast_node_t *left) {
    (void)left;
    eka_token_t open = parser->previous;

    if (match(parser, TOKEN_RBRACKET)) {
        ast_node_t *list = ast_new(AST_LIST_LITERAL, open);
        list->as.list_literal.items = NULL;
        return list;
    }

    ast_node_t *first = parse_expr(parser);
    ast_node_t *list = ast_new(AST_LIST_LITERAL, open);
    list->as.list_literal.items = first;

    while (match(parser, TOKEN_COMMA)) {
        skip_newlines(parser);
        if (check(parser, TOKEN_RBRACKET)) break;
        ast_node_t *item = parse_expr(parser);
        list->as.list_literal.items = ast_append(list->as.list_literal.items, item);
    }

    consume(parser, TOKEN_RBRACKET, "expected ']'");
    return list;
}

/* ================================================================
 * Infix parsers
 * ================================================================ */

static ast_node_t *parse_binary(eka_parser_t *parser, ast_node_t *left) {
    eka_token_t op = parser->previous;
    precedence_t prec = get_precedence(op.type);
    ast_node_t *right = parse_precedence(parser, (precedence_t)(prec + 1));
    ast_node_t *node = ast_new(AST_BINARY, op);
    node->as.binary.op = op.type;
    node->as.binary.lhs = left;
    node->as.binary.rhs = right;
    return node;
}

static ast_node_t *parse_and(eka_parser_t *parser, ast_node_t *left) {
    eka_token_t op = parser->previous;
    ast_node_t *right = parse_precedence(parser, PREC_AND);
    ast_node_t *node = ast_new(AST_BINARY, op);
    node->as.binary.op = TOKEN_AND;
    node->as.binary.lhs = left;
    node->as.binary.rhs = right;
    return node;
}

static ast_node_t *parse_or(eka_parser_t *parser, ast_node_t *left) {
    eka_token_t op = parser->previous;
    ast_node_t *right = parse_precedence(parser, PREC_OR);
    ast_node_t *node = ast_new(AST_BINARY, op);
    node->as.binary.op = TOKEN_OR;
    node->as.binary.lhs = left;
    node->as.binary.rhs = right;
    return node;
}

static ast_node_t *parse_call(eka_parser_t *parser, ast_node_t *left) {
    eka_token_t paren = parser->previous;
    ast_node_t *node = ast_new(AST_CALL, paren);
    node->as.call.callee = left;

    if (match(parser, TOKEN_RPAREN)) {
        node->as.call.args = NULL;
        return node;
    }

    /* Parse arguments: named params: name: value */
    ast_node_t *first = parse_expr(parser);
    node->as.call.args = first;

    while (match(parser, TOKEN_COMMA)) {
        skip_newlines(parser);
        if (check(parser, TOKEN_RPAREN)) break;
        ast_node_t *arg = parse_expr(parser);
        node->as.call.args = ast_append(node->as.call.args, arg);
    }

    consume(parser, TOKEN_RPAREN, "expected ')'");
    return node;
}

static ast_node_t *parse_dot(eka_parser_t *parser, ast_node_t *left) {
    eka_token_t dot = parser->previous;
    /* Next must be an identifier */
    if (!match(parser, TOKEN_IDENTIFIER)) {
        parser->had_error = true;
        parser->error_message = "expected property name after '.'";
        return left;
    }

    ast_node_t *node = ast_new(AST_PROPERTY, dot);
    node->as.property.obj = left;
    node->as.property.prop = parser->previous.start;
    node->as.property.prop_len = parser->previous.length;
    return node;
}

static ast_node_t *parse_null_safe(eka_parser_t *parser, ast_node_t *left) {
    eka_token_t op = parser->previous;
    /* Next must be an identifier */
    if (!match(parser, TOKEN_IDENTIFIER)) {
        parser->had_error = true;
        parser->error_message = "expected property name after '?.'";
        return left;
    }

    ast_node_t *node = ast_new(AST_NULL_SAFE, op);
    node->as.property.obj = left;
    node->as.property.prop = parser->previous.start;
    node->as.property.prop_len = parser->previous.length;
    return node;
}

static ast_node_t *parse_index(eka_parser_t *parser, ast_node_t *left) {
    eka_token_t bracket = parser->previous;
    ast_node_t *idx = parse_expr(parser);
    consume(parser, TOKEN_RBRACKET, "expected ']'");

    ast_node_t *node = ast_new(AST_INDEX, bracket);
    node->as.index.obj = left;
    node->as.index.index = idx;
    return node;
}

static ast_node_t *parse_null_coalesce(eka_parser_t *parser, ast_node_t *left) {
    eka_token_t op = parser->previous;
    ast_node_t *right = parse_precedence(parser, PREC_COALESCE);
    ast_node_t *node = ast_new(AST_NULL_COALESCE, op);
    node->as.null_coalesce.lhs = left;
    node->as.null_coalesce.rhs = right;
    return node;
}

static ast_node_t *parse_assign(eka_parser_t *parser, ast_node_t *left) {
    eka_token_t eq = parser->previous;
    ast_node_t *value = parse_expr(parser);
    ast_node_t *node = ast_new(AST_ASSIGN, eq);
    node->as.assign.target = left;
    node->as.assign.value = value;
    return node;
}

/* ================================================================
 * Statement parsing
 * ================================================================ */

static ast_node_t *parse_let(eka_parser_t *parser, bool is_const) {
    eka_token_t keyword = parser->previous;

    if (!consume(parser, TOKEN_IDENTIFIER, "expected variable name")) return NULL;

    ast_node_t *node = ast_new(is_const ? AST_CONST_STMT : AST_LET_STMT, keyword);
    node->as.var_decl.name = parser->previous.start;
    node->as.var_decl.name_len = parser->previous.length;

    /* Optional type annotation */
    if (match(parser, TOKEN_COLON)) {
        if (match(parser, TOKEN_IDENTIFIER)) {
            node->as.var_decl.type_annot = parser->previous.start;
            node->as.var_decl.type_annot_len = parser->previous.length;
        }
    }

    if (consume(parser, TOKEN_ASSIGN, "expected '=' in variable declaration")) {
        node->as.var_decl.value = parse_expr(parser);
    }

    return node;
}

static ast_node_t *parse_if(eka_parser_t *parser) {
    eka_token_t if_token = parser->previous;

    ast_node_t *node = ast_new(AST_IF_STMT, if_token);
    node->as.control.condition = parse_expr(parser);

    skip_newlines(parser);
    node->as.control.body = parse_block(parser);

    /* else if / else */
    skip_newlines(parser);
    if (match(parser, TOKEN_ELSE)) {
        skip_newlines(parser);
        if (match(parser, TOKEN_IF)) {
            /* else if → parse as nested if */
            node->as.control.else_body = parse_if(parser);
        } else {
            node->as.control.else_body = parse_block(parser);
        }
    }

    return node;
}

static ast_node_t *parse_for(eka_parser_t *parser) {
    eka_token_t for_token = parser->previous;

    ast_node_t *node = ast_new(AST_FOR_STMT, for_token);

    /* for <var> in <expr> */
    if (!consume(parser, TOKEN_IDENTIFIER, "expected loop variable")) return node;
    node->as.control.condition = ast_new(AST_IDENTIFIER, parser->previous);
    node->as.control.condition->as.identifier.name = parser->previous.start;
    node->as.control.condition->as.identifier.name_len = parser->previous.length;

    consume(parser, TOKEN_IN, "expected 'in' after for variable");

    node->as.control.iterable = parse_expr(parser);

    skip_newlines(parser);
    node->as.control.body = parse_block(parser);

    /* Optional @else / else */
    skip_newlines(parser);
    if (match(parser, TOKEN_ELSE) || match(parser, TOKEN_AT_ELSE)) {
        skip_newlines(parser);
        node->as.control.else_body = parse_block(parser);
    }

    return node;
}

static ast_node_t *parse_while(eka_parser_t *parser) {
    eka_token_t while_token = parser->previous;

    ast_node_t *node = ast_new(AST_WHILE_STMT, while_token);
    node->as.control.condition = parse_expr(parser);

    skip_newlines(parser);
    node->as.control.body = parse_block(parser);

    return node;
}

static ast_node_t *parse_try(eka_parser_t *parser) {
    eka_token_t try_token = parser->previous;

    ast_node_t *node = ast_new(AST_TRY_STMT, try_token);
    skip_newlines(parser);
    node->as.try_stmt.body = parse_block(parser);

    skip_newlines(parser);
    if (!consume(parser, TOKEN_CATCH, "expected 'catch' after try block")) return node;

    consume(parser, TOKEN_IDENTIFIER, "expected error variable name after catch");
    node->as.try_stmt.error_var = parser->previous.start;
    node->as.try_stmt.error_var_len = parser->previous.length;

    skip_newlines(parser);
    node->as.try_stmt.handler = parse_block(parser);

    return node;
}

static ast_node_t *parse_return(eka_parser_t *parser) {
    eka_token_t ret_token = parser->previous;
    ast_node_t *node = ast_new(AST_RETURN_STMT, ret_token);

    /* Optional value */
    if (!check(parser, TOKEN_NEWLINE) && !check(parser, TOKEN_END) &&
        !check(parser, TOKEN_EOF) && !check(parser, TOKEN_AT_END)) {
        node->as.return_stmt.value = parse_expr(parser);
    }

    return node;
}

static ast_node_t *parse_func(eka_parser_t *parser) {
    eka_token_t func_token = parser->previous;

    if (!consume(parser, TOKEN_IDENTIFIER, "expected function name")) return NULL;

    ast_node_t *node = ast_new(AST_FUNC_DECL, func_token);
    node->as.func_decl.name = parser->previous.start;
    node->as.func_decl.name_len = parser->previous.length;

    /* Parameter list */
    consume(parser, TOKEN_LPAREN, "expected '(' after function name");

    if (!check(parser, TOKEN_RPAREN)) {
        /* Parse parameters */
        ast_node_t *first_param = NULL;

        do {
            skip_newlines(parser);
            if (!consume(parser, TOKEN_IDENTIFIER, "expected parameter name")) break;

            ast_node_t *param = ast_new(AST_PARAM, parser->previous);
            param->as.param.name = parser->previous.start;
            param->as.param.name_len = parser->previous.length;

            /* Optional type annotation */
            if (match(parser, TOKEN_COLON)) {
                if (match(parser, TOKEN_IDENTIFIER)) {
                    param->as.param.type_annot = parser->previous.start;
                    param->as.param.type_annot_len = parser->previous.length;
                }
            }

            /* Optional default value */
            if (match(parser, TOKEN_ASSIGN)) {
                param->as.param.default_value = parse_expr(parser);
            }

            first_param = ast_append(first_param, param);
        } while (match(parser, TOKEN_COMMA));

        node->as.func_decl.params = first_param;
    }

    consume(parser, TOKEN_RPAREN, "expected ')' after parameters");

    /* Optional return type annotation */
    if (match(parser, TOKEN_COLON)) {
        if (match(parser, TOKEN_IDENTIFIER)) {
            node->as.func_decl.return_type = parser->previous.start;
            node->as.func_decl.return_type_len = parser->previous.length;
        }
    }

    skip_newlines(parser);
    node->as.func_decl.body = parse_block(parser);

    return node;
}

static ast_node_t *parse_block(eka_parser_t *parser) {
    /* A block is: statements until 'end' or EOF, wrapped in AST_BLOCK.
     * Consumes the terminating 'end' token (if present). */
    eka_token_t start = parser->current;
    ast_node_t *block = ast_new(AST_BLOCK, start);
    ast_node_t *stmts = NULL;

    while (!check(parser, TOKEN_EOF) &&
           !check(parser, TOKEN_END) &&
           !check(parser, TOKEN_AT_END) &&
           !check(parser, TOKEN_ELSE) &&
           !check(parser, TOKEN_AT_ELSE) &&
           !check(parser, TOKEN_CATCH)) {
        skip_newlines(parser);

        if (check(parser, TOKEN_EOF) || check(parser, TOKEN_END) ||
            check(parser, TOKEN_AT_END) || check(parser, TOKEN_ELSE) ||
            check(parser, TOKEN_AT_ELSE) || check(parser, TOKEN_CATCH)) break;

        ast_node_t *stmt = parse_stmt(parser);
        if (stmt) {
            stmts = ast_append(stmts, stmt);
        } else if (parser->had_error) {
            break;
        }
    }

    /* Consume the 'end' token if present */
    if (check(parser, TOKEN_END)) {
        advance(parser);
    }

    block->as.block.stmts = stmts;
    return block;
}

static ast_node_t *parse_stmt(eka_parser_t *parser) {
    skip_newlines(parser);

    if (match(parser, TOKEN_LET))    return parse_let(parser, false);
    if (match(parser, TOKEN_CONST))  return parse_let(parser, true);
    if (match(parser, TOKEN_IF))     return parse_if(parser);
    if (match(parser, TOKEN_FOR))    return parse_for(parser);
    if (match(parser, TOKEN_WHILE))  return parse_while(parser);
    if (match(parser, TOKEN_TRY))    return parse_try(parser);
    if (match(parser, TOKEN_RETURN)) return parse_return(parser);
    if (match(parser, TOKEN_FUNC))   return parse_func(parser);

    /* Expression statement */
    ast_node_t *expr = parse_expr(parser);
    if (!expr) return NULL;

    ast_node_t *stmt = ast_new(AST_EXPR_STMT, expr->token);
    stmt->as.expr_stmt.expr = expr;
    return stmt;
}

/* ================================================================
 * Template parsing (inside method blocks)
 * ================================================================ */

static ast_node_t *parse_template_body(eka_parser_t *parser, eka_token_type_t end_type) {
    /* Parse template nodes until @end */
    eka_token_t start = parser->current;
    ast_node_t *block = ast_new(AST_BLOCK, start);
    ast_node_t *nodes = NULL;

    while (!check(parser, TOKEN_EOF) && !check(parser, end_type)) {
        if (match(parser, TOKEN_AT_IF)) {
            /* Lexer auto-switched to CODE mode after @if */
            ast_node_t *cond = parse_expr(parser);

            ast_node_t *node = ast_new(AST_TEMPLATE_IF, parser->previous);
            node->as.control.condition = cond;

            /* Switch back to TEMPLATE for body */
            switch_lexer_mode(parser, LEX_MODE_TEMPLATE);
            node->as.control.body = parse_template_body(parser, TOKEN_AT_END);

            /* Optional @else */
            if (match(parser, TOKEN_AT_ELSE) || match(parser, TOKEN_ELSE)) {
                node->as.control.else_body = parse_template_body(parser, TOKEN_AT_END);
            }

            /* Consume @end */
            consume(parser, TOKEN_AT_END, "expected @end after @if");

            nodes = ast_append(nodes, node);

        } else if (match(parser, TOKEN_AT_FOR)) {
            /* Lexer auto-switched to CODE mode after @for */

            ast_node_t *node = ast_new(AST_TEMPLATE_FOR, parser->previous);

            if (!consume(parser, TOKEN_IDENTIFIER, "expected loop variable after @for")) {
                nodes = ast_append(nodes, node);
                continue;
            }
            node->as.control.condition = ast_new(AST_IDENTIFIER, parser->previous);
            node->as.control.condition->as.identifier.name = parser->previous.start;
            node->as.control.condition->as.identifier.name_len = parser->previous.length;

            consume(parser, TOKEN_IN, "expected 'in' after @for variable");
            node->as.control.iterable = parse_expr(parser);

            /* Back to template for body */
            switch_lexer_mode(parser, LEX_MODE_TEMPLATE);
            node->as.control.body = parse_template_body(parser, TOKEN_AT_END);

            /* Optional @else */
            if (match(parser, TOKEN_AT_ELSE) || match(parser, TOKEN_ELSE)) {
                node->as.control.else_body = parse_template_body(parser, TOKEN_AT_END);
            }

            consume(parser, TOKEN_AT_END, "expected @end after @for");
            nodes = ast_append(nodes, node);

        } else if (match(parser, TOKEN_AT_DO)) {
            /* Lexer auto-switched to CODE mode after @do */

            ast_node_t *node = ast_new(AST_TEMPLATE_DO, parser->previous);
            ast_node_t *stmts = NULL;

            while (!check(parser, TOKEN_EOF) && !check(parser, TOKEN_AT_END)) {
                skip_newlines(parser);
                if (check(parser, TOKEN_AT_END)) break;
                ast_node_t *stmt = parse_stmt(parser);
                if (stmt) stmts = ast_append(stmts, stmt);
                if (parser->had_error) break;
            }

            node->as.block.stmts = stmts;

            consume(parser, TOKEN_AT_END, "expected @end after @do");
            switch_lexer_mode(parser, LEX_MODE_TEMPLATE);
            nodes = ast_append(nodes, node);

        } else if (match(parser, TOKEN_EXPR_START)) {
            /* Lexer auto-switched to CODE mode after {{ */
            ast_node_t *expr = parse_expr(parser);
            consume(parser, TOKEN_EXPR_END, "expected }}");
            /* Lexer auto-switched back to TEMPLATE mode after }} */

            ast_node_t *node = ast_new(AST_TEMPLATE_EXPR, parser->previous);
            node->as.expr_stmt.expr = expr;
            nodes = ast_append(nodes, node);

        } else if (match(parser, TOKEN_TEXT)) {
            ast_node_t *node = ast_new(AST_TEMPLATE_TEXT, parser->previous);
            node->as.text.text = parser->previous.start;
            node->as.text.text_len = parser->previous.length;
            nodes = ast_append(nodes, node);

        } else {
            /* Unknown token in template — skip */
            advance(parser);
        }
    }

    block->as.block.stmts = nodes;
    return block;
}

/* ================================================================
 * Method block parsing
 * ================================================================ */

static ast_node_t *parse_method_block(eka_parser_t *parser) {
    eka_token_type_t method = parser->previous.type;
    eka_token_t method_token = parser->previous;

    /* Switch to CODE mode to parse the path */
    switch_lexer_mode(parser, LEX_MODE_CODE);

    /* Parse the path as an expression (string of path segments) */
    /* The path looks like: /api/users/[id] */
    /* For V1: parse as a single expression. The lexer splits / and identifiers.
     * We'll concatenate path segments into a string expression. */
    ast_node_t *path = NULL;
    bool csrf_disabled = false;

    /* Check for CSRF off */
    if (match(parser, TOKEN_AT_CSRF)) {
        /* @csrf off — just consume 'off' */
        csrf_disabled = true;
        if (match(parser, TOKEN_IDENTIFIER)) {
            /* csrf off — valid */
        }
        /* No path needed */
        path = ast_new(AST_LITERAL, method_token);
    } else {
        /* Parse path: / followed by segments */
        /* The path is like: / | /about | /user/[id] */
        if (match(parser, TOKEN_SLASH) || match(parser, TOKEN_IDENTIFIER) ||
            match(parser, TOKEN_STRING)) {
            /* Capture full path from first to last token.
             * Adjacent tokens in source form the path (e.g. /about, /user/42). */
            eka_token_t first = parser->previous;
            while (!check(parser, TOKEN_NEWLINE) && !check(parser, TOKEN_EOF) &&
                   (check(parser, TOKEN_SLASH) || check(parser, TOKEN_IDENTIFIER) ||
                    check(parser, TOKEN_STRING) || check(parser, TOKEN_NUMBER) ||
                    check(parser, TOKEN_LBRACKET) || check(parser, TOKEN_RBRACKET))) {
                advance(parser);
            }
            eka_token_t last = parser->previous;
            eka_token_t path_tok = first;
            path_tok.length = (size_t)(last.start + last.length - first.start);
            path = ast_new(AST_LITERAL, path_tok);
        } else {
            /* @get / — root path */
            path = ast_new(AST_LITERAL, method_token);
        }
    }

    /* Switch to TEMPLATE mode for the body BEFORE reading any body tokens.
     * Don't skip_newlines() here — that would read @if in CODE mode
     * and we need the template lexer to see it for auto-switch. */
    switch_lexer_mode(parser, LEX_MODE_TEMPLATE);

    /* Parse template body until @end */
    ast_node_t *body = parse_template_body(parser, TOKEN_AT_END);

    /* Consume @end */
    if (check(parser, TOKEN_AT_END)) {
        advance(parser);
    }

    /* Back to CODE mode for next statement */
    switch_lexer_mode(parser, LEX_MODE_CODE);

    ast_node_t *node = ast_new(AST_METHOD_BLOCK, method_token);
    node->as.method_block.method = method;
    node->as.method_block.path = path;
    node->as.method_block.body = body;
    node->as.method_block.csrf_disabled = csrf_disabled;

    return node;
}

/* ================================================================
 * Program parsing (top-level)
 * ================================================================ */

ast_node_t *eka_parse(eka_parser_t *parser) {
    init_rules();

    ast_node_t *program = ast_new(AST_PROGRAM, parser->current);
    ast_node_t *stmts = NULL;

    while (!check(parser, TOKEN_EOF)) {
        skip_newlines(parser);

        if (check(parser, TOKEN_EOF)) break;

        /* Method blocks at top level */
        if (check(parser, TOKEN_AT_GET) || check(parser, TOKEN_AT_POST) ||
            check(parser, TOKEN_AT_PUT) || check(parser, TOKEN_AT_DELETE) ||
            check(parser, TOKEN_AT_PATCH)) {

            advance(parser);
            ast_node_t *method = parse_method_block(parser);
            if (method) stmts = ast_append(stmts, method);

        } else if (check(parser, TOKEN_AT_IF) || check(parser, TOKEN_AT_FOR) ||
                   check(parser, TOKEN_AT_DO)) {
            /* Template control at top level (valid: if no method blocks,
             * entire file is @get /) */
            /* For now, treat as error */
            parser->had_error = true;
            parser->error_message = "template control outside method block";
            break;

        } else {
            /* Regular statement */
            ast_node_t *stmt = parse_stmt(parser);
            if (stmt) {
                stmts = ast_append(stmts, stmt);
            } else if (parser->had_error) {
                break;
            } else {
                /* Skip unrecognized token */
                advance(parser);
            }
        }
    }

    program->as.block.stmts = stmts;
    return program;
}

/* ================================================================
 * Parser lifecycle
 * ================================================================ */

void eka_parser_init(eka_parser_t *parser, const char *source) {
    memset(parser, 0, sizeof(*parser));
    eka_lexer_init(&parser->lexer, source);
    eka_lexer_set_mode(&parser->lexer, LEX_MODE_CODE);
    advance(parser);  /* prime first token */
}

const char *eka_parser_error(const eka_parser_t *parser) {
    return parser->had_error ? parser->error_message : NULL;
}
