/*
 * tests/test_lexer.c — Lexer smoke tests
 */
#include "lexer/lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) do { tests_run++; printf("  %s ... ", name); } while(0)
#define PASS()      do { printf("PASS\n"); } while(0)
#define FAIL(msg)   do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

/* Helper: collect all tokens from a source in CODE mode */
static void lex_all(const char *src, eka_token_type_t *expected, int count,
                    const char *test_name) {
    eka_lexer_t lexer;
    eka_lexer_init(&lexer, src);
    eka_lexer_set_mode(&lexer, LEX_MODE_CODE);

    for (int i = 0; i < count; i++) {
        eka_token_t tok = eka_lexer_next(&lexer);
        if (tok.type != expected[i]) {
            printf("FAIL: %s — token %d: expected %s, got %s (at '%.*s')\n",
                   test_name, i,
                   eka_token_name(expected[i]),
                   eka_token_name(tok.type),
                   (int)tok.length, tok.start);
            tests_failed++;
            return;
        }
    }
    /* Should be EOF */
    eka_token_t tok = eka_lexer_next(&lexer);
    CHECK(tok.type == TOKEN_EOF, "expected EOF after all tokens");
    PASS();
}

/* ================================================================ */

static void test_keywords(void) {
    TEST("keywords");
    eka_token_type_t expected[] = {
        TOKEN_LET, TOKEN_IDENTIFIER, TOKEN_ASSIGN, TOKEN_TRUE, TOKEN_NEWLINE,
        TOKEN_IF, TOKEN_TRUE, TOKEN_RETURN, TOKEN_NULL, TOKEN_NEWLINE,
        TOKEN_FOR, TOKEN_IDENTIFIER, TOKEN_IN, TOKEN_IDENTIFIER, TOKEN_NEWLINE,
        TOKEN_WHILE, TOKEN_FALSE, TOKEN_NEWLINE,
        TOKEN_TRY, TOKEN_IDENTIFIER, TOKEN_CATCH, TOKEN_IDENTIFIER, TOKEN_NEWLINE,
        TOKEN_FUNC, TOKEN_IDENTIFIER, TOKEN_LPAREN, TOKEN_IDENTIFIER, TOKEN_RPAREN, TOKEN_NEWLINE,
        TOKEN_END, TOKEN_NEWLINE,
        TOKEN_CONST, TOKEN_IDENTIFIER, TOKEN_ASSIGN, TOKEN_NUMBER,
    };
    const char *src =
        "let x = true\n"
        "if true return null\n"
        "for item in list\n"
        "while false\n"
        "try risky catch err\n"
        "func greet(name)\n"
        "end\n"
        "const PI = 3.14";
    lex_all(src, expected, sizeof(expected) / sizeof(expected[0]), "keywords");
}

static void test_operators(void) {
    TEST("operators");
    eka_token_type_t expected[] = {
        TOKEN_IDENTIFIER, TOKEN_ASSIGN, TOKEN_IDENTIFIER, TOKEN_PLUS, TOKEN_IDENTIFIER, TOKEN_NEWLINE,
        TOKEN_IDENTIFIER, TOKEN_ASSIGN, TOKEN_IDENTIFIER, TOKEN_EQ, TOKEN_IDENTIFIER, TOKEN_NEWLINE,
        TOKEN_IDENTIFIER, TOKEN_ASSIGN, TOKEN_IDENTIFIER, TOKEN_NEQ, TOKEN_IDENTIFIER, TOKEN_NEWLINE,
        TOKEN_IDENTIFIER, TOKEN_ASSIGN, TOKEN_IDENTIFIER, TOKEN_LT, TOKEN_IDENTIFIER, TOKEN_NEWLINE,
        TOKEN_IDENTIFIER, TOKEN_ASSIGN, TOKEN_IDENTIFIER, TOKEN_LTE, TOKEN_IDENTIFIER, TOKEN_NEWLINE,
        TOKEN_IDENTIFIER, TOKEN_ASSIGN, TOKEN_IDENTIFIER, TOKEN_GT, TOKEN_IDENTIFIER, TOKEN_NEWLINE,
        TOKEN_IDENTIFIER, TOKEN_ASSIGN, TOKEN_IDENTIFIER, TOKEN_GTE, TOKEN_IDENTIFIER, TOKEN_NEWLINE,
        TOKEN_IDENTIFIER, TOKEN_ASSIGN, TOKEN_IDENTIFIER, TOKEN_NULL_COALESCE, TOKEN_IDENTIFIER, TOKEN_NEWLINE,
        TOKEN_IDENTIFIER, TOKEN_ASSIGN, TOKEN_IDENTIFIER, TOKEN_NULL_SAFE, TOKEN_IDENTIFIER, TOKEN_NEWLINE,
        TOKEN_IDENTIFIER, TOKEN_ASSIGN, TOKEN_IDENTIFIER, TOKEN_AND, TOKEN_IDENTIFIER, TOKEN_NEWLINE,
        TOKEN_IDENTIFIER, TOKEN_ASSIGN, TOKEN_IDENTIFIER, TOKEN_OR, TOKEN_IDENTIFIER, TOKEN_NEWLINE,
        TOKEN_IDENTIFIER, TOKEN_ASSIGN, TOKEN_NOT, TOKEN_IDENTIFIER,
    };
    const char *src =
        "a = b + c\n"
        "a = b == c\n"
        "a = b != c\n"
        "a = b < c\n"
        "a = b <= c\n"
        "a = b > c\n"
        "a = b >= c\n"
        "a = b ?? c\n"
        "a = b?.c\n"
        "a = b and c\n"
        "a = b or c\n"
        "a = not b";
    lex_all(src, expected, sizeof(expected) / sizeof(expected[0]), "operators");
}

static void test_strings(void) {
    TEST("string literals");
    eka_token_type_t expected[] = {
        TOKEN_LET, TOKEN_IDENTIFIER, TOKEN_ASSIGN, TOKEN_STRING,
    };
    const char *src = "let name = \"Alice\"";
    eka_lexer_t lexer;
    eka_lexer_init(&lexer, src);
    eka_lexer_set_mode(&lexer, LEX_MODE_CODE);

    eka_token_t t1 = eka_lexer_next(&lexer); CHECK(t1.type == TOKEN_LET, "1: TOKEN_LET");
    eka_token_t t2 = eka_lexer_next(&lexer); CHECK(t2.type == TOKEN_IDENTIFIER, "2: IDENTIFIER");
    eka_token_t t3 = eka_lexer_next(&lexer); CHECK(t3.type == TOKEN_ASSIGN, "3: ASSIGN");
    eka_token_t t4 = eka_lexer_next(&lexer);
    CHECK(t4.type == TOKEN_STRING, "4: TOKEN_STRING");
    CHECK(t4.length == 5, "string length should be 5");
    CHECK(strncmp(t4.start, "Alice", 5) == 0, "string should be 'Alice'");

    eka_token_t t5 = eka_lexer_next(&lexer);
    CHECK(t5.type == TOKEN_EOF, "5: EOF");
    PASS();
}

static void test_numbers(void) {
    TEST("number literals");
    eka_token_type_t expected[] = {
        TOKEN_LET, TOKEN_IDENTIFIER, TOKEN_ASSIGN, TOKEN_NUMBER, TOKEN_NEWLINE,
        TOKEN_LET, TOKEN_IDENTIFIER, TOKEN_ASSIGN, TOKEN_NUMBER, TOKEN_NEWLINE,
        TOKEN_LET, TOKEN_IDENTIFIER, TOKEN_ASSIGN, TOKEN_NUMBER, TOKEN_NEWLINE,
        TOKEN_LET, TOKEN_IDENTIFIER, TOKEN_ASSIGN, TOKEN_MINUS, TOKEN_NUMBER,
    };
    const char *src =
        "let a = 42\n"
        "let b = 3.14\n"
        "let c = 0xFF\n"
        "let d = -5";
    lex_all(src, expected, sizeof(expected) / sizeof(expected[0]), "numbers");
}

static void test_comments(void) {
    TEST("line comments are skipped");
    eka_token_type_t expected[] = {
        TOKEN_LET, TOKEN_IDENTIFIER, TOKEN_ASSIGN, TOKEN_NUMBER, TOKEN_NEWLINE,
        TOKEN_LET, TOKEN_IDENTIFIER, TOKEN_ASSIGN, TOKEN_NUMBER,
    };
    const char *src =
        "let x = 42\n"
        "-- this is a comment\n"
        "let y = 99";
    lex_all(src, expected, sizeof(expected) / sizeof(expected[0]), "comments");
}

static void test_map_literal(void) {
    TEST("map literal braces");
    eka_token_type_t expected[] = {
        TOKEN_LET, TOKEN_IDENTIFIER, TOKEN_ASSIGN,
        TOKEN_LBRACE, TOKEN_IDENTIFIER, TOKEN_COLON, TOKEN_STRING, TOKEN_RBRACE,
    };
    const char *src = "let u = {name: \"Bob\"}";
    lex_all(src, expected, sizeof(expected) / sizeof(expected[0]), "map");
}

/* ================================================================
 * Template mode tests
 * ================================================================ */

static void test_template_text(void) {
    TEST("template text");
    eka_lexer_t lexer;
    eka_lexer_init(&lexer, "<h1>Hello</h1>\n<p>World</p>");
    eka_lexer_set_mode(&lexer, LEX_MODE_TEMPLATE);

    eka_token_t t1 = eka_lexer_next(&lexer);
    CHECK(t1.type == TOKEN_TEXT, "1: TEXT");
    CHECK(strncmp(t1.start, "<h1>Hello</h1>\n<p>World</p>", t1.length) == 0,
          "text should match");

    eka_token_t t2 = eka_lexer_next(&lexer);
    CHECK(t2.type == TOKEN_EOF, "2: EOF");
    PASS();
}

static void test_template_at_if(void) {
    TEST("template @if");
    eka_lexer_t lexer;
    eka_lexer_init(&lexer, "\n  <div>@if user\n  <p>Hi</p>@end");
    eka_lexer_set_mode(&lexer, LEX_MODE_TEMPLATE);

    eka_token_t t1 = eka_lexer_next(&lexer);
    CHECK(t1.type == TOKEN_TEXT, "1: TEXT (whitespace before @if)");

    eka_token_t t2 = eka_lexer_next(&lexer);
    CHECK(t2.type == TOKEN_AT_IF, "2: @if");

    eka_token_t t3 = eka_lexer_next(&lexer);
    CHECK(t3.type == TOKEN_TEXT, "3: TEXT (body)");

    eka_token_t t4 = eka_lexer_next(&lexer);
    CHECK(t4.type == TOKEN_AT_END, "4: @end");
    PASS();
}

static void test_template_interpolation(void) {
    TEST("template {{ }}");
    eka_lexer_t lexer;
    eka_lexer_init(&lexer, "<h1>{{ name }}</h1>");
    eka_lexer_set_mode(&lexer, LEX_MODE_TEMPLATE);

    eka_token_t t1 = eka_lexer_next(&lexer);
    CHECK(t1.type == TOKEN_TEXT, "1: TEXT '<h1>'");

    eka_token_t t2 = eka_lexer_next(&lexer);
    CHECK(t2.type == TOKEN_EXPR_START, "2: EXPR_START");

    /* After {{, the parser switches to CODE mode to parse ' name ' then }} */
    /* We test that }} is recognized */
    eka_lexer_set_mode(&lexer, LEX_MODE_CODE);
    eka_token_t t3 = eka_lexer_next(&lexer);
    CHECK(t3.type == TOKEN_IDENTIFIER, "3: IDENTIFIER 'name'");

    /* Now }} should be recognized */
    eka_token_t t4 = eka_lexer_next(&lexer);
    CHECK(t4.type == TOKEN_EXPR_END, "4: EXPR_END");

    /* Back to template mode for the rest */
    eka_lexer_set_mode(&lexer, LEX_MODE_TEMPLATE);
    eka_token_t t5 = eka_lexer_next(&lexer);
    CHECK(t5.type == TOKEN_TEXT, "5: TEXT '</h1>'");

    eka_token_t t6 = eka_lexer_next(&lexer);
    CHECK(t6.type == TOKEN_EOF, "6: EOF");
    PASS();
}

static void test_template_at_for_else(void) {
    TEST("template @for/@else");
    eka_lexer_t lexer;
    eka_lexer_init(&lexer, "<ul>@for post in posts<li>{{ post }}</li>@else<p>none</p>@end</ul>");
    eka_lexer_set_mode(&lexer, LEX_MODE_TEMPLATE);

    eka_token_t t1 = eka_lexer_next(&lexer);
    CHECK(t1.type == TOKEN_TEXT, "1: TEXT '<ul>'");

    eka_token_t t2 = eka_lexer_next(&lexer);
    CHECK(t2.type == TOKEN_AT_FOR, "2: @for");

    eka_token_t t3 = eka_lexer_next(&lexer);
    CHECK(t3.type == TOKEN_TEXT, "3: TEXT '<li>'");

    eka_token_t t4 = eka_lexer_next(&lexer);
    CHECK(t4.type == TOKEN_EXPR_START, "4: EXPR_START");

    /* Switch to code mode for the expression */
    eka_lexer_set_mode(&lexer, LEX_MODE_CODE);
    eka_token_t t5 = eka_lexer_next(&lexer);
    CHECK(t5.type == TOKEN_IDENTIFIER, "5: IDENTIFIER 'post'");

    eka_token_t t6 = eka_lexer_next(&lexer);
    CHECK(t6.type == TOKEN_EXPR_END, "6: EXPR_END");

    /* Back to template */
    eka_lexer_set_mode(&lexer, LEX_MODE_TEMPLATE);
    eka_token_t t7 = eka_lexer_next(&lexer);
    CHECK(t7.type == TOKEN_TEXT, "7: TEXT '</li>'");

    eka_token_t t8 = eka_lexer_next(&lexer);
    CHECK(t8.type == TOKEN_AT_ELSE, "8: @else");

    eka_token_t t9 = eka_lexer_next(&lexer);
    CHECK(t9.type == TOKEN_TEXT, "9: TEXT '<p>none</p>'");

    eka_token_t t10 = eka_lexer_next(&lexer);
    CHECK(t10.type == TOKEN_AT_END, "10: @end");

    eka_token_t t11 = eka_lexer_next(&lexer);
    CHECK(t11.type == TOKEN_TEXT, "11: TEXT '</ul>'");
    PASS();
}

static void test_method_blocks(void) {
    TEST("method blocks in code mode");
    eka_token_type_t expected[] = {
        TOKEN_AT_GET, TOKEN_SLASH, TOKEN_NEWLINE,
        TOKEN_AT_POST, TOKEN_SLASH, TOKEN_IDENTIFIER, TOKEN_SLASH, TOKEN_IDENTIFIER, TOKEN_SLASH, TOKEN_LBRACKET, TOKEN_IDENTIFIER, TOKEN_RBRACKET,
    };
    const char *src =
        "@get /\n"
        "@post /api/users/[id]";
    lex_all(src, expected, sizeof(expected) / sizeof(expected[0]), "method blocks");
}

int main(void) {
    printf("lexer tests:\n");
    test_keywords();
    test_operators();
    test_strings();
    test_numbers();
    test_comments();
    test_map_literal();
    test_template_text();
    test_template_at_if();
    test_template_interpolation();
    test_template_at_for_else();
    test_method_blocks();

    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
