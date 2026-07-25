/*
 * tests/test_builtins.c — Builtin function smoke tests
 */
#include "builtins/builtins.h"
#include "core/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) do { tests_run++; printf("  %s ... ", name); } while(0)
#define PASS()      do { printf("PASS\n"); } while(0)
#define FAIL(msg)   do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

/* ================================================================
 * Helpers — call a global by name (must be a native function)
 * ================================================================ */

static eka_value_t call_global(eka_vm_t *vm, const char *name,
                                eka_value_t *args, int argc) {
    eka_value_t g = eka_vm_get_global(vm, name);
    if (!eka_obj_is_type(g, OBJ_NATIVE)) return eka_nil();
    eka_native_t *nat = eka_as_native(g);
    return nat->fn(vm, nat->ctx, argc, args);
}

static eka_value_t call_method(eka_value_t obj, const char *method,
                                eka_vm_t *vm, eka_value_t *args, int argc) {
    if (!eka_obj_is_type(obj, OBJ_MAP)) return eka_nil();
    eka_map_t *map = eka_as_map(obj);
    eka_string_t *key = eka_string_intern(method, strlen(method));
    eka_value_t m = eka_map_get(map, key);
    if (!eka_obj_is_type(m, OBJ_NATIVE)) return eka_nil();
    eka_native_t *nat = eka_as_native(m);
    return nat->fn(vm, nat->ctx, argc, args);
}

/* ================================================================
 * Test: print (just ensure it doesn't crash)
 * ================================================================ */

static void test_print(void) {
    TEST("print");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t args[] = { eka_string_val(eka_string_new("hello", 5)) };
    eka_value_t result = call_global(&vm, "print", args, 1);
    /* print returns nil, and we don't check stdout */
    CHECK(eka_is_nil(result), "print should return nil");
    PASS();
}

/* ================================================================
 * Test: str.len
 * ================================================================ */

static void test_str_len(void) {
    TEST("str.len");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t str_obj = eka_vm_get_global(&vm, "str");
    eka_value_t args[] = { eka_string_val(eka_string_new("Hello", 5)) };
    eka_value_t result = call_method(str_obj, "len", &vm, args, 1);

    CHECK(eka_is_int(result), "len should return int");
    CHECK(eka_as_int(result) == 5, "len('Hello') should be 5");
    PASS();
}

/* ================================================================
 * Test: str.lower, str.upper, str.trim
 * ================================================================ */

static void test_str_case(void) {
    TEST("str.lower / upper / trim");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t str_obj = eka_vm_get_global(&vm, "str");

    /* lower */
    {
        eka_value_t args[] = { eka_string_val(eka_string_new("HELLO", 5)) };
        eka_value_t r = call_method(str_obj, "lower", &vm, args, 1);
        CHECK(eka_obj_is_type(r, OBJ_STRING), "lower returns string");
        CHECK(strcmp(eka_as_string(r)->data, "hello") == 0, "lower('HELLO') = 'hello'");
    }

    /* upper */
    {
        eka_value_t args[] = { eka_string_val(eka_string_new("hello", 5)) };
        eka_value_t r = call_method(str_obj, "upper", &vm, args, 1);
        CHECK(strcmp(eka_as_string(r)->data, "HELLO") == 0, "upper('hello') = 'HELLO'");
    }

    /* trim */
    {
        eka_value_t args[] = { eka_string_val(eka_string_new("  hi  ", 6)) };
        eka_value_t r = call_method(str_obj, "trim", &vm, args, 1);
        CHECK(strcmp(eka_as_string(r)->data, "hi") == 0, "trim('  hi  ') = 'hi'");
    }

    PASS();
}

/* ================================================================
 * Test: str.split
 * ================================================================ */

static void test_str_split(void) {
    TEST("str.split");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t str_obj = eka_vm_get_global(&vm, "str");
    eka_value_t args[] = {
        eka_string_val(eka_string_new("a,b,c", 5)),
        eka_string_val(eka_string_new(",", 1)),
    };
    eka_value_t result = call_method(str_obj, "split", &vm, args, 2);

    CHECK(eka_obj_is_type(result, OBJ_LIST), "split returns list");
    eka_list_t *list = eka_as_list(result);
    CHECK(list->length == 3, "split('a,b,c', ',') should have 3 items");
    CHECK(strcmp(eka_as_string(list->items[0])->data, "a") == 0, "[0] = 'a'");
    CHECK(strcmp(eka_as_string(list->items[1])->data, "b") == 0, "[1] = 'b'");
    CHECK(strcmp(eka_as_string(list->items[2])->data, "c") == 0, "[2] = 'c'");
    PASS();
}

/* ================================================================
 * Test: str.replace
 * ================================================================ */

static void test_str_replace(void) {
    TEST("str.replace");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t str_obj = eka_vm_get_global(&vm, "str");
    eka_value_t args[] = {
        eka_string_val(eka_string_new("foo bar", 7)),
        eka_string_val(eka_string_new("bar", 3)),
        eka_string_val(eka_string_new("baz", 3)),
    };
    eka_value_t result = call_method(str_obj, "replace", &vm, args, 3);

    CHECK(eka_obj_is_type(result, OBJ_STRING), "replace returns string");
    CHECK(strcmp(eka_as_string(result)->data, "foo baz") == 0,
          "replace('foo bar', 'bar', 'baz') = 'foo baz'");
    PASS();
}

/* ================================================================
 * Test: str.substr
 * ================================================================ */

static void test_str_substr(void) {
    TEST("str.substr");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t str_obj = eka_vm_get_global(&vm, "str");
    eka_value_t args[] = {
        eka_string_val(eka_string_new("hello", 5)),
        eka_int(1),
        eka_int(3),
    };
    eka_value_t result = call_method(str_obj, "substr", &vm, args, 3);

    CHECK(eka_obj_is_type(result, OBJ_STRING), "substr returns string");
    CHECK(strcmp(eka_as_string(result)->data, "ell") == 0,
          "substr('hello', 1, 3) = 'ell'");
    PASS();
}

/* ================================================================
 * Test: str.contains / str.index / str.starts / str.ends
 * ================================================================ */

static void test_str_search(void) {
    TEST("str.contains / index / starts / ends");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t str_obj = eka_vm_get_global(&vm, "str");

    /* contains */
    {
        eka_value_t args[] = {
            eka_string_val(eka_string_new("hello world", 11)),
            eka_string_val(eka_string_new("world", 5)),
        };
        eka_value_t r = call_method(str_obj, "contains", &vm, args, 2);
        CHECK(eka_is_bool(r) && eka_as_bool(r) == true, "contains('hello world', 'world') = true");
    }

    /* index */
    {
        eka_value_t args[] = {
            eka_string_val(eka_string_new("hello world", 11)),
            eka_string_val(eka_string_new("world", 5)),
        };
        eka_value_t r = call_method(str_obj, "index", &vm, args, 2);
        CHECK(eka_is_int(r) && eka_as_int(r) == 6, "index('hello world', 'world') = 6");
    }

    /* starts */
    {
        eka_value_t args[] = {
            eka_string_val(eka_string_new("hello", 5)),
            eka_string_val(eka_string_new("he", 2)),
        };
        eka_value_t r = call_method(str_obj, "starts", &vm, args, 2);
        CHECK(eka_is_bool(r) && eka_as_bool(r) == true, "starts('hello', 'he') = true");
    }

    /* ends */
    {
        eka_value_t args[] = {
            eka_string_val(eka_string_new("hello", 5)),
            eka_string_val(eka_string_new("lo", 2)),
        };
        eka_value_t r = call_method(str_obj, "ends", &vm, args, 2);
        CHECK(eka_is_bool(r) && eka_as_bool(r) == true, "ends('hello', 'lo') = true");
    }

    PASS();
}

/* ================================================================
 * Test: json.parse and json.stringify
 * ================================================================ */

static void test_json(void) {
    TEST("json.parse + json.stringify");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t json_obj = eka_vm_get_global(&vm, "json");

    /* parse */
    {
        const char *json_lit = "{\"name\":\"Alice\",\"age\":30}";
        eka_value_t args[] = {
            eka_string_val(eka_string_new(json_lit, strlen(json_lit))),
        };
        eka_value_t r = call_method(json_obj, "parse", &vm, args, 1);
        CHECK(eka_obj_is_type(r, OBJ_MAP), "json.parse returns map");
        eka_map_t *map = eka_as_map(r);
        eka_string_t *k = eka_string_intern("name", 4);
        eka_value_t name_val = eka_map_get(map, k);
        CHECK(eka_obj_is_type(name_val, OBJ_STRING) &&
              strcmp(eka_as_string(name_val)->data, "Alice") == 0,
              "parsed name = 'Alice'");
    }

    /* stringify (round-trip) */
    {
        eka_map_t *m = eka_map_new(4);
        eka_map_set(m, eka_string_intern("x", 1), eka_int(42));
        eka_value_t args[] = { eka_map_val(m) };
        eka_value_t r = call_method(json_obj, "stringify", &vm, args, 1);
        CHECK(eka_obj_is_type(r, OBJ_STRING), "json.stringify returns string");
        CHECK(strcmp(eka_as_string(r)->data, "{\"x\":42}") == 0,
              "stringify({x:42}) = '{\"x\":42}'");
    }

    PASS();
}

/* ================================================================
 * Test: html.escape
 * ================================================================ */

static void test_html_escape(void) {
    TEST("html.escape");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t html_obj = eka_vm_get_global(&vm, "html");
    eka_value_t args[] = {
        eka_string_val(eka_string_new("<script>alert('xss')</script>", 29)),
    };
    eka_value_t result = call_method(html_obj, "escape", &vm, args, 1);

    CHECK(eka_obj_is_type(result, OBJ_STRING), "html.escape returns string");
    CHECK(strcmp(eka_as_string(result)->data,
                 "&lt;script&gt;alert(&#x27;xss&#x27;)&lt;/script&gt;") == 0,
          "escape should replace < > ' \" &");
    PASS();
}

/* ================================================================
 * Test: crypto.sha256
 * ================================================================ */

static void test_crypto_sha256(void) {
    TEST("crypto.sha256");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t crypto_obj = eka_vm_get_global(&vm, "crypto");
    eka_value_t args[] = {
        eka_string_val(eka_string_new("hello", 5)),
    };
    eka_value_t result = call_method(crypto_obj, "sha256", &vm, args, 1);

    CHECK(eka_obj_is_type(result, OBJ_STRING), "sha256 returns string");
    CHECK(strcmp(eka_as_string(result)->data,
                 "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824") == 0,
          "sha256('hello') matches known hash");
    PASS();
}

/* ================================================================
 * Test: crypto.randomBytes
 * ================================================================ */

static void test_crypto_random_bytes(void) {
    TEST("crypto.randomBytes");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t crypto_obj = eka_vm_get_global(&vm, "crypto");
    eka_value_t args[] = { eka_int(16) };
    eka_value_t result = call_method(crypto_obj, "randomBytes", &vm, args, 1);

    CHECK(eka_obj_is_type(result, OBJ_STRING), "randomBytes returns string");
    CHECK(eka_as_string(result)->length == 32, "randomBytes(16) should be 32 hex chars");
    PASS();
}

/* ================================================================
 * Test: env.get
 * ================================================================ */

static void test_env_get(void) {
    TEST("env.get");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t env_obj = eka_vm_get_global(&vm, "env");

    /* Non-existent key */
    {
        eka_value_t args[] = { eka_string_val(eka_string_new("EKA_NONEXISTENT_TEST_VAR", 25)) };
        eka_value_t r = call_method(env_obj, "get", &vm, args, 1);
        CHECK(eka_is_nil(r), "env.get of nonexistent key = nil");
    }

    /* With default */
    {
        eka_value_t args[] = {
            eka_string_val(eka_string_new("EKA_NONEXISTENT_TEST_VAR", 25)),
            eka_string_val(eka_string_new("default", 7)),
        };
        eka_value_t r = call_method(env_obj, "get", &vm, args, 2);
        CHECK(eka_obj_is_type(r, OBJ_STRING) &&
              strcmp(eka_as_string(r)->data, "default") == 0,
              "env.get with default returns default");
    }

    /* Set a var and read it back */
    setenv("EKA_TEST_VAR", "hello", 1);
    {
        eka_value_t args[] = { eka_string_val(eka_string_new("EKA_TEST_VAR", 12)) };
        eka_value_t r = call_method(env_obj, "get", &vm, args, 1);
        CHECK(eka_obj_is_type(r, OBJ_STRING) &&
              strcmp(eka_as_string(r)->data, "hello") == 0,
              "env.get of set variable");
    }
    unsetenv("EKA_TEST_VAR");

    PASS();
}

/* ================================================================
 * Test: datetime.now
 * ================================================================ */

static void test_datetime_now(void) {
    TEST("datetime.now");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t dt_obj = eka_vm_get_global(&vm, "datetime");
    eka_value_t result = call_method(dt_obj, "now", &vm, NULL, 0);

    CHECK(eka_obj_is_type(result, OBJ_MAP), "datetime.now returns map");
    eka_map_t *map = eka_as_map(result);
    eka_value_t year = eka_map_get(map, eka_string_intern("year", 4));
    CHECK(eka_is_int(year) && eka_as_int(year) >= 2024,
          "year should be >= 2024");

    /* Test format method on the returned map */
    eka_value_t fmt_args[] = {
        eka_string_val(eka_string_new("YYYY-MM-DD", 10)),
    };
    eka_value_t formatted = call_method(result, "format", &vm, fmt_args, 1);
    CHECK(eka_obj_is_type(formatted, OBJ_STRING), "format returns string");
    CHECK(eka_as_string(formatted)->length >= 10, "formatted date has reasonable length");

    PASS();
}

/* ================================================================
 * Test: markdown.parse
 * ================================================================ */

static void test_markdown_parse(void) {
    TEST("markdown.parse");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t md_obj = eka_vm_get_global(&vm, "markdown");
    eka_value_t args[] = {
        eka_string_val(eka_string_new("# Hello\n\nWorld", 15)),
    };
    eka_value_t result = call_method(md_obj, "parse", &vm, args, 1);

    CHECK(eka_obj_is_type(result, OBJ_STRING), "markdown.parse returns string");
    CHECK(strstr(eka_as_string(result)->data, "<h1>Hello</h1>") != NULL,
          "should contain <h1>Hello</h1>");
    PASS();
}

/* ================================================================
 * Test: cache.set / cache.get / cache.delete
 * ================================================================ */

static void test_cache(void) {
    TEST("cache.set / get / delete");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t cache_obj = eka_vm_get_global(&vm, "cache");

    /* set */
    {
        eka_value_t args[] = {
            eka_string_val(eka_string_new("mykey", 5)),
            eka_string_val(eka_string_new("myvalue", 7)),
        };
        call_method(cache_obj, "set", &vm, args, 2);
    }

    /* get */
    {
        eka_value_t args[] = { eka_string_val(eka_string_new("mykey", 5)) };
        eka_value_t r = call_method(cache_obj, "get", &vm, args, 1);
        CHECK(eka_obj_is_type(r, OBJ_STRING) &&
              strcmp(eka_as_string(r)->data, "myvalue") == 0,
              "cache.get returns set value");
    }

    /* delete */
    {
        eka_value_t args[] = { eka_string_val(eka_string_new("mykey", 5)) };
        call_method(cache_obj, "delete", &vm, args, 1);
        eka_value_t r = call_method(cache_obj, "get", &vm, args, 1);
        CHECK(eka_is_nil(r), "cache.get after delete = nil");
    }

    PASS();
}

/* ================================================================
 * Test: request builtin (setup + basic fields)
 * ================================================================ */

static void test_request_builtin(void) {
    TEST("request builtin (path, method)");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    /* Craft a fake HTTP request */
    char raw[] = "GET /users HTTP/1.1\r\nHost: localhost\r\n\r\n";
    eka_http_request_t req;
    eka_http_parse(&req, raw, strlen(raw));

    eka_builtins_setup_request(&vm, &req);

    eka_value_t request_obj = eka_vm_get_global(&vm, "request");
    CHECK(eka_obj_is_type(request_obj, OBJ_MAP), "request is a map");

    eka_map_t *rmap = eka_as_map(request_obj);
    eka_value_t path = eka_map_get(rmap, eka_string_intern("path", 4));
    eka_value_t method = eka_map_get(rmap, eka_string_intern("method", 6));

    CHECK(eka_obj_is_type(path, OBJ_STRING) &&
          strcmp(eka_as_string(path)->data, "/users") == 0,
          "request.path = '/users'");
    CHECK(eka_obj_is_type(method, OBJ_STRING) &&
          strcmp(eka_as_string(method)->data, "GET") == 0,
          "request.method = 'GET'");

    eka_builtins_teardown_request(&vm);
    PASS();
}

/* ================================================================
 * Test: response builtin (status, redirect)
 * ================================================================ */

static void test_response_builtin(void) {
    TEST("response.status + redirect");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_builtins_setup_request(&vm, NULL);

    eka_value_t response_obj = eka_vm_get_global(&vm, "response");

    /* status */
    {
        eka_value_t args[] = { eka_int(404) };
        call_method(response_obj, "status", &vm, args, 1);
        CHECK(vm.response_state.status == 404, "response.status(404) sets status to 404");
    }

    /* redirect */
    {
        eka_value_t args[] = {
            eka_string_val(eka_string_new("/login", 6)),
        };
        call_method(response_obj, "redirect", &vm, args, 1);
        CHECK(vm.response_state.is_redirect, "response.redirect sets redirect flag");
        CHECK(strcmp(vm.response_state.redirect_location, "/login") == 0,
              "redirect location = '/login'");
    }

    eka_builtins_teardown_request(&vm);
    PASS();
}

/* ================================================================
 * Test: sqlite.open (in-memory)
 * ================================================================ */

static void test_sqlite_open(void) {
    TEST("sqlite.open in-memory");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t sqlite_obj = eka_vm_get_global(&vm, "sqlite");
    eka_value_t args[] = {
        eka_string_val(eka_string_new(":memory:", 8)),
    };
    eka_value_t db = call_method(sqlite_obj, "open", &vm, args, 1);

    CHECK(eka_obj_is_type(db, OBJ_MAP), "sqlite.open returns map");
    PASS();
}

int main(void) {
    printf("Builtins tests:\n");

    test_print();
    test_str_len();
    test_str_case();
    test_str_split();
    test_str_replace();
    test_str_substr();
    test_str_search();
    test_json();
    test_html_escape();
    test_crypto_sha256();
    test_crypto_random_bytes();
    test_env_get();
    test_datetime_now();
    test_markdown_parse();
    test_cache();
    test_request_builtin();
    test_response_builtin();
    test_sqlite_open();

    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
