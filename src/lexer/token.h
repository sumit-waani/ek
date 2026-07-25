#ifndef TOKEN_H
#define TOKEN_H

#include <stdint.h>
#include <stddef.h>

/*
 * Eka token types.
 *
 * The lexer operates in two modes:
 *   CODE mode     — produces programming language tokens
 *   TEMPLATE mode — produces TOKEN_TEXT for raw HTML, switching
 *                   to CODE for @if/@for/@do/@get/@post/{{ etc.
 */

typedef enum {
    /* --- Special --- */
    TOKEN_EOF,
    TOKEN_ERROR,
    TOKEN_NEWLINE,     /* significant in some contexts */

    /* --- Template --- */
    TOKEN_TEXT,        /* raw HTML text (only in template mode) */

    /* --- Keywords --- */
    TOKEN_LET,
    TOKEN_CONST,
    TOKEN_FUNC,
    TOKEN_END,
    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_FOR,
    TOKEN_IN,
    TOKEN_WHILE,
    TOKEN_TRY,
    TOKEN_CATCH,
    TOKEN_RETURN,
    TOKEN_TRUE,
    TOKEN_FALSE,
    TOKEN_NULL,
    TOKEN_AND,
    TOKEN_OR,
    TOKEN_NOT,

    /* --- Method blocks --- */
    TOKEN_AT_GET,
    TOKEN_AT_POST,
    TOKEN_AT_PUT,
    TOKEN_AT_DELETE,
    TOKEN_AT_PATCH,

    /* --- Template control --- */
    TOKEN_AT_IF,
    TOKEN_AT_FOR,
    TOKEN_AT_DO,
    TOKEN_AT_ELSE,
    TOKEN_AT_END,
    TOKEN_AT_CSRF,     /* @csrf off */

    /* --- Expression interpolation --- */
    TOKEN_EXPR_START,  /* {{ */
    TOKEN_EXPR_END,    /* }} */

    /* --- Literals --- */
    TOKEN_IDENTIFIER,
    TOKEN_STRING,
    TOKEN_NUMBER,

    /* --- Operators and punctuation --- */
    TOKEN_PLUS,          /* + */
    TOKEN_MINUS,         /* - */
    TOKEN_STAR,          /* * */
    TOKEN_SLASH,         /* / */
    TOKEN_PERCENT,       /* % */
    TOKEN_EQ,            /* == */
    TOKEN_NEQ,           /* != */
    TOKEN_APPROX_EQ,     /* ~= */
    TOKEN_GT,            /* > */
    TOKEN_GTE,           /* >= */
    TOKEN_LT,            /* < */
    TOKEN_LTE,           /* <= */
    TOKEN_ASSIGN,        /* = */
    TOKEN_NULL_COALESCE, /* ?? */
    TOKEN_NULL_SAFE,     /* ?. */
    TOKEN_DOT,           /* . */
    TOKEN_COMMA,         /* , */
    TOKEN_COLON,         /* : */
    TOKEN_SEMICOLON,     /* ; */

    /* --- Brackets --- */
    TOKEN_LPAREN,        /* ( */
    TOKEN_RPAREN,        /* ) */
    TOKEN_LBRACKET,      /* [ */
    TOKEN_RBRACKET,      /* ] */
    TOKEN_LBRACE,        /* { */
    TOKEN_RBRACE,        /* } */

    TOKEN_COUNT
} eka_token_type_t;

/* Token with source location */
typedef struct {
    eka_token_type_t type;
    const char      *start;      /* pointer into source */
    size_t           length;     /* byte length of token */
    uint32_t         line;       /* 1-based line number */
    uint32_t         column;     /* 1-based column */
} eka_token_t;

/* Token names for debugging */
const char *eka_token_name(eka_token_type_t type);

#endif /* TOKEN_H */
