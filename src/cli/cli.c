#include "cli/cli.h"
#include "eka.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cli_print_version(void) {
    printf("eka v%s (linux/%s)\n", EKA_VERSION,
#ifdef __x86_64__
           "amd64"
#elif defined(__aarch64__)
           "arm64"
#else
           "unknown"
#endif
    );
}

void cli_print_help(const char *progname) {
    printf(
        "Eka — One file. One truth.\n"
        "\n"
        "Usage: %s <command> [options]\n"
        "\n"
        "Commands:\n"
        "  run       Start the development server\n"
        "  check     Syntax and type check (no server)\n"
        "  fmt       Format app.eka\n"
        "  version   Print version and exit\n"
        "\n"
        "Options (run):\n"
        "  --port <n>       Server port (default: 8080)\n"
        "  --static <dir>   Static files directory (default: public/)\n"
        "  --file <path>    .eka file to run (default: app.eka)\n"
        "\n"
        "Options (check, fmt):\n"
        "  --file <path>    .eka file to check/format\n"
        "\n"
        "Options (fmt):\n"
        "  --check          Check formatting only, don't modify\n"
        "\n"
        "Environment:\n"
        "  EKA_PORT          Server port (overridden by --port)\n"
        "  EKA_STATIC        Static files dir\n"
        "  EKA_SECRET        Session signing key\n"
        "  EKA_ENV           'development' or 'production'\n"
        "  EKA_THREAD_POOL   Worker thread count\n"
        "  EKA_MAX_SSE       Max SSE connections (default: 1000)\n"
        "  EKA_CACHE_SIZE    Cache size in MB (default: 64)\n"
        "\n",
        progname);
}

int cli_parse(int argc, char **argv, cli_args_t *out) {
    memset(out, 0, sizeof(*out));
    out->command = CLI_UNKNOWN;
    out->port = 8080;
    out->file = "app.eka";
    out->static_dir = "public";

    if (argc < 2) {
        out->command = CLI_HELP;
        return 0;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "run") == 0) {
        out->command = CLI_RUN;
    } else if (strcmp(cmd, "check") == 0) {
        out->command = CLI_CHECK;
    } else if (strcmp(cmd, "fmt") == 0) {
        out->command = CLI_FMT;
    } else if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        out->command = CLI_VERSION;
    } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        out->command = CLI_HELP;
    } else {
        fprintf(stderr, "eka: unknown command '%s'\n", cmd);
        fprintf(stderr, "Try 'eka help' for usage.\n");
        return 1;
    }

    /* Parse options */
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            out->port = atoi(argv[++i]);
            if (out->port < 1 || out->port > 65535) {
                fprintf(stderr, "eka: invalid port '%s'\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            out->file = argv[++i];
        } else if (strcmp(argv[i], "--static") == 0 && i + 1 < argc) {
            out->static_dir = argv[++i];
        } else if (strcmp(argv[i], "--check") == 0) {
            out->check_only = 1;
        } else {
            fprintf(stderr, "eka: unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    return 0;
}
