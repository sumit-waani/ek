#include "eka.h"
#include "cli/cli.h"
#include "parser/parser.h"
#include "compiler/compiler.h"
#include "runtime/server.h"
#include "fmt/fmt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Read entire file into a null-terminated string. */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "eka: cannot open '%s'\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

int main(int argc, char **argv) {
    cli_args_t args;
    int parse_ok = cli_parse(argc, argv, &args);

    if (parse_ok != 0) {
        return 2;
    }

    switch (args.command) {
    case CLI_VERSION:
        cli_print_version();
        return 0;

    case CLI_HELP:
    case CLI_UNKNOWN:
        cli_print_help(argv[0]);
        return (args.command == CLI_HELP) ? 0 : 2;

    case CLI_RUN: {
        /* Read the .eka file */
        char *source = read_file(args.file);
        if (!source) return 2;

        /* Parse */
        eka_parser_t parser;
        eka_parser_init(&parser, source);
        ast_node_t *ast = eka_parse(&parser);

        if (parser.had_error) {
            fprintf(stderr, "eka: parse error: %s\n", parser.error_message);
            free(source);
            return 1;
        }

        /* Compile */
        eka_compiled_program_t *prog = eka_compile(ast);
        if (prog->had_error) {
            fprintf(stderr, "eka: compile error: %s\n", prog->error_msg);
            free(source);
            return 1;
        }

        /* Run server (source must stay alive — AST nodes reference it) */
        eka_server_t *server = eka_server_create(prog, args.static_dir, args.port);
        int ret = eka_server_run(server);
        eka_server_free(server);
        free(source);
        return ret;
    }

    case CLI_CHECK: {
        char *source = read_file(args.file);
        if (!source) return 2;

        eka_parser_t parser;
        eka_parser_init(&parser, source);
        eka_parse(&parser);

        if (parser.had_error) {
            fprintf(stderr, "eka: %s\n", parser.error_message);
            free(source);
            return 1;
        }

        printf("eka: %s — OK\n", args.file);
        free(source);
        return 0;
    }

    case CLI_FMT: {
        return eka_fmt(args.file, args.check_only != 0);
    }
    }

    return 0;
}
