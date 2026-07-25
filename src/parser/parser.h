#ifndef PARSER_H
#define PARSER_H

#include "parser/ast.h"
#include "lexer/lexer.h"

#include <stdbool.h>

typedef struct {
    eka_lexer_t  lexer;
    eka_token_t  current;
    eka_token_t  previous;
    bool         had_error;
    bool         panic_mode;
    const char  *error_message;
} eka_parser_t;

/* Initialise parser with source string */
void eka_parser_init(eka_parser_t *parser, const char *source);

/* Parse the entire program. Returns AST_PROGRAM node or NULL on error. */
ast_node_t *eka_parse(eka_parser_t *parser);

/* Get error message (if any) */
const char *eka_parser_error(const eka_parser_t *parser);

#endif /* PARSER_H */
