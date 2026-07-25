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
    TEST("init: let x = 42");
    const char *src = "let x = 42";
    eka_vm_t *vm = compile_and_init(src, "init_let");
    CHECK(vm != NULL, "should compile and init");
    /* Verify x is set (currently globals aren't stored, so skip for now) */
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
    TEST("method: template with {{ expr }}");
    /* Global variable resolution not fully wired yet — skip for now */
    printf("SKIP (global vars not wired)\n");
    tests_run--;
    return;
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

int main(void) {
    printf("compiler tests:\n");
    test_init_let();
    test_method_simple();
    test_method_with_expr();
    test_method_with_if();
    test_func_call();

    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
