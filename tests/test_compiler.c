/*
 * tests/test_compiler.c — Compiler smoke tests: parse → compile → execute
 */
#include "parser/parser.h"
#include "compiler/compiler.h"
#include "core/vm.h"
#include "core/obj.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) do { tests_run++; printf("  %s ... ", name); } while(0)
#define PASS()      do { printf("PASS\n"); } while(0)
#define FAIL(msg)   do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

/* Helper: parse → compile, run init, return VM */
static eka_vm_t *compile_and_init(const char *src, const char *name) {
    eka_parser_t parser;
    eka_parser_init(&parser, src);
    ast_node_t *ast = eka_parse(&parser);
    if (parser.had_error) {
        printf("FAIL: %s — parse error: %s\n", name, parser.error_message);
        tests_failed++;
        return NULL;
    }

    eka_compiled_program_t *prog = eka_compile(ast);
    if (prog->had_error) {
        printf("FAIL: %s — compile error: %s\n", name, prog->error_msg);
        tests_failed++;
        return NULL;
    }

    eka_vm_t *vm = arena_alloc(sizeof(eka_vm_t));
    eka_vm_init(vm);

    /* Execute init */
    if (prog->init_func && prog->init_func->code_length > 0) {
        eka_closure_t *cl = eka_closure_new(prog->init_func);
        const char *err = NULL;
        eka_vm_execute_init(vm, cl, &err);
        if (err) {
            printf("FAIL: %s — init error: %s\n", name, err);
            tests_failed++;
            return NULL;
        }
    }

    return vm;
}

/* Helper: execute a method by index */
static eka_value_t execute_method(eka_vm_t *vm, eka_compiled_program_t *prog,
                                   int idx, const char *name) {
    if (idx >= prog->method_count) {
        printf("FAIL: %s — no method %d\n", name, idx);
        tests_failed++;
        return eka_nil();
    }
    eka_closure_t *cl = eka_closure_new(prog->methods[idx].func);
    const char *err = NULL;
    eka_value_t result = eka_vm_execute(vm, cl, NULL, 0, &err);
    if (err) {
        printf("FAIL: %s — runtime error: %s\n", name, err);
        tests_failed++;
        return eka_nil();
    }
    return result;
}

/* ================================================================ */

static void test_init_let(void) {
    TEST("init: let x = 42 stores global");
    const char *src = "let x = 42";
    eka_vm_t *vm = compile_and_init(src, "init_let");
    CHECK(vm != NULL, "should compile and init");
    /* Verify x is in globals — look up by interned string */
    eka_string_t *key = eka_string_intern("x", 1);
    eka_value_t val = eka_map_get(vm->globals, key);
    CHECK(eka_is_number(val) && eka_as_number(val) == 42.0, "x should be 42 in globals");
    PASS();
}

static void test_method_simple(void) {
    TEST("method: @get / → <h1>Hi</h1>");
    const char *src = "@get /\n  <h1>Hi</h1>\n@end";
    eka_parser_t parser;
    eka_parser_init(&parser, src);
    ast_node_t *ast = eka_parse(&parser);
    CHECK(!parser.had_error, "should parse");

    eka_compiled_program_t *prog = eka_compile(ast);
    CHECK(!prog->had_error, "should compile");
    CHECK(prog->method_count == 1, "should have 1 method");

    eka_vm_t vm;
    eka_vm_init(&vm);

    eka_value_t result = execute_method(&vm, prog, 0, "method_simple");
    CHECK(eka_obj_is_type(result, OBJ_STRING), "result should be string");
    /* Template includes leading whitespace and trailing newline */
    const char *out = eka_as_string(result)->data;
    CHECK(strstr(out, "<h1>Hi</h1>") != NULL, "should contain '<h1>Hi</h1>'");
    PASS();
}

static void test_method_with_expr(void) {
    TEST("method: template with {{ expr }} from init global");
    const char *src =
        "let title = \"Eka\"\n"
        "@get /\n"
        "  <h1>{{ title }}</h1>\n"
        "@end";
    eka_vm_t *vm = compile_and_init(src, "method_expr");
    CHECK(vm != NULL, "should compile and init");

    /* Re-compile to get the program (compile_and_init only returns VM, but 
     * we need the prog. Let's do it manually.) */
    eka_parser_t parser;
    eka_parser_init(&parser, src);
    ast_node_t *ast = eka_parse(&parser);
    CHECK(!parser.had_error, "should parse");

    eka_compiled_program_t *prog = eka_compile(ast);
    CHECK(!prog->had_error, "should compile");
    CHECK(prog->method_count == 1, "should have 1 method");

    eka_vm_t vm2;
    eka_vm_init(&vm2);
    /* Execute init */
    if (prog->init_func && prog->init_func->code_length > 0) {
        eka_closure_t *cl = eka_closure_new(prog->init_func);
        const char *err = NULL;
        eka_vm_execute_init(&vm2, cl, &err);
        CHECK(!err, "init should not error");
    }

    eka_value_t result = execute_method(&vm2, prog, 0, "method_expr");
    CHECK(eka_obj_is_type(result, OBJ_STRING), "result should be string");
    const char *out = eka_as_string(result)->data;
    CHECK(strstr(out, "Eka") != NULL, "should contain 'Eka' from global");
    PASS();
}

static void test_method_with_if(void) {
    TEST("method: @if in template");
    const char *src =
        "let show = true\n"
        "@get /\n"
        "  @if show\n"
        "    <p>visible</p>\n"
        "  @end\n"
        "@end";
    eka_parser_t parser;
    eka_parser_init(&parser, src);
    ast_node_t *ast = eka_parse(&parser);
    CHECK(!parser.had_error, "should parse");

    eka_compiled_program_t *prog = eka_compile(ast);
    CHECK(!prog->had_error, "should compile");

    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_vm_set_global(&vm, "show", eka_bool(true));

    eka_value_t result = execute_method(&vm, prog, 0, "method_if");
    CHECK(eka_obj_is_type(result, OBJ_STRING), "result should be string");
    const char *out = eka_as_string(result)->data;
    CHECK(strstr(out, "visible") != NULL, "should contain 'visible'");
    PASS();
}

static void test_func_call(void) {
    TEST("function declaration and call");
    /* TODO: test func compilation end-to-end */
    printf("SKIP (needs func call in VM)\n");
    tests_run--;
    return;
}

static void test_global_assignment(void) {
    TEST("global assignment: let then reassign in init");
    const char *src =
        "let x = 10\n"
        "x = 20\n"
        "@get /\n"
        "  {{ x }}\n"
        "@end";
    eka_parser_t parser;
    eka_parser_init(&parser, src);
    ast_node_t *ast = eka_parse(&parser);
    CHECK(!parser.had_error, "should parse");

    eka_compiled_program_t *prog = eka_compile(ast);
    CHECK(!prog->had_error, "should compile");
    CHECK(prog->method_count == 1, "should have 1 method");

    eka_vm_t vm;
    eka_vm_init(&vm);
    if (prog->init_func && prog->init_func->code_length > 0) {
        eka_closure_t *cl = eka_closure_new(prog->init_func);
        const char *err = NULL;
        eka_vm_execute_init(&vm, cl, &err);
        CHECK(!err, "init should not error");
    }

    eka_value_t result = execute_method(&vm, prog, 0, "global_assign");
    CHECK(eka_obj_is_type(result, OBJ_STRING), "result should be string");
    const char *out = eka_as_string(result)->data;
    /* x was reassigned to 20; template converts to "20" */
    CHECK(strstr(out, "20") != NULL, "should contain '20' from reassigned global");
    PASS();
}

/* ================================================================
 * Test: for-in loop
 * ================================================================ */

static void test_for_in_loop(void) {
    TEST("for-in loop");

    const char *src =
        "let items = [10, 20, 30]\n"
        "let total = 0\n"
        "for x in items\n"
        "  total = total + x\n"
        "end\n"
        "@get /\n"
        "  {{ total }}\n"
        "@end";

    eka_parser_t parser;
    eka_parser_init(&parser, src);
    ast_node_t *ast = eka_parse(&parser);
    CHECK(!parser.had_error, "should parse");

    eka_compiled_program_t *prog = eka_compile(ast);
    CHECK(!prog->had_error, "should compile");

    eka_vm_t vm;
    eka_vm_init(&vm);
    if (prog->init_func && prog->init_func->code_length > 0) {
        eka_closure_t *cl = eka_closure_new(prog->init_func);
        const char *err = NULL;
        eka_vm_execute_init(&vm, cl, &err);
        CHECK(!err, "init should not error");
    }

    eka_value_t result = execute_method(&vm, prog, 0, "for_in");
    CHECK(eka_obj_is_type(result, OBJ_STRING), "result should be string");
    const char *out = eka_as_string(result)->data;
    CHECK(strstr(out, "60") != NULL, "total should be 60 (10+20+30)");
    PASS();
}

/* ================================================================
 * Test: template @for loop
 * ================================================================ */

static void test_template_for(void) {
    TEST("template @for loop");

    const char *src =
        "let items = [\"a\", \"b\", \"c\"]\n"
        "@get /\n"
        "@for item in items\n"
        "<span>{{ item }}</span>\n"
        "@end\n"
        "@end";

    eka_parser_t parser;
    eka_parser_init(&parser, src);
    ast_node_t *ast = eka_parse(&parser);
    CHECK(!parser.had_error, "should parse");

    eka_compiled_program_t *prog = eka_compile(ast);
    CHECK(!prog->had_error, "should compile");

    eka_vm_t vm;
    eka_vm_init(&vm);
    if (prog->init_func && prog->init_func->code_length > 0) {
        eka_closure_t *cl = eka_closure_new(prog->init_func);
        const char *err = NULL;
        eka_vm_execute_init(&vm, cl, &err);
        CHECK(!err, "init should not error");
    }

    eka_value_t result = execute_method(&vm, prog, 0, "template_for");
    CHECK(eka_obj_is_type(result, OBJ_STRING), "result should be string");
    const char *out = eka_as_string(result)->data;
    CHECK(strstr(out, "<span>a</span>") != NULL, "should contain <span>a</span>");
    CHECK(strstr(out, "<span>b</span>") != NULL, "should contain <span>b</span>");
    CHECK(strstr(out, "<span>c</span>") != NULL, "should contain <span>c</span>");
    PASS();
}

/* ================================================================
 * Test: null-coalesce ?? operator
 * ================================================================ */

static void test_null_coalesce_fallback(void) {
    TEST("null-coalesce: null ?? \"default\" → \"default\"");
    const char *src =
        "let x = null\n"
        "let y = x ?? \"default\"\n"
        "@get /\n"
        "  {{ y }}\n"
        "@end";

    eka_parser_t parser;
    eka_parser_init(&parser, src);
    ast_node_t *ast = eka_parse(&parser);
    CHECK(!parser.had_error, "should parse");

    eka_compiled_program_t *prog = eka_compile(ast);
    CHECK(!prog->had_error, "should compile");

    eka_vm_t vm;
    eka_vm_init(&vm);
    if (prog->init_func && prog->init_func->code_length > 0) {
        eka_closure_t *cl = eka_closure_new(prog->init_func);
        const char *err = NULL;
        eka_vm_execute_init(&vm, cl, &err);
        CHECK(!err, "init should not error");
    }

    eka_value_t result = execute_method(&vm, prog, 0, "null_coalesce_fallback");
    CHECK(eka_obj_is_type(result, OBJ_STRING), "result should be string");
    const char *out = eka_as_string(result)->data;
    CHECK(strstr(out, "default") != NULL, "should contain 'default' (null ?? fallback)");
    PASS();
}

static void test_null_coalesce_preserve(void) {
    TEST("null-coalesce: \"hello\" ?? \"default\" → \"hello\"");
    const char *src =
        "let a = \"hello\"\n"
        "let b = a ?? \"default\"\n"
        "@get /\n"
        "  {{ b }}\n"
        "@end";

    eka_parser_t parser;
    eka_parser_init(&parser, src);
    ast_node_t *ast = eka_parse(&parser);
    CHECK(!parser.had_error, "should parse");

    eka_compiled_program_t *prog = eka_compile(ast);
    CHECK(!prog->had_error, "should compile");

    eka_vm_t vm;
    eka_vm_init(&vm);
    if (prog->init_func && prog->init_func->code_length > 0) {
        eka_closure_t *cl = eka_closure_new(prog->init_func);
        const char *err = NULL;
        eka_vm_execute_init(&vm, cl, &err);
        CHECK(!err, "init should not error");
    }

    eka_value_t result = execute_method(&vm, prog, 0, "null_coalesce_preserve");
    CHECK(eka_obj_is_type(result, OBJ_STRING), "result should be string");
    const char *out = eka_as_string(result)->data;
    CHECK(strstr(out, "hello") != NULL, "should contain 'hello' (non-null ?? fallback)");
    CHECK(strstr(out, "default") == NULL, "should NOT contain 'default'");
    PASS();
}

/* ================================================================
 * Test: ~= (approximate/loose equality)
 * ================================================================ */

static void test_approx_eq(void) {
    TEST("~=: equal strings return true");
    const char *src =
        "let a = \"hello\"\n"
        "let b = \"hello\"\n"
        "let result = a ~= b\n"
        "@get /\n"
        "  {{ result }}\n"
        "@end";

    eka_parser_t parser;
    eka_parser_init(&parser, src);
    ast_node_t *ast = eka_parse(&parser);
    CHECK(!parser.had_error, "should parse");

    eka_compiled_program_t *prog = eka_compile(ast);
    CHECK(!prog->had_error, "should compile");

    eka_vm_t vm;
    eka_vm_init(&vm);
    if (prog->init_func && prog->init_func->code_length > 0) {
        eka_closure_t *cl = eka_closure_new(prog->init_func);
        const char *err = NULL;
        eka_vm_execute_init(&vm, cl, &err);
        CHECK(!err, "init should not error");
    }

    eka_value_t result = execute_method(&vm, prog, 0, "approx_eq");
    CHECK(eka_obj_is_type(result, OBJ_STRING), "result should be string");
    const char *out = eka_as_string(result)->data;
    CHECK(strstr(out, "true") != NULL, "should contain 'true' (equal strings ~= equal)");
    PASS();
}

static eka_vm_t test_vm;

int main(void) {
    printf("compiler tests:\n");
    eka_vm_init(&test_vm);  /* required for arena allocation */
    test_init_let();
    test_method_simple();
    test_method_with_expr();
    test_method_with_if();
    test_func_call();
    test_global_assignment();
    test_for_in_loop();
    test_template_for();
    test_null_coalesce_fallback();
    test_null_coalesce_preserve();
    test_approx_eq();

    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
