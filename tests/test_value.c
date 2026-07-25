/*
 * tests/test_value.c — NaN-boxing value representation smoke tests
 */
#include "core/value.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) do { tests_run++; printf("  %s ... ", name); } while(0)
#define PASS()      do { printf("PASS\n"); } while(0)
#define FAIL(msg)   do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

static void test_nil(void) {
    TEST("nil round-trip");
    eka_value_t v = eka_nil();
    CHECK(eka_is_nil(v), "nil should be nil");
    CHECK(!eka_is_bool(v), "nil should not be bool");
    CHECK(!eka_is_number(v), "nil should not be number");
    CHECK(!eka_is_int(v), "nil should not be int");
    PASS();
}

static void test_bool(void) {
    TEST("bool round-trip");
    eka_value_t t = eka_bool(true);
    eka_value_t f = eka_bool(false);
    CHECK(eka_is_bool(t) && eka_as_bool(t) == true, "true round-trip");
    CHECK(eka_is_bool(f) && eka_as_bool(f) == false, "false round-trip");
    CHECK(t != f, "true != false");
    PASS();
}

static void test_number(void) {
    TEST("number round-trip");
    double pi = 3.141592653589793;
    eka_value_t v = eka_number(pi);
    CHECK(eka_is_number(v), "should be number");
    CHECK(!eka_is_obj(v), "number should not be obj");
    CHECK(fabs(eka_as_number(v) - pi) < 1e-15, "pi round-trip precision");
    PASS();
}

static void test_int(void) {
    TEST("small int round-trip");
    eka_value_t v = eka_int(42);
    CHECK(eka_is_int(v), "should be int");
    CHECK(eka_as_int(v) == 42, "42 round-trip");

    eka_value_t neg = eka_int(-1);
    CHECK(eka_is_int(neg), "should be int");
    CHECK(eka_as_int(neg) == -1, "-1 round-trip");

    eka_value_t zero = eka_int(0);
    CHECK(eka_is_int(zero), "0 should be int");
    CHECK(eka_as_int(zero) == 0, "0 round-trip");
    PASS();
}

static void test_number_zero(void) {
    TEST("zero as double (not int)");
    eka_value_t v = eka_number(0.0);
    CHECK(eka_is_number(v), "0.0 should be number");
    CHECK(eka_as_number(v) == 0.0, "0.0 round-trip");
    CHECK(!eka_is_int(v), "0.0 should NOT be int");
    PASS();
}

int main(void) {
    printf("value tests:\n");
    test_nil();
    test_bool();
    test_number();
    test_int();
    test_number_zero();
    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
