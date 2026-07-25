#include "lexer/lexer.h"

#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* ================================================================
 * Helpers
 * ================================================================ */

static inline bool is_at_end(const eka_lexer_t *lexer) {
    return *lexer->current == '\0';
}

static inline char peek(const eka_lexer_t *lexer) {
    return *lexer->current;
}

static inline char peek_next(const eka_lexer_t *lexer) {
    if (is_at_end(lexer)) return '\0';
    return lexer->current[1];
}

static inline char advance(eka_lexer_t *lexer) {
    return *lexer->current++;
}

static inline bool match(eka_lexer_t *lexer, char expected) {
    if (is_at_end(lexer) || *lexer->current != expected) return false;
    lexer->current++;
    return true;
}

static void skip_whitespace(eka_lexer_t *lexer) {
    for (;;) {
        switch (peek(lexer)) {
        case ' ':
        case '\t':
        case '\r':
            advance(lexer);
            break;
        case '\n':
            /* Don't skip newlines — they may be significant.
             * But we emit them as TOKEN_NEWLINE so the parser can use them. */
            return;
        default:
            return;
        }
    }
}

/* ================================================================
 * Token creation
 * ================================================================ */

static eka_token_t make_token(eka_lexer_t *lexer, eka_token_type_t type) {
    eka_token_t token;
    token.type   = type;
    token.start  = lexer->start;
    token.length = (int)(lexer->current - lexer->start);
    token.line   = lexer->line;
    token.column = (uint32_t)(lexer->start - lexer->line_start) + 1;
    return token;
}

static eka_token_t make_token_sized(eka_lexer_t *lexer, eka_token_type_t type,
                                     const char *start, size_t length) {
    eka_token_t token;
    token.type   = type;
    token.start  = start;
    token.length = length;
    token.line   = lexer->line;
    token.column = (uint32_t)(start - lexer->line_start) + 1;
    return token;
}

static eka_token_t error_token(eka_lexer_t *lexer, const char *message) {
    eka_token_t token;
    token.type   = TOKEN_ERROR;
    token.start  = message;
    token.length = strlen(message);
    token.line   = lexer->line;
    token.column = (uint32_t)(lexer->current - lexer->line_start) + 1;
    lexer->had_error = true;
    return token;
}

/* ================================================================
 * Keyword lookup (trie or linear search)
 * ================================================================ */

typedef struct {
    const char     *name;
    eka_token_type_t type;
    size_t           length;
} keyword_entry_t;

static const keyword_entry_t keywords[] = {
    {"let",    TOKEN_LET,    3},
    {"const",  TOKEN_CONST,  5},
    {"func",   TOKEN_FUNC,   4},
    {"end",    TOKEN_END,    3},
    {"if",     TOKEN_IF,     2},
    {"else",   TOKEN_ELSE,   4},
    {"for",    TOKEN_FOR,    3},
    {"in",     TOKEN_IN,     2},
    {"while",  TOKEN_WHILE,  5},
    {"try",    TOKEN_TRY,    3},
    {"catch",  TOKEN_CATCH,  5},
    {"return", TOKEN_RETURN, 6},
    {"true",   TOKEN_TRUE,   4},
    {"false",  TOKEN_FALSE,  5},
    {"null",   TOKEN_NULL,   4},
    {"and",    TOKEN_AND,    3},
    {"or",     TOKEN_OR,     2},
    {"not",    TOKEN_NOT,    3},
    {NULL, 0, 0},
};

static eka_token_type_t lookup_keyword(const char *word, size_t length) {
    for (const keyword_entry_t *k = keywords; k->name; k++) {
        if (k->length == length && memcmp(k->name, word, length) == 0) {
            return k->type;
        }
    }
    return TOKEN_IDENTIFIER;
}

/* ================================================================
 * @keyword lookup for template mode
 * ================================================================ */

typedef struct {
    const char     *name;
    eka_token_type_t type;
    size_t           length;
} at_keyword_entry_t;

static const at_keyword_entry_t at_keywords[] = {
    {"get",    TOKEN_AT_GET,    3},
    {"post",   TOKEN_AT_POST,   4},
    {"put",    TOKEN_AT_PUT,    3},
    {"delete", TOKEN_AT_DELETE, 6},
    {"patch",  TOKEN_AT_PATCH,  5},
    {"if",     TOKEN_AT_IF,     2},
    {"for",    TOKEN_AT_FOR,    3},
    {"do",     TOKEN_AT_DO,     2},
    {"else",   TOKEN_AT_ELSE,   4},
    {"end",    TOKEN_AT_END,    3},
    {"csrf",   TOKEN_AT_CSRF,   4},
    {NULL, 0, 0},
};

static eka_token_type_t lookup_at_keyword(const char *word, size_t length) {
    for (const at_keyword_entry_t *k = at_keywords; k->name; k++) {
        if (k->length == length && memcmp(k->name, word, length) == 0) {
            return k->type;
        }
    }
    return TOKEN_ERROR;  /* unknown @keyword */
}

/* ================================================================
 * Code mode lexing
 * ================================================================ */

static eka_token_t lex_string(eka_lexer_t *lexer) {
    /* We've consumed the opening " */
    const char *start = lexer->current;

    while (!is_at_end(lexer) && peek(lexer) != '"') {
        if (peek(lexer) == '\\' && peek_next(lexer) != '\0') {
            advance(lexer);  /* skip the backslash */
        }
        if (peek(lexer) == '\n') lexer->line++;
        advance(lexer);
    }

    if (is_at_end(lexer)) {
        return error_token(lexer, "unterminated string");
    }

    /* Consume closing " */
    advance(lexer);

    /* Length excludes the quotes */
    size_t length = (size_t)(lexer->current - start - 1);
    return make_token_sized(lexer, TOKEN_STRING, start, length);
}

static eka_token_t lex_number(eka_lexer_t *lexer) {
    const char *start = lexer->current - 1;  /* first digit already consumed */

    /* Hex prefix */
    if (peek(lexer) == 'x' || peek(lexer) == 'X') {
        advance(lexer);
        while (isxdigit((unsigned char)peek(lexer))) advance(lexer);
        goto done;
    }

    /* Integer part */
    while (isdigit((unsigned char)peek(lexer))) advance(lexer);

    /* Fractional part */
    if (peek(lexer) == '.' && isdigit((unsigned char)peek_next(lexer))) {
        advance(lexer);  /* dot */
        while (isdigit((unsigned char)peek(lexer))) advance(lexer);
    }

    /* Exponent */
    if (peek(lexer) == 'e' || peek(lexer) == 'E') {
        advance(lexer);
        if (peek(lexer) == '+' || peek(lexer) == '-') advance(lexer);
        while (isdigit((unsigned char)peek(lexer))) advance(lexer);
    }

done:
    /* length calculation after the label needs a statement first */
    (void)0;
    size_t length = (size_t)(lexer->current - start);
    return make_token_sized(lexer, TOKEN_NUMBER, start, length);
}

static eka_token_t lex_identifier(eka_lexer_t *lexer) {
    const char *start = lexer->current - 1;  /* first char already consumed */

    while (isalnum((unsigned char)peek(lexer)) || peek(lexer) == '_') {
        advance(lexer);
    }

    size_t length = (size_t)(lexer->current - start);
    eka_token_type_t type = lookup_keyword(start, length);

    if (type == TOKEN_IDENTIFIER) {
        return make_token_sized(lexer, TOKEN_IDENTIFIER, start, length);
    }
    return make_token_sized(lexer, type, start, length);
}

static eka_token_t lex_code_token(eka_lexer_t *lexer) {
    skip_whitespace(lexer);

    if (is_at_end(lexer)) {
        return make_token(lexer, TOKEN_EOF);
    }

    /* Mark start of token */
    lexer->start = lexer->current;

    /* Newline */
    if (peek(lexer) == '\n') {
        advance(lexer);
        eka_token_t tok = make_token(lexer, TOKEN_NEWLINE);
        lexer->line++;
        lexer->line_start = lexer->current;
        return tok;
    }

    /* Line comment */
    if (peek(lexer) == '-' && peek_next(lexer) == '-') {
        advance(lexer); advance(lexer);  /* skip -- */
        while (!is_at_end(lexer) && peek(lexer) != '\n') {
            advance(lexer);
        }
        /* Consume the trailing newline too */
        if (peek(lexer) == '\n') {
            advance(lexer);
            lexer->line++;
            lexer->line_start = lexer->current;
        }
        /* Recurse to get next token */
        return lex_code_token(lexer);
    }

    char c = advance(lexer);

    /* Strings */
    if (c == '"') return lex_string(lexer);

    /* Numbers */
    if (isdigit((unsigned char)c)) return lex_number(lexer);

    /* Identifiers and keywords */
    if (isalpha((unsigned char)c) || c == '_') return lex_identifier(lexer);

    /* Operators and punctuation */
    switch (c) {
    case '+': return make_token(lexer, TOKEN_PLUS);
    case '*': return make_token(lexer, TOKEN_STAR);
    case '%': return make_token(lexer, TOKEN_PERCENT);
    case ',': return make_token(lexer, TOKEN_COMMA);
    case ':': return make_token(lexer, TOKEN_COLON);
    case ';': return make_token(lexer, TOKEN_SEMICOLON);
    case '(': return make_token(lexer, TOKEN_LPAREN);
    case ')': return make_token(lexer, TOKEN_RPAREN);
    case '[': return make_token(lexer, TOKEN_LBRACKET);
    case ']': return make_token(lexer, TOKEN_RBRACKET);
    case '{':
        /* Could be {{ template expression start if in template mode.
         * But in CODE mode, { is a map literal start. */
        return make_token(lexer, TOKEN_LBRACE);
    case '}':
        /* Check for }} (expression end) — auto-switch to TEMPLATE */
        if (match(lexer, '}')) {
            lexer->mode = LEX_MODE_TEMPLATE;
            return make_token(lexer, TOKEN_EXPR_END);
        }
        return make_token(lexer, TOKEN_RBRACE);

    case '-': return make_token(lexer, TOKEN_MINUS);

    case '/':
        return make_token(lexer, TOKEN_SLASH);

    case '=':
        if (match(lexer, '=')) return make_token(lexer, TOKEN_EQ);
        return make_token(lexer, TOKEN_ASSIGN);

    case '!':
        if (match(lexer, '=')) return make_token(lexer, TOKEN_NEQ);
        return error_token(lexer, "unexpected '!' (did you mean '!=' or 'not'?)");

    case '~':
        if (match(lexer, '=')) return make_token(lexer, TOKEN_APPROX_EQ);
        return error_token(lexer, "unexpected '~' (did you mean '~='?)");

    case '>':
        if (match(lexer, '=')) return make_token(lexer, TOKEN_GTE);
        return make_token(lexer, TOKEN_GT);

    case '<':
        if (match(lexer, '=')) return make_token(lexer, TOKEN_LTE);
        return make_token(lexer, TOKEN_LT);

    case '?':
        if (match(lexer, '?')) return make_token(lexer, TOKEN_NULL_COALESCE);
        if (match(lexer, '.')) return make_token(lexer, TOKEN_NULL_SAFE);
        return error_token(lexer, "unexpected '?' (did you mean '?.' or '\?\?'?)");

    case '.':
        return make_token(lexer, TOKEN_DOT);

    default:
        return error_token(lexer, "unexpected character");
    }
}

/* ================================================================
 * Template mode lexing
 * ================================================================ */

static eka_token_t lex_template_text(eka_lexer_t *lexer) {
    /* Accumulate raw HTML text until we hit @ or {{ */
    const char *start = lexer->current;

    while (!is_at_end(lexer)) {
        char c = peek(lexer);

        if (c == '{' && peek_next(lexer) == '{') {
            break;  /* {{ expression */
        }

        if (c == '@') {
            /* Peek at next word to see if it's a keyword */
            const char *after_at = lexer->current + 1;
            /* Skip whitespace after @ */
            while (*after_at == ' ' || *after_at == '\t') after_at++;
            if (isalpha((unsigned char)*after_at) || *after_at == '_') {
                /* It's @something — could be a keyword. Let caller handle. */
                break;
            }
            /* Not a keyword — @ is literal text. Fall through. */
        }

        if (c == '\n') {
            lexer->line++;
            lexer->line_start = lexer->current + 1;
        }
        advance(lexer);
    }

    size_t length = (size_t)(lexer->current - start);
    if (length == 0) {
        /* No text accumulated — caller needs a different token */
        return make_token(lexer, TOKEN_ERROR);
    }
    return make_token_sized(lexer, TOKEN_TEXT, start, length);
}

static eka_token_t lex_at_keyword(eka_lexer_t *lexer) {
    /* We've seen '@'. Consume the keyword. */
    const char *start = lexer->current;  /* after @ */
    advance(lexer);  /* skip @ */

    /* Skip whitespace between @ and keyword (allowed per spec? Let's be lenient) */
    while (peek(lexer) == ' ' || peek(lexer) == '\t') advance(lexer);

    const char *kw_start = lexer->current;
    while (isalpha((unsigned char)peek(lexer)) || peek(lexer) == '_') {
        advance(lexer);
    }

    size_t kw_len = (size_t)(lexer->current - kw_start);
    eka_token_type_t type = lookup_at_keyword(kw_start, kw_len);

    if (type == TOKEN_ERROR) {
        return error_token(lexer, "unknown @ directive");
    }

    return make_token_sized(lexer, type, start, lexer->current - start);
}

static eka_token_t lex_template_expr(eka_lexer_t *lexer) {
    lexer->start = lexer->current;
    advance(lexer); advance(lexer);  /* skip {{ */
    /* Auto-switch to CODE mode for the expression content */
    lexer->mode = LEX_MODE_CODE;
    return make_token(lexer, TOKEN_EXPR_START);
}

static eka_token_t lex_template_expr_end(eka_lexer_t *lexer) {
    lexer->start = lexer->current;
    advance(lexer); advance(lexer);  /* skip }} */
    /* Auto-switch back to TEMPLATE mode */
    lexer->mode = LEX_MODE_TEMPLATE;
    return make_token(lexer, TOKEN_EXPR_END);
}

/* ================================================================
 * Public API
 * ================================================================ */

void eka_lexer_init(eka_lexer_t *lexer, const char *source) {
    memset(lexer, 0, sizeof(*lexer));
    lexer->source     = source;
    lexer->start      = source;
    lexer->current    = source;
    lexer->line_start = source;
    lexer->line       = 1;
    lexer->mode       = LEX_MODE_CODE;
    lexer->had_error  = false;
}

void eka_lexer_set_mode(eka_lexer_t *lexer, eka_lexer_mode_t mode) {
    lexer->mode = mode;
}

eka_token_t eka_lexer_current(const eka_lexer_t *lexer) {
    return lexer->current_token;
}

eka_token_t eka_lexer_next(eka_lexer_t *lexer) {
    if (lexer->had_error) {
        /* After an error, keep returning EOF */
        lexer->current_token = make_token(lexer, TOKEN_EOF);
        return lexer->current_token;
    }

    if (lexer->mode == LEX_MODE_TEMPLATE) {
        /* Check what's next */
        if (is_at_end(lexer)) {
            lexer->current_token = make_token(lexer, TOKEN_EOF);
            return lexer->current_token;
        }

        /* {{ */
        if (peek(lexer) == '{' && peek_next(lexer) == '{') {
            lexer->current_token = lex_template_expr(lexer);
            /* lex_template_expr already switches to CODE mode */
            return lexer->current_token;
        }

        /* }} */
        if (peek(lexer) == '}' && peek_next(lexer) == '}') {
            lexer->current_token = lex_template_expr_end(lexer);
            /* lex_template_expr_end already switches to TEMPLATE mode */
            return lexer->current_token;
        }

        /* @keyword */
        if (peek(lexer) == '@') {
            const char *after_at = lexer->current + 1;
            while (*after_at == ' ' || *after_at == '\t') after_at++;
            if (isalpha((unsigned char)*after_at) || *after_at == '_') {
                lexer->current_token = lex_at_keyword(lexer);
                /* Auto-switch to CODE mode for @if, @for, @do (need to parse
                 * condition/expression before the template body).
                 * @end and @else stay in TEMPLATE mode. */
                switch (lexer->current_token.type) {
                case TOKEN_AT_IF:
                case TOKEN_AT_FOR:
                case TOKEN_AT_DO:
                case TOKEN_AT_CSRF:
                    lexer->mode = LEX_MODE_CODE;
                    break;
                default:
                    break;
                }
                return lexer->current_token;
            }
            /* @ not followed by word — fall through to text accumulation */
        }

        /* Regular text */
        eka_token_t text = lex_template_text(lexer);
        if (text.type != TOKEN_ERROR) {
            lexer->current_token = text;
            return lexer->current_token;
        }

        /* If text accumulation returned empty (shouldn't happen if not @ or {{),
         * fall through to code mode as a safety net */
    }

    /* CODE mode (or fallback) */
    /* Check for }} which is only valid in template expression context */
    if (peek(lexer) == '}' && peek_next(lexer) == '}') {
        lexer->current_token = lex_template_expr_end(lexer);
        return lexer->current_token;
    }

    /* Check for @keyword in code mode (for method blocks at top level) */
    if (peek(lexer) == '@') {
        const char *after_at = lexer->current + 1;
        while (*after_at == ' ' || *after_at == '\t') after_at++;
        if (isalpha((unsigned char)*after_at) || *after_at == '_') {
            lexer->current_token = lex_at_keyword(lexer);
            return lexer->current_token;
        }
        /* @ at start of an identifier — not valid in Eka, error. */
    }

    lexer->current_token = lex_code_token(lexer);
    return lexer->current_token;
}
