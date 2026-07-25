#ifndef CLI_H
#define CLI_H

typedef enum {
    CLI_RUN,
    CLI_CHECK,
    CLI_FMT,
    CLI_VERSION,
    CLI_HELP,
    CLI_UNKNOWN,
} cli_command_t;

typedef struct {
    cli_command_t command;
    int           port;
    const char   *file;       /* --file flag */
    const char   *static_dir; /* --static flag */
    int           check_only; /* --check flag for fmt */
} cli_args_t;

/* Parse argv into cli_args_t. Prints errors to stderr. Returns 0 on success. */
int cli_parse(int argc, char **argv, cli_args_t *out);

/* Print version info to stdout. */
void cli_print_version(void);

/* Print usage to stdout. */
void cli_print_help(const char *progname);

#endif /* CLI_H */
