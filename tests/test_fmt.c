#define _POSIX_C_SOURCE 200809L
#include "fmt/fmt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); \
    } else { \
        tests_passed++; \
    } \
} while(0)

/* ================================================================
 * Test helpers
 * ================================================================ */

/* Format a string and return the result (caller frees).
 * Returns NULL on error. */
static char *fmt_string(const char *source) {
    /* Write source to a temp file, format it, read back */
    char tmppath[] = "/tmp/eka_fmt_test_XXXXXX";
    int fd = mkstemp(tmppath);
    if (fd < 0) return NULL;

    write(fd, source, strlen(source));
    close(fd);

    int result = eka_fmt(tmppath, false);
    if (result != 0) {
        unlink(tmppath);
        return NULL;
    }

    FILE *f = fopen(tmppath, "rb");
    if (!f) { unlink(tmppath); return NULL; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *out = malloc((size_t)size + 1);
    if (!out) { fclose(f); unlink(tmppath); return NULL; }
    size_t n = fread(out, 1, (size_t)size, f);
    fclose(f);
    out[n] = '\0';
    unlink(tmppath);
    return out;
}

/* ================================================================
 * Tests
 * ================================================================ */

static void test_basic_indentation(void) {
    printf("test_basic_indentation...\n");

    char *result = fmt_string(
        "@get /\n"
        "<h1>Hello</h1>\n"
        "@end\n"
    );
    CHECK(result != NULL, "should format without error");
    if (result) {
        CHECK(strstr(result, "  <h1>Hello</h1>") != NULL,
              "template content should be indented inside method block");
        free(result);
    }
}

static void test_nested_indentation(void) {
    printf("test_nested_indentation...\n");

    char *result = fmt_string(
        "@get /\n"
        "@if show\n"
        "<p>Visible</p>\n"
        "@end\n"
        "@end\n"
    );
    CHECK(result != NULL, "should format without error");
    if (result) {
        CHECK(strstr(result, "    <p>Visible</p>") != NULL,
              "content inside @if should be double-indented");
        free(result);
    }
}

static void test_trailing_whitespace(void) {
    printf("test_trailing_whitespace...\n");

    char *result = fmt_string("let x = 1   \n");
    CHECK(result != NULL, "should format without error");
    if (result) {
        CHECK(strcmp(result, "let x = 1\n") == 0,
              "trailing whitespace should be stripped");
        free(result);
    }
}

static void test_blank_line_normalization(void) {
    printf("test_blank_line_normalization...\n");

    char *result = fmt_string(
        "let a = 1\n"
        "\n"
        "\n"
        "\n"
        "let b = 2\n"
    );
    CHECK(result != NULL, "should format without error");
    if (result) {
        /* Should have at most 1 consecutive blank line */
        CHECK(strstr(result, "\n\n\n") == NULL,
              "should not have more than 1 consecutive blank line");
        free(result);
    }
}

static void test_idempotent(void) {
    printf("test_idempotent...\n");

    const char *source =
        "let db = sqlite.open(\"app.db\")\n"
        "\n"
        "@get /\n"
        "  <h1>Hello</h1>\n"
        "@end\n";

    char *first = fmt_string(source);
    CHECK(first != NULL, "first format should succeed");

    if (first) {
        char *second = fmt_string(first);
        CHECK(second != NULL, "second format should succeed");
        if (second) {
            CHECK(strcmp(first, second) == 0,
                  "formatting should be idempotent");
            free(second);
        }
        free(first);
    }
}

static void test_check_mode_clean(void) {
    printf("test_check_mode_clean...\n");

    /* Write a well-formatted file and check it */
    char tmppath[] = "/tmp/eka_fmt_check_XXXXXX";
    int fd = mkstemp(tmppath);
    if (fd < 0) return;

    const char *source = "let x = 1\n";
    write(fd, source, strlen(source));
    close(fd);

    int result = eka_fmt(tmppath, true);
    CHECK(result == 0, "check on clean file should return 0");
    unlink(tmppath);
}

static void test_check_mode_dirty(void) {
    printf("test_check_mode_dirty...\n");

    /* Write an unformatted file and check it */
    char tmppath[] = "/tmp/eka_fmt_dirty_XXXXXX";
    int fd = mkstemp(tmppath);
    if (fd < 0) return;

    const char *source = "let x = 1   \n";  /* trailing whitespace */
    write(fd, source, strlen(source));
    close(fd);

    int result = eka_fmt(tmppath, true);
    CHECK(result == 1, "check on dirty file should return 1");
    unlink(tmppath);
}

static void test_code_mode_functions(void) {
    printf("test_code_mode_functions...\n");

    char *result = fmt_string(
        "func greet(name)\n"
        "\"Hello\"\n"
        "end\n"
    );
    CHECK(result != NULL, "should format without error");
    if (result) {
        CHECK(strstr(result, "  \"Hello\"") != NULL,
              "function body should be indented");
        free(result);
    }
}

static void test_else_handling(void) {
    printf("test_else_handling...\n");

    char *result = fmt_string(
        "if cond\n"
        "\"yes\"\n"
        "else\n"
        "\"no\"\n"
        "end\n"
    );
    CHECK(result != NULL, "should format without error");
    if (result) {
        /* else should be at same level as if */
        CHECK(strstr(result, "else\n") != NULL,
              "else should be present");
        free(result);
    }
}

static void test_raw_passthrough(void) {
    printf("test_raw_passthrough...\n");

    char *result = fmt_string(
        "@get /\n"
        "<script>\n"
        "  var x = 1;\n"
        "    var y = 2;\n"
        "</script>\n"
        "@end\n"
    );
    CHECK(result != NULL, "should format without error");
    if (result) {
        /* Script content should be preserved as-is */
        CHECK(strstr(result, "  var x = 1;") != NULL,
              "script content should be preserved");
        CHECK(strstr(result, "    var y = 2;") != NULL,
              "script content indentation should be preserved");
        free(result);
    }
}

static void test_full_eka_file(void) {
    printf("test_full_eka_file...\n");

    char *result = fmt_string(
        "let db = sqlite.open(\"app.db\")\n"
        "db.exec(\"CREATE TABLE IF NOT EXISTS todos (id INTEGER PRIMARY KEY, text TEXT)\")\n"
        "\n"
        "@get /\n"
        "<h1>Todos</h1>\n"
        "@do\n"
        "let todos = db.query(\"SELECT * FROM todos ORDER BY id DESC\")\n"
        "@end\n"
        "@for t in todos\n"
        "<p>{{ t.text }}</p>\n"
        "@end\n"
        "@end\n"
    );
    CHECK(result != NULL, "should format full .eka file");
    if (result) {
        /* Method block header at indent 0 */
        CHECK(strstr(result, "@get /") != NULL, "method block should be present");
        /* Template content indented */
        CHECK(strstr(result, "  <h1>Todos</h1>") != NULL,
              "template content should be indented");
        /* @do content further indented */
        CHECK(strstr(result, "    let todos") != NULL,
              "@do body should be double-indented");
        /* @for content indented */
        CHECK(strstr(result, "    <p>") != NULL,
              "@for body should be double-indented");
        free(result);
    }
}

/* ================================================================
 * Main
 * ================================================================ */

int main(void) {
    printf("=== fmt tests ===\n");

    test_basic_indentation();
    test_nested_indentation();
    test_trailing_whitespace();
    test_blank_line_normalization();
    test_idempotent();
    test_check_mode_clean();
    test_check_mode_dirty();
    test_code_mode_functions();
    test_else_handling();
    test_raw_passthrough();
    test_full_eka_file();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
