/*
 * tests/test_obj.c — Object allocation, GC, string interning, list/map ops
 */
#include "core/obj.h"
#include "core/vm.h"  /* for eka_vm_init (arena allocation needs a VM) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) do { tests_run++; printf("  %s ... ", name); } while(0)
#define PASS()      do { printf("PASS\n"); } while(0)
#define FAIL(msg)   do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

/* The GC allocator needs an active VM. Set one up for all tests. */
static eka_vm_t test_vm;

/* --- String tests --- */

static void test_string_alloc(void) {
    TEST("string allocation");
    eka_string_t *s = eka_string_new("hello", 5);
    CHECK(s != NULL, "string should not be null");
    CHECK(s->header.type == OBJ_STRING, "type should be OBJ_STRING");
    CHECK(s->length == 5, "length should be 5");
    CHECK(strcmp(s->data, "hello") == 0, "data should be 'hello'");
    CHECK(s->data[5] == '\0', "should be null-terminated");
    PASS();
}

static void test_string_interning(void) {
    TEST("string interning");
    eka_string_t *a = eka_string_new("interning", 9);
    eka_string_t *b = eka_string_new("interning", 9);
    CHECK(a == b, "same content should return same pointer");
    CHECK(a == eka_string_intern("interning", 9), "eka_string_intern should also intern");

    /* Different strings */
    eka_string_t *c = eka_string_new("different", 9);
    CHECK(a != c, "different content should give different pointers");
    PASS();
}

static void test_string_hash(void) {
    TEST("string hash consistency");
    eka_string_t *s = eka_string_new("hashme", 6);
    uint32_t h1 = eka_string_hash(s);
    uint32_t h2 = eka_string_hash(s);
    CHECK(h1 == h2, "hash should be idempotent");
    CHECK(h1 != 0, "hash should not be zero");
    PASS();
}

static void test_string_empty(void) {
    TEST("empty string");
    eka_string_t *s = eka_string_new("", 0);
    CHECK(s != NULL, "empty string should not be null");
    CHECK(s->length == 0, "length should be 0");
    CHECK(s->data[0] == '\0', "data should be empty");
    PASS();
}

/* --- Value wrapping --- */

static void test_value_wrapping(void) {
    TEST("value wrapping");
    eka_string_t *s = eka_string_new("wrap", 4);
    eka_value_t v = eka_string_val(s);

    CHECK(eka_is_obj(v), "string value should be obj");
    CHECK(eka_obj_is_type(v, OBJ_STRING), "should be OBJ_STRING");
    CHECK(!eka_is_nil(v), "should not be nil");
    CHECK(!eka_is_int(v), "should not be int");
    CHECK(!eka_is_number(v), "should not be number");

    eka_string_t *back = eka_as_string(v);
    CHECK(back == s, "round-trip should preserve pointer");
    PASS();
}

/* --- List tests --- */

static void test_list_basic(void) {
    TEST("list basic operations");
    eka_list_t *list = eka_list_new(4);
    CHECK(list != NULL, "list should not be null");
    CHECK(list->length == 0, "initial length should be 0");
    CHECK(list->capacity >= 4, "capacity should be >= 4");

    eka_list_push(list, eka_int(10));
    eka_list_push(list, eka_int(20));
    eka_list_push(list, eka_int(30));
    CHECK(list->length == 3, "length should be 3 after pushes");
    CHECK(eka_as_int(list->items[0]) == 10, "item[0] should be 10");
    CHECK(eka_as_int(list->items[1]) == 20, "item[1] should be 20");
    CHECK(eka_as_int(list->items[2]) == 30, "item[2] should be 30");
    PASS();
}

static void test_list_pop(void) {
    TEST("list pop");
    eka_list_t *list = eka_list_new(4);
    eka_list_push(list, eka_int(1));
    eka_list_push(list, eka_int(2));

    eka_value_t popped = eka_list_pop(list);
    CHECK(eka_as_int(popped) == 2, "pop should return last element");
    CHECK(list->length == 1, "length should decrease");
    CHECK(eka_as_int(list->items[0]) == 1, "remaining item should be 1");
    PASS();
}

static void test_list_pop_empty(void) {
    TEST("list pop empty");
    eka_list_t *list = eka_list_new(4);
    eka_value_t v = eka_list_pop(list);
    CHECK(eka_is_nil(v), "pop on empty list should return nil");
    PASS();
}

static void test_list_insert(void) {
    TEST("list insert");
    eka_list_t *list = eka_list_new(4);
    eka_list_push(list, eka_int(1));
    eka_list_push(list, eka_int(3));
    eka_list_insert(list, 1, eka_int(2));
    CHECK(list->length == 3, "length should be 3");
    CHECK(eka_as_int(list->items[0]) == 1, "[0]=1");
    CHECK(eka_as_int(list->items[1]) == 2, "[1]=2");
    CHECK(eka_as_int(list->items[2]) == 3, "[2]=3");
    PASS();
}

static void test_list_remove_at(void) {
    TEST("list remove_at");
    eka_list_t *list = eka_list_new(4);
    eka_list_push(list, eka_int(1));
    eka_list_push(list, eka_int(2));
    eka_list_push(list, eka_int(3));
    eka_list_remove_at(list, 1);
    CHECK(list->length == 2, "length should be 2");
    CHECK(eka_as_int(list->items[0]) == 1, "[0]=1");
    CHECK(eka_as_int(list->items[1]) == 3, "[1]=3");
    PASS();
}

/* --- Map tests --- */

static void test_map_basic(void) {
    TEST("map basic operations");
    eka_map_t *map = eka_map_new(8);
    CHECK(map != NULL, "map should not be null");
    CHECK(map->length == 0, "initial length should be 0");

    eka_string_t *key_name = eka_string_new("name", 4);
    eka_string_t *key_age  = eka_string_new("age", 3);

    eka_map_set(map, key_name, eka_string_val(eka_string_new("Alice", 5)));
    eka_map_set(map, key_age, eka_int(30));
    CHECK(map->length == 2, "length should be 2 after inserts");

    CHECK(eka_map_has(map, key_name), "should have 'name'");
    CHECK(eka_map_has(map, key_age), "should have 'age'");

    eka_value_t name_val = eka_map_get(map, key_name);
    CHECK(eka_obj_is_type(name_val, OBJ_STRING), "name should be a string");
    CHECK(strcmp(eka_as_string(name_val)->data, "Alice") == 0, "name should be Alice");

    eka_value_t age_val = eka_map_get(map, key_age);
    CHECK(eka_is_int(age_val), "age should be int");
    CHECK(eka_as_int(age_val) == 30, "age should be 30");
    PASS();
}

static void test_map_missing_key(void) {
    TEST("map missing key");
    eka_map_t *map = eka_map_new(8);
    eka_string_t *key = eka_string_new("missing", 7);
    CHECK(!eka_map_has(map, key), "should not have missing key");
    CHECK(eka_is_nil(eka_map_get(map, key)), "missing key should return nil");
    PASS();
}

static void test_map_overwrite(void) {
    TEST("map overwrite");
    eka_map_t *map = eka_map_new(8);
    eka_string_t *key = eka_string_new("x", 1);
    eka_map_set(map, key, eka_int(1));
    eka_map_set(map, key, eka_int(2));
    CHECK(map->length == 1, "length should still be 1 after overwrite");
    CHECK(eka_as_int(eka_map_get(map, key)) == 2, "value should be updated to 2");
    PASS();
}

static void test_map_delete(void) {
    TEST("map delete");
    eka_map_t *map = eka_map_new(8);
    eka_string_t *key = eka_string_new("del", 3);
    eka_map_set(map, key, eka_int(42));
    CHECK(eka_map_has(map, key), "should have key before delete");
    eka_map_delete(map, key);
    CHECK(!eka_map_has(map, key), "should not have key after delete");
    CHECK(eka_is_nil(eka_map_get(map, key)), "get after delete should return nil");
    PASS();
}

/* --- List grow stress test --- */

static void test_list_grow(void) {
    TEST("list grow (push 1000 items, capacity starts at 4)");
    eka_list_t *list = eka_list_new(4);
    CHECK(list->capacity == 4, "initial capacity should be 4");

    /* Push 1000 items — triggers multiple grows */
    for (int i = 0; i < 1000; i++) {
        eka_list_push(list, eka_int(i));
    }

    CHECK(list->length == 1000, "length should be 1000");
    CHECK(list->capacity >= 1000, "capacity should be >= 1000");

    /* Verify every single item */
    bool all_correct = true;
    for (int i = 0; i < 1000; i++) {
        if (!eka_is_int(list->items[i]) || eka_as_int(list->items[i]) != i) {
            all_correct = false;
            break;
        }
    }
    CHECK(all_correct, "all 1000 items must be correct after multiple grows");
    PASS();
}

/* --- Map grow stress test --- */

static void test_map_grow(void) {
    TEST("map grow (insert 500 entries, capacity starts at 8)");
    eka_map_t *map = eka_map_new(8);
    CHECK(map->capacity == 8, "initial capacity should be 8");

    /* Create 500 unique keys and insert them */
    char key_buf[32];
    eka_string_t *keys[500];
    for (int i = 0; i < 500; i++) {
        int len = snprintf(key_buf, sizeof(key_buf), "key_%d", i);
        keys[i] = eka_string_intern(key_buf, (size_t)len);
        eka_map_set(map, keys[i], eka_int(i * 10));
    }

    CHECK(map->length == 500, "length should be 500");

    /* Verify every single entry */
    bool all_correct = true;
    for (int i = 0; i < 500; i++) {
        eka_value_t v = eka_map_get(map, keys[i]);
        if (!eka_is_int(v) || eka_as_int(v) != i * 10) {
            all_correct = false;
            break;
        }
    }
    CHECK(all_correct, "all 500 entries must be retrievable after multiple grows");
    PASS();
}

/* --- GC smoke test --- */

static void test_gc_no_crash(void) {
    TEST("GC basic cycle");
    /* Allocate a bunch of objects, trigger GC */
    for (int i = 0; i < 1000; i++) {
        eka_string_t *s = eka_string_new("gc-test", 7);
        (void)s;
    }
    /* Should not crash */
    PASS();
}

int main(void) {
    printf("object / GC tests:\n");
    eka_vm_init(&test_vm);  /* required for arena allocation */
    test_string_alloc();
    test_string_interning();
    test_string_hash();
    test_string_empty();
    test_value_wrapping();
    test_list_basic();
    test_list_pop();
    test_list_pop_empty();
    test_list_insert();
    test_list_remove_at();
    test_list_grow();
    test_map_basic();
    test_map_missing_key();
    test_map_overwrite();
    test_map_delete();
    test_map_grow();
    test_gc_no_crash();

    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
