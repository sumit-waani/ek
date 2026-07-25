#ifndef LEXER_H
#define LEXER_H

#include "lexer/token.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    LEX_MODE_CODE,
    LEX_MODE_TEMPLATE,
} eka_lexer_mode_t;

typedef struct {
    const char      *source;       /* entire source string */
    const char      *start;        /* start of current token being lexed */
    const char      *current;      /* current position */
    const char      *line_start;   /* start of current line */
    uint32_t         line;         /* 1-based */
    eka_lexer_mode_t mode;
    eka_token_t      current_token;
    bool             had_error;
} eka_lexer_t;

/* Initialise lexer with source. Starts in CODE mode. */
void eka_lexer_init(eka_lexer_t *lexer, const char *source);

/* Set lexer mode (CODE or TEMPLATE). */
void eka_lexer_set_mode(eka_lexer_t *lexer, eka_lexer_mode_t mode);

/* Return the next token. Updates lexer->current_token. */
eka_token_t eka_lexer_next(eka_lexer_t *lexer);

/* Peek at the current token without consuming. */
eka_token_t eka_lexer_current(const eka_lexer_t *lexer);

#endif /* LEXER_H */
