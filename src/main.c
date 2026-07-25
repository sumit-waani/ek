#include "eka.h"
#include "cli/cli.h"

#include <stdio.h>
#include <stdlib.h>

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

    case CLI_RUN:
        printf("eka: run — not implemented yet (port=%d, file=%s, static=%s)\n",
               args.port, args.file, args.static_dir);
        return 0;

    case CLI_CHECK:
        printf("eka: check — not implemented yet (file=%s)\n", args.file);
        return 0;

    case CLI_FMT:
        printf("eka: fmt — not implemented yet (file=%s, check=%d)\n",
               args.file, args.check_only);
        return 0;
    }

    return 0;
}
