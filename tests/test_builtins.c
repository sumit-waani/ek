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

/* ================================================================
 * Phase 2: math
 * ================================================================ */

static void test_math(void) {
    TEST("math.floor / ceil / abs / min / max");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t math_obj = eka_vm_get_global(&vm, "math");

    /* floor */
    {
        eka_value_t args[] = { eka_number(3.7) };
        eka_value_t r = call_method(math_obj, "floor", &vm, args, 1);
        CHECK(eka_is_number(r) && eka_as_number(r) == 3.0, "floor(3.7) = 3");
    }
    /* ceil */
    {
        eka_value_t args[] = { eka_number(3.2) };
        eka_value_t r = call_method(math_obj, "ceil", &vm, args, 1);
        CHECK(eka_is_number(r) && eka_as_number(r) == 4.0, "ceil(3.2) = 4");
    }
    /* abs */
    {
        eka_value_t args[] = { eka_int(-42) };
        eka_value_t r = call_method(math_obj, "abs", &vm, args, 1);
        CHECK(eka_is_int(r) && eka_as_int(r) == 42, "abs(-42) = 42");
    }
    /* min */
    {
        eka_value_t args[] = { eka_int(10), eka_int(3) };
        eka_value_t r = call_method(math_obj, "min", &vm, args, 2);
        CHECK(eka_is_number(r) && eka_as_number(r) == 3.0, "min(10,3) = 3");
    }
    PASS();
}

/* ================================================================
 * Phase 2: slug
 * ================================================================ */

static void test_slug(void) {
    TEST("slug.make");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t slug_obj = eka_vm_get_global(&vm, "slug");
    {
        eka_value_t args[] = { eka_string_val(eka_string_new("Hello World!", 12)) };
        eka_value_t r = call_method(slug_obj, "make", &vm, args, 1);
        CHECK(eka_obj_is_type(r, OBJ_STRING) &&
              strcmp(eka_as_string(r)->data, "hello-world") == 0,
              "slug.make('Hello World!') = 'hello-world'");
    }
    PASS();
}

/* ================================================================
 * Phase 2: base64
 * ================================================================ */

static void test_base64(void) {
    TEST("base64.encode / decode");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t b64_obj = eka_vm_get_global(&vm, "base64");

    {
        eka_value_t args[] = { eka_string_val(eka_string_new("hello", 5)) };
        eka_value_t en = call_method(b64_obj, "encode", &vm, args, 1);
        CHECK(eka_obj_is_type(en, OBJ_STRING), "encode returns string");

        /* Decode it back */
        eka_value_t args2[] = { en };
        eka_value_t de = call_method(b64_obj, "decode", &vm, args2, 1);
        CHECK(eka_obj_is_type(de, OBJ_STRING) &&
              strcmp(eka_as_string(de)->data, "hello") == 0,
              "base64 roundtrip: 'hello'");
    }
    PASS();
}

/* ================================================================
 * Phase 2: url
 * ================================================================ */

static void test_url(void) {
    TEST("url.parse");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t url_obj = eka_vm_get_global(&vm, "url");
    {
        eka_value_t args[] = {
            eka_string_val(eka_string_new("https://example.com/path?q=1", 30))
        };
        eka_value_t r = call_method(url_obj, "parse", &vm, args, 1);
        CHECK(eka_obj_is_type(r, OBJ_MAP), "url.parse returns map");

        eka_map_t *m = eka_as_map(r);
        eka_value_t scheme = eka_map_get(m, eka_string_intern("scheme", 6));
        CHECK(eka_obj_is_type(scheme, OBJ_STRING) &&
              strcmp(eka_as_string(scheme)->data, "https") == 0,
              "scheme = 'https'");
    }
    PASS();
}

/* ================================================================
 * Phase 2: validate
 * ================================================================ */

static void test_validate(void) {
    TEST("validate.email / required");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t val_obj = eka_vm_get_global(&vm, "validate");

    {
        eka_value_t args[] = { eka_string_val(eka_string_new("test@example.com", 16)) };
        eka_value_t r = call_method(val_obj, "email", &vm, args, 1);
        CHECK(eka_is_bool(r) && eka_as_bool(r) == true, "email test@example.com = true");
    }
    {
        eka_value_t args[] = { eka_string_val(eka_string_new("notanemail", 10)) };
        eka_value_t r = call_method(val_obj, "email", &vm, args, 1);
        CHECK(eka_is_bool(r) && eka_as_bool(r) == false, "email 'notanemail' = false");
    }
    {
        eka_value_t args[] = { eka_string_val(eka_string_new("hello", 5)) };
        eka_value_t r = call_method(val_obj, "required", &vm, args, 1);
        CHECK(eka_is_bool(r) && eka_as_bool(r) == true, "required('hello') = true");
    }
    PASS();
}

/* ================================================================
 * Phase 2: fs (exists)
 * ================================================================ */

static void test_fs(void) {
    TEST("fs.exists / write / read");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t fs_obj = eka_vm_get_global(&vm, "fs");

    /* Write a temp file then check it exists */
    {
        eka_value_t args[] = {
            eka_string_val(eka_string_new("eka_fs_test_tmp.txt", 20)),
            eka_string_val(eka_string_new("hello", 5)),
        };
        call_method(fs_obj, "write", &vm, args, 2);

        eka_value_t args2[] = {
            eka_string_val(eka_string_new("eka_fs_test_tmp.txt", 20)),
        };
        eka_value_t r = call_method(fs_obj, "exists", &vm, args2, 1);
        CHECK(eka_is_bool(r) && eka_as_bool(r) == true, "test file exists");

        /* Read it back */
        eka_value_t r2 = call_method(fs_obj, "read", &vm, args2, 1);
        CHECK(eka_obj_is_type(r2, OBJ_STRING) &&
              strcmp(eka_as_string(r2)->data, "hello") == 0,
              "fs.read returns written content");

        /* Clean up */
        call_method(fs_obj, "delete", &vm, args2, 1);

        eka_value_t r3 = call_method(fs_obj, "exists", &vm, args2, 1);
        CHECK(eka_is_bool(r3) && eka_as_bool(r3) == false, "deleted file = false");
    }
    PASS();
}

/* ================================================================
 * Phase 2: regex
 * ================================================================ */

static void test_regex(void) {
    TEST("regex.test / match");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t regex_obj = eka_vm_get_global(&vm, "regex");

    /* test */
    {
        eka_value_t args[] = {
            eka_string_val(eka_string_new("^\\d+$", 5)),
            eka_string_val(eka_string_new("12345", 5)),
        };
        eka_value_t r = call_method(regex_obj, "test", &vm, args, 2);
        CHECK(eka_is_bool(r) && eka_as_bool(r) == true, "regex.test('^\\d+$', '12345') = true");
    }
    /* match */
    {
        eka_value_t args[] = {
            eka_string_val(eka_string_new("^hello", 6)),
            eka_string_val(eka_string_new("hello world", 11)),
        };
        eka_value_t r = call_method(regex_obj, "match", &vm, args, 2);
        CHECK(eka_obj_is_type(r, OBJ_STRING) &&
              strcmp(eka_as_string(r)->data, "hello") == 0,
              "match '^hello' in 'hello world' = 'hello'");
    }
    PASS();
}

/* ================================================================
 * Phase 2: sitemap
 * ================================================================ */

static void test_sitemap(void) {
    TEST("sitemap.generate");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t sm_obj = eka_vm_get_global(&vm, "sitemap");

    eka_list_t *urls = eka_list_new(4);
    eka_map_t *u = eka_map_new(4);
    eka_map_set(u, eka_string_intern("url", 3), eka_string_val(eka_string_new("/", 1)));
    eka_list_push(urls, eka_map_val(u));

    eka_value_t args[] = { eka_list_val(urls) };
    eka_value_t r = call_method(sm_obj, "generate", &vm, args, 1);

    CHECK(eka_obj_is_type(r, OBJ_STRING), "sitemap.generate returns string");
    CHECK(strstr(eka_as_string(r)->data, "<urlset") != NULL, "contains <urlset");
    PASS();
}

/* ================================================================
 * Phase 2: rss
 * ================================================================ */

static void test_rss(void) {
    TEST("rss.generate");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t rss_obj = eka_vm_get_global(&vm, "rss");

    eka_map_t *feed = eka_map_new(8);
    eka_map_set(feed, eka_string_intern("title", 5),
                eka_string_val(eka_string_new("Test", 4)));
    eka_map_set(feed, eka_string_intern("link", 4),
                eka_string_val(eka_string_new("http://example.com", 18)));
    eka_map_set(feed, eka_string_intern("description", 11),
                eka_string_val(eka_string_new("Desc", 4)));

    eka_value_t args[] = { eka_map_val(feed) };
    eka_value_t r = call_method(rss_obj, "generate", &vm, args, 1);

    CHECK(eka_obj_is_type(r, OBJ_STRING), "rss.generate returns string");
    CHECK(strstr(eka_as_string(r)->data, "<rss") != NULL, "contains <rss");
    PASS();
}

/* ================================================================
 * Phase 2: i18n
 * ================================================================ */

static void test_i18n(void) {
    TEST("i18n.t interpolation");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    eka_value_t i18n_obj = eka_vm_get_global(&vm, "i18n");

    /* Set lang */
    {
        eka_value_t args[] = { eka_string_val(eka_string_new("en", 2)) };
        call_method(i18n_obj, "set", &vm, args, 1);
    }

    /* Translate with interpolation */
    {
        eka_map_t *vars = eka_map_new(4);
        eka_map_set(vars, eka_string_intern("name", 4),
                    eka_string_val(eka_string_new("Alice", 5)));

        eka_value_t args[] = {
            eka_string_val(eka_string_new("Hello, {{ name }}!", 18)),
            eka_map_val(vars),
        };
        eka_value_t r = call_method(i18n_obj, "t", &vm, args, 2);
        CHECK(eka_obj_is_type(r, OBJ_STRING) &&
              strcmp(eka_as_string(r)->data, "Hello, Alice!") == 0,
              "i18n.t interpolates {{ name }}");
    }
    PASS();
}

/* ================================================================
 * Phase 2: session — full lifecycle
 * ================================================================ */

static void test_session(void) {
    TEST("session full lifecycle");
    eka_vm_t vm;
    eka_vm_init(&vm);
    eka_builtins_register(&vm);

    /* Initialize session DB */
    eka_session_init_db(&vm);

    eka_value_t sess_obj = eka_vm_get_global(&vm, "session");

    /* --- Test 1: session.set + session.get --- */
    {
        /* Load empty session (no cookie → new session) */
        vm.session_id[0] = '\0';
        eka_session_load(&vm);
        CHECK(vm.session_is_new, "new session without cookie");

        /* Set a value */
        eka_value_t set_args[] = {
            eka_string_val(eka_string_new("user", 4)),
            eka_string_val(eka_string_new("alice", 5)),
        };
        call_method(sess_obj, "set", &vm, set_args, 2);

        /* Get it back */
        eka_value_t get_args[] = { eka_string_val(eka_string_new("user", 4)) };
        eka_value_t r = call_method(sess_obj, "get", &vm, get_args, 1);
        CHECK(eka_obj_is_type(r, OBJ_STRING) &&
              strcmp(eka_as_string(r)->data, "alice") == 0,
              "session.get returns set value");

        CHECK(vm.session_dirty, "session is dirty after set");
    }

    /* --- Test 2: session.get with default value --- */
    {
        eka_value_t get_args[] = {
            eka_string_val(eka_string_new("nonexistent", 11)),
            eka_string_val(eka_string_new("fallback", 8)),
        };
        eka_value_t r = call_method(sess_obj, "get", &vm, get_args, 2);
        CHECK(eka_obj_is_type(r, OBJ_STRING) &&
              strcmp(eka_as_string(r)->data, "fallback") == 0,
              "session.get returns default for missing key");
    }

    /* --- Test 3: session.delete --- */
    {
        eka_value_t del_args[] = { eka_string_val(eka_string_new("user", 4)) };
        call_method(sess_obj, "delete", &vm, del_args, 1);

        eka_value_t get_args[] = { eka_string_val(eka_string_new("user", 4)) };
        eka_value_t r = call_method(sess_obj, "get", &vm, get_args, 1);
        CHECK(eka_is_nil(r), "session.get after delete = nil");
    }

    /* --- Test 4: session.clear --- */
    {
        /* Set some values first */
        eka_value_t set_args1[] = {
            eka_string_val(eka_string_new("a", 1)),
            eka_int(1),
        };
        eka_value_t set_args2[] = {
            eka_string_val(eka_string_new("b", 1)),
            eka_int(2),
        };
        call_method(sess_obj, "set", &vm, set_args1, 2);
        call_method(sess_obj, "set", &vm, set_args2, 2);

        /* Clear */
        call_method(sess_obj, "clear", &vm, NULL, 0);

        /* Both should be gone */
        eka_value_t get_a[] = { eka_string_val(eka_string_new("a", 1)) };
        CHECK(eka_is_nil(call_method(sess_obj, "get", &vm, get_a, 1)),
              "session.get after clear = nil (key 'a')");

        CHECK(vm.session_id[0] == '\0', "session_id cleared after clear");
    }

    /* --- Test 5: persistence — save and reload --- */
    {
        /* Load a fresh session and set data */
        vm.session_id[0] = '\0';
        vm.session_dirty = false;
        vm.session_is_new = true;
        eka_session_load(&vm);

        eka_value_t set_args[] = {
            eka_string_val(eka_string_new("color", 5)),
            eka_string_val(eka_string_new("blue", 4)),
        };
        call_method(sess_obj, "set", &vm, set_args, 2);

        /* Save to SQLite */
        eka_session_save(&vm);
        CHECK(vm.session_id[0] != '\0', "session_id generated after save");

        /* Now reload with the same session_id */
        char saved_id[33];
        memcpy(saved_id, vm.session_id, 33);

        eka_vm_t vm2;
        eka_vm_init(&vm2);
        eka_builtins_register(&vm2);
        vm2.session_db = vm.session_db;  /* share the DB connection */
        memcpy(vm2.session_id, saved_id, 33);
        eka_session_load(&vm2);

        CHECK(!vm2.session_is_new, "reloaded session is not new");

        eka_value_t get_args[] = { eka_string_val(eka_string_new("color", 5)) };
        eka_value_t r = call_method(
            eka_vm_get_global(&vm2, "session"), "get", &vm2, get_args, 1);
        CHECK(eka_obj_is_type(r, OBJ_STRING) &&
              strcmp(eka_as_string(r)->data, "blue") == 0,
              "session data persists across save/load");

        eka_vm_free(&vm2);
    }

    /* --- Test 6: csrf token generation --- */
    {
        eka_value_t r = call_method(sess_obj, "csrf", &vm, NULL, 0);
        CHECK(eka_obj_is_type(r, OBJ_STRING) &&
              eka_as_string(r)->length == 32,
              "session.csrf returns 32-char hex string");
    }

    /* --- Test 7: non-string values (int, bool) --- */
    {
        vm.session_id[0] = '\0';
        vm.session_dirty = false;
        vm.session_is_new = true;
        eka_session_load(&vm);

        eka_value_t set_int[] = { eka_string_val(eka_string_new("count", 5)), eka_int(42) };
        eka_value_t set_bool[] = { eka_string_val(eka_string_new("active", 6)), eka_bool(true) };
        call_method(sess_obj, "set", &vm, set_int, 2);
        call_method(sess_obj, "set", &vm, set_bool, 2);

        eka_value_t get_count[] = { eka_string_val(eka_string_new("count", 5)) };
        eka_value_t r1 = call_method(sess_obj, "get", &vm, get_count, 1);
        CHECK(eka_is_int(r1) && eka_as_int(r1) == 42,
              "session stores and retrieves int values");

        eka_value_t get_active[] = { eka_string_val(eka_string_new("active", 6)) };
        eka_value_t r2 = call_method(sess_obj, "get", &vm, get_active, 1);
        CHECK(eka_is_bool(r2) && eka_as_bool(r2) == true,
              "session stores and retrieves bool values");
    }

    /* Clean up: remove test DB */
    remove("eka_sessions.db");
    remove("eka_sessions.db-wal");
    remove("eka_sessions.db-shm");

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

    /* Phase 2 */
    test_math();
    test_slug();
    test_base64();
    test_url();
    test_validate();
    test_fs();
    test_regex();
    test_sitemap();
    test_rss();
    test_i18n();
    test_session();

    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
