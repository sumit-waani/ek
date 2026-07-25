#define _GNU_SOURCE
#include "fmt/fmt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

/* ================================================================
 * Helpers
 * ================================================================ */

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Check if line starts with keyword (followed by space/tab/newline/(/: /end of string) */
static bool starts_with_kw(const char *s, const char *kw) {
    size_t len = strlen(kw);
    if (strncmp(s, kw, len) != 0) return false;
    char c = s[len];
    return (c == '\0' || c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
            c == '(' || c == ':' || c == ',');
}

static size_t strip_trailing(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r'))
        len--;
    s[len] = '\0';
    return len;
}

static bool is_blank(const char *s) {
    s = skip_ws(s);
    return (*s == '\0' || *s == '\n' || *s == '\r');
}

/* ================================================================
 * Raw passthrough tags
 * ================================================================ */

static bool is_open_raw_tag(const char *s) {
    static const char *tags[] = {"<script", "<style", "<pre", "<textarea", "<code", NULL};
    for (const char **t = tags; *t; t++) {
        size_t len = strlen(*t);
        if (strncasecmp(s, *t, len) == 0) {
            char c = s[len];
            if ((c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\0')
                && s[1] != '/') {
                return true;
            }
        }
    }
    return false;
}

static bool is_close_raw_tag(const char *s) {
    static const char *tags[] = {"</script", "</style", "</pre", "</textarea", "</code", NULL};
    for (const char **t = tags; *t; t++) {
        size_t len = strlen(*t);
        if (strncasecmp(s, *t, len) == 0) {
            char c = s[len];
            if (c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\0') {
                return true;
            }
        }
    }
    return false;
}

/* ================================================================
 * Line splitting
 * ================================================================ */

static char **split_lines(const char *src, size_t *out_n) {
    size_t cap = 256, n = 0;
    char **lines = malloc(cap * sizeof(char *));
    const char *p = src;
    while (*p) {
        const char *start = p;
        while (*p && *p != '\n') p++;
        size_t len = (size_t)(p - start);
        char *line = malloc(len + 1);
        memcpy(line, start, len);
        line[len] = '\0';
        if (n >= cap) { cap *= 2; lines = realloc(lines, cap * sizeof(char *)); }
        lines[n++] = line;
        if (*p == '\n') p++;
    }
    *out_n = n;
    return lines;
}

static void free_lines(char **lines, size_t n) {
    for (size_t i = 0; i < n; i++) free(lines[i]);
    free(lines);
}

/* ================================================================
 * Output buffer
 * ================================================================ */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} buf_t;

static void buf_init(buf_t *b) {
    b->cap = 4096;
    b->len = 0;
    b->data = malloc(b->cap);
}

static void buf_ensure(buf_t *b, size_t need) {
    if (b->len + need > b->cap) {
        b->cap = (b->cap + need) * 2;
        b->data = realloc(b->data, b->cap);
    }
}

static void buf_write(buf_t *b, const char *s, size_t len) {
    buf_ensure(b, len);
    memcpy(b->data + b->len, s, len);
    b->len += len;
}

static void buf_char(buf_t *b, char c) {
    buf_ensure(b, 1);
    b->data[b->len++] = c;
}

static void buf_indent(buf_t *b, int depth) {
    buf_ensure(b, (size_t)(depth * 2));
    for (int i = 0; i < depth * 2; i++) b->data[b->len++] = ' ';
}

static void buf_line(buf_t *b, int indent, const char *text, size_t len) {
    buf_indent(b, indent);
    buf_write(b, text, len);
    buf_char(b, '\n');
}

static char *buf_finish(buf_t *b) {
    /* Ensure ends with newline */
    if (b->len == 0 || b->data[b->len - 1] != '\n') buf_char(b, '\n');
    b->data[b->len] = '\0';
    return b->data;
}

/* ================================================================
 * Formatter core
 * ================================================================ */

/* Context tracking for determining indent behavior */
typedef enum {
    CTX_TOPLEVEL,       /* top-level code (init scope) */
    CTX_METHOD,         /* inside @get/@post/etc, before first template content */
    CTX_TEMPLATE,       /* template content (HTML + template control) */
} ctx_t;

static char *do_format(const char *source) {
    size_t nlines = 0;
    char **lines = split_lines(source, &nlines);
    if (!lines) return NULL;

    buf_t out;
    buf_init(&out);

    int indent = 0;
    bool in_raw = false;
    bool prev_blank = false;
    ctx_t ctx = CTX_TOPLEVEL;

    /* Stack for indent levels when entering template constructs */
    /* We don't need a full stack; the indent variable tracks depth correctly. */

    for (size_t i = 0; i < nlines; i++) {
        const char *raw = lines[i];
        const char *trimmed = skip_ws(raw);

        /* ---- Raw passthrough: emit verbatim ---- */
        if (in_raw) {
            buf_write(&out, raw, strlen(raw));
            buf_char(&out, '\n');
            if (is_close_raw_tag(trimmed)) in_raw = false;
            prev_blank = false;
            continue;
        }

        if (is_open_raw_tag(trimmed)) {
            /* Emit opening tag at current indent, then switch to raw mode */
            size_t tlen = strlen(trimmed);
            char *copy = strdup(trimmed);
            strip_trailing(copy);
            buf_line(&out, indent, copy, strlen(copy));
            free(copy);
            in_raw = true;
            prev_blank = false;
            continue;
        }

        /* ---- Blank lines ---- */
        if (is_blank(trimmed)) {
            if (!prev_blank) {
                buf_char(&out, '\n');
                prev_blank = true;
            }
            continue;
        }

        /* ---- Comment lines ---- */
        if (trimmed[0] == '-' && trimmed[1] == '-') {
            buf_line(&out, indent, trimmed, strlen(trimmed));
            prev_blank = false;
            continue;
        }

        /* ---- Method block start ---- */
        if (ctx == CTX_TOPLEVEL &&
            (starts_with_kw(trimmed, "@get") || starts_with_kw(trimmed, "@post") ||
             starts_with_kw(trimmed, "@put") || starts_with_kw(trimmed, "@delete") ||
             starts_with_kw(trimmed, "@patch"))) {
            /* Blank line before method block (if not at start) */
            if (i > 0 && !prev_blank) buf_char(&out, '\n');

            size_t tlen = strlen(trimmed);
            char *copy = strdup(trimmed);
            strip_trailing(copy);
            buf_line(&out, indent, copy, strlen(copy));
            free(copy);

            indent++;
            ctx = CTX_METHOD;
            prev_blank = false;
            continue;
        }

        /* ---- @end closing method block ---- */
        if (ctx != CTX_TOPLEVEL && starts_with_kw(trimmed, "@end") && indent == 1) {
            indent--;
            ctx = CTX_TOPLEVEL;
            buf_line(&out, indent, "@end", 4);
            prev_blank = false;
            continue;
        }

        /* ---- Template mode keywords ---- */
        if (ctx != CTX_TOPLEVEL) {
            /* @end (closing template construct, not method block) */
            if (starts_with_kw(trimmed, "@end")) {
                indent--;
                char *copy = strdup(trimmed);
                strip_trailing(copy);
                buf_line(&out, indent, copy, strlen(copy));
                free(copy);
                prev_blank = false;
                continue;
            }

            /* @else (template) */
            if (starts_with_kw(trimmed, "@else")) {
                indent--;
                char *copy = strdup(trimmed);
                strip_trailing(copy);
                buf_line(&out, indent, copy, strlen(copy));
                free(copy);
                indent++;
                prev_blank = false;
                continue;
            }

            /* @if, @for, @do → indent increases after */
            bool inc = starts_with_kw(trimmed, "@if") ||
                       starts_with_kw(trimmed, "@for") ||
                       starts_with_kw(trimmed, "@do");

            char *copy = strdup(trimmed);
            strip_trailing(copy);
            buf_line(&out, indent, copy, strlen(copy));
            free(copy);

            if (inc) indent++;
            prev_blank = false;
            continue;
        }

        /* ---- Code mode keywords (top-level) ---- */
        {
            /* end */
            if (starts_with_kw(trimmed, "end")) {
                indent--;
                if (indent < 0) indent = 0;
                char *copy = strdup(trimmed);
                strip_trailing(copy);
                buf_line(&out, indent, copy, strlen(copy));
                free(copy);
                prev_blank = false;
                continue;
            }

            /* else / else if */
            if (starts_with_kw(trimmed, "else")) {
                indent--;
                if (indent < 0) indent = 0;
                char *copy = strdup(trimmed);
                strip_trailing(copy);
                buf_line(&out, indent, copy, strlen(copy));
                free(copy);
                indent++;
                prev_blank = false;
                continue;
            }

            /* catch */
            if (starts_with_kw(trimmed, "catch")) {
                indent--;
                if (indent < 0) indent = 0;
                char *copy = strdup(trimmed);
                strip_trailing(copy);
                buf_line(&out, indent, copy, strlen(copy));
                free(copy);
                indent++;
                prev_blank = false;
                continue;
            }

            /* func, if, for, while, try → indent increase after */
            bool inc = starts_with_kw(trimmed, "func") ||
                       starts_with_kw(trimmed, "if") ||
                       starts_with_kw(trimmed, "for") ||
                       starts_with_kw(trimmed, "while") ||
                       starts_with_kw(trimmed, "try");

            /* Blank line before func/const at top level */
            if (indent == 0 && i > 0 && !prev_blank &&
                (starts_with_kw(trimmed, "func") || starts_with_kw(trimmed, "const"))) {
                buf_char(&out, '\n');
            }

            char *copy = strdup(trimmed);
            strip_trailing(copy);
            buf_line(&out, indent, copy, strlen(copy));
            free(copy);

            if (inc) indent++;
            prev_blank = false;
            continue;
        }
    }

    free_lines(lines, nlines);
    return buf_finish(&out);
}

/* ================================================================
 * Public API
 * ================================================================ */

int eka_fmt(const char *filepath, bool check_only) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        fprintf(stderr, "eka fmt: cannot open '%s'\n", filepath);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *source = malloc((size_t)size + 1);
    if (!source) { fclose(f); return -1; }
    size_t n = fread(source, 1, (size_t)size, f);
    fclose(f);
    source[n] = '\0';

    char *formatted = do_format(source);
    if (!formatted) { free(source); return -1; }

    if (check_only) {
        if (strcmp(source, formatted) == 0) {
            free(source); free(formatted);
            return 0;
        }
        fprintf(stderr, "eka fmt: %s is not formatted\n", filepath);
        free(source); free(formatted);
        return 1;
    }

    if (strcmp(source, formatted) == 0) {
        free(source); free(formatted);
        return 0;
    }

    f = fopen(filepath, "wb");
    if (!f) {
        fprintf(stderr, "eka fmt: cannot write '%s'\n", filepath);
        free(source); free(formatted);
        return -1;
    }
    fwrite(formatted, 1, strlen(formatted), f);
    fclose(f);

    printf("eka fmt: %s\n", filepath);
    free(source); free(formatted);
    return 0;
}
