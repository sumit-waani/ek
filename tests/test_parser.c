/*
 * tests/test_parser.c — Parser smoke tests
 */
#include "parser/parser.h"
#include "core/vm.h"  /* for eka_vm_init (arena allocation needs a VM) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static eka_vm_t test_vm;

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) do { tests_run++; printf("  %s ... ", name); } while(0)
#define PASS()      do { printf("PASS\n"); } while(0)
#define FAIL(msg)   do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

/* Parse source, return AST node or NULL on failure. */
static ast_node_t *parse_ok(const char *src, const char *name) {
    eka_parser_t parser;
    eka_parser_init(&parser, src);
    ast_node_t *ast = eka_parse(&parser);
    if (parser.had_error) {
        printf("FAIL: %s — %s\n", name, parser.error_message);
        tests_failed++;
        return NULL;
    }
    if (!ast || ast->type != AST_PROGRAM) {
        printf("FAIL: %s — bad AST\n", name);
        tests_failed++;
        return NULL;
    }
    return ast;
}

/* Get first statement, or NULL and fail. */
static ast_node_t *first(ast_node_t *ast, const char *name) {
    if (!ast) return NULL;
    ast_node_t *s = ast->as.block.stmts;
    if (!s) { printf("FAIL: %s — no statements\n", name); tests_failed++; }
    return s;
}

/* ================================================================ */

static void test_empty(void) {
    TEST("empty program");
    ast_node_t *ast = parse_ok("", "empty");
    CHECK(ast->as.block.stmts == NULL, "should have no stmts");
    PASS();
}

static void test_let_int(void) {
    TEST("let x = 42");
    ast_node_t *ast = parse_ok("let x = 42", "let_int");
    ast_node_t *s = first(ast, "let_int"); CHECK(s, "need stmt");
    CHECK(s->type == AST_LET_STMT, "should be LET_STMT");
    CHECK(s->as.var_decl.value != NULL, "should have value");
    CHECK(s->as.var_decl.value->type == AST_LITERAL, "value should be literal");
    CHECK(s->as.var_decl.name_len == 1 && s->as.var_decl.name[0] == 'x', "name='x'");
    PASS();
}

static void test_let_string(void) {
    TEST("let name = \"Alice\"");
    ast_node_t *ast = parse_ok("let name = \"Alice\"", "let_string");
    ast_node_t *s = first(ast, "let_string"); CHECK(s, "need stmt");
    CHECK(s->type == AST_LET_STMT, "should be LET_STMT");
    PASS();
}

static void test_const(void) {
    TEST("const PI = 3.14");
    ast_node_t *ast = parse_ok("const PI = 3.14", "const");
    ast_node_t *s = first(ast, "const"); CHECK(s, "need stmt");
    CHECK(s->type == AST_CONST_STMT, "should be CONST_STMT");
    PASS();
}

static void test_binary(void) {
    TEST("a + b * c");
    ast_node_t *ast = parse_ok("let r = a + b * c", "binary");
    ast_node_t *s = first(ast, "binary"); CHECK(s, "need stmt");
    ast_node_t *v = s->as.var_decl.value;
    CHECK(v->type == AST_BINARY && v->as.binary.op == TOKEN_PLUS, "root should be +");
    CHECK(v->as.binary.rhs->type == AST_BINARY, "rhs should be binary (*)");
    CHECK(v->as.binary.rhs->as.binary.op == TOKEN_STAR, "should be *");
    PASS();
}

static void test_func_decl(void) {
    TEST("func greet(name) ... end");
    ast_node_t *ast = parse_ok("func greet(name)\n  \"Hello!\"\nend", "func_decl");
    ast_node_t *s = first(ast, "func_decl"); CHECK(s, "need stmt");
    CHECK(s->type == AST_FUNC_DECL, "should be FUNC_DECL");
    CHECK(s->as.func_decl.params != NULL, "should have params");
    PASS();
}

static void test_if_stmt(void) {
    TEST("if true ... end");
    ast_node_t *ast = parse_ok("if true\n  42\nend", "if_stmt");
    ast_node_t *s = first(ast, "if_stmt"); CHECK(s, "need stmt");
    CHECK(s->type == AST_IF_STMT, "should be IF_STMT");
    CHECK(s->as.control.condition != NULL, "should have condition");
    CHECK(s->as.control.body != NULL, "should have body");
    PASS();
}

static void test_for_in(void) {
    TEST("for item in items ... end");
    ast_node_t *ast = parse_ok("for item in items\n  print(item)\nend", "for_in");
    ast_node_t *s = first(ast, "for_in"); CHECK(s, "need stmt");
    CHECK(s->type == AST_FOR_STMT, "should be FOR_STMT");
    CHECK(s->as.control.iterable != NULL, "should have iterable");
    PASS();
}

static void test_return_val(void) {
    TEST("return 42");
    ast_node_t *ast = parse_ok("func f()\n  return 42\nend", "return_val");
    ast_node_t *s = first(ast, "return_val"); CHECK(s, "need stmt");
    ast_node_t *body = s->as.func_decl.body;
    CHECK(body != NULL, "should have body");
    ast_node_t *ret = body->as.block.stmts;
    CHECK(ret != NULL && ret->type == AST_RETURN_STMT, "should be RETURN");
    PASS();
}

static void test_map_literal(void) {
    TEST("{name: \"Alice\", age: 30}");
    ast_node_t *ast = parse_ok("let u = {name: \"Alice\", age: 30}", "map");
    ast_node_t *s = first(ast, "map"); CHECK(s, "need stmt");
    ast_node_t *v = s->as.var_decl.value;
    CHECK(v->type == AST_MAP_LITERAL, "should be MAP_LITERAL");
    int n = 0;
    for (ast_node_t *e = v->as.map_literal.entries; e; e = e->next) n++;
    CHECK(n == 2, "should have 2 entries");
    PASS();
}

static void test_list_literal(void) {
    TEST("[1, 2, 3]");
    ast_node_t *ast = parse_ok("let items = [1, 2, 3]", "list");
    ast_node_t *s = first(ast, "list"); CHECK(s, "need stmt");
    ast_node_t *v = s->as.var_decl.value;
    CHECK(v->type == AST_LIST_LITERAL, "should be LIST_LITERAL");
    int n = 0;
    for (ast_node_t *i = v->as.list_literal.items; i; i = i->next) n++;
    CHECK(n == 3, "should have 3 items");
    PASS();
}

static void test_property(void) {
    TEST("obj.prop.sub");
    ast_node_t *ast = parse_ok("let x = a.b.c", "prop");
    ast_node_t *s = first(ast, "prop"); CHECK(s, "need stmt");
    ast_node_t *v = s->as.var_decl.value;
    CHECK(v->type == AST_PROPERTY, "outer should be PROPERTY");
    CHECK(v->as.property.obj->type == AST_PROPERTY, "inner should be PROPERTY");
    PASS();
}

static void test_null_coalesce(void) {
    TEST("a ?? b");
    ast_node_t *ast = parse_ok("let x = a ?? \"default\"", "coalesce");
    ast_node_t *s = first(ast, "coalesce"); CHECK(s, "need stmt");
    CHECK(s->as.var_decl.value->type == AST_NULL_COALESCE, "should be NULL_COALESCE");
    PASS();
}

static void test_null_safe(void) {
    TEST("obj?.prop");
    ast_node_t *ast = parse_ok("let x = obj?.prop", "nullsafe");
    ast_node_t *s = first(ast, "nullsafe"); CHECK(s, "need stmt");
    CHECK(s->as.var_decl.value->type == AST_NULL_SAFE, "should be NULL_SAFE");
    PASS();
}

static void test_call(void) {
    TEST("fn(a, b)");
    ast_node_t *ast = parse_ok("print(\"hello\", 42)", "call");
    ast_node_t *s = first(ast, "call"); CHECK(s, "need stmt");
    ast_node_t *call = s->as.expr_stmt.expr;
    CHECK(call->type == AST_CALL, "should be CALL");
    int n = 0;
    for (ast_node_t *a = call->as.call.args; a; a = a->next) n++;
    CHECK(n == 2, "should have 2 args");
    PASS();
}

static void test_method_block(void) {
    TEST("@get / ... @end");
    ast_node_t *ast = parse_ok("@get /\n  <h1>Hello</h1>\n@end", "method");
    ast_node_t *s = first(ast, "method"); CHECK(s, "need stmt");
    CHECK(s->type == AST_METHOD_BLOCK, "should be METHOD_BLOCK");
    CHECK(s->as.method_block.method == TOKEN_AT_GET, "should be @get");
    PASS();
}

static void test_template_text(void) {
    TEST("template text in method block");
    ast_node_t *ast = parse_ok("@get /\n  <p>Hi</p>\n@end", "tmpl_text");
    ast_node_t *s = first(ast, "tmpl_text"); CHECK(s, "need stmt");
    ast_node_t *body = s->as.method_block.body;
    CHECK(body && body->as.block.stmts, "should have body nodes");
    int found = 0;
    for (ast_node_t *n = body->as.block.stmts; n; n = n->next)
        if (n->type == AST_TEMPLATE_TEXT) found = 1;
    CHECK(found, "should have TEXT node");
    PASS();
}

static void test_template_expr(void) {
    TEST("{{ expr }} in template");
    ast_node_t *ast = parse_ok("@get /\n  <h1>{{ name }}</h1>\n@end", "tmpl_expr");
    ast_node_t *s = first(ast, "tmpl_expr"); CHECK(s, "need stmt");
    ast_node_t *body = s->as.method_block.body;
    int found = 0;
    for (ast_node_t *n = body->as.block.stmts; n; n = n->next)
        if (n->type == AST_TEMPLATE_EXPR) found = 1;
    CHECK(found, "should have TEMPLATE_EXPR node");
    PASS();
}

static void test_template_if(void) {
    TEST("@if in template");
    ast_node_t *ast = parse_ok(
        "@get /\n"
        "  @if user\n"
        "    <p>Hi</p>\n"
        "  @end\n"
        "@end", "tmpl_if");
    ast_node_t *s = first(ast, "tmpl_if"); CHECK(s, "need stmt");
    ast_node_t *body = s->as.method_block.body;
    int found = 0;
    for (ast_node_t *n = body->as.block.stmts; n; n = n->next)
        if (n->type == AST_TEMPLATE_IF) found = 1;
    CHECK(found, "should have TEMPLATE_IF node");
    PASS();
}

int main(void) {
    printf("parser tests:\n");
    eka_vm_init(&test_vm);  /* required for arena allocation */
    test_empty();
    test_let_int();
    test_let_string();
    test_const();
    test_binary();
    test_func_decl();
    test_if_stmt();
    test_for_in();
    test_return_val();
    test_map_literal();
    test_list_literal();
    test_property();
    test_null_coalesce();
    test_null_safe();
    test_call();
    test_method_block();
    test_template_text();
    test_template_expr();
    test_template_if();

    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
