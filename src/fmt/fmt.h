#ifndef FMT_H
#define FMT_H

#include <stdbool.h>

/*
 * Eka source formatter.
 *
 * Rules:
 *   - 2-space indentation
 *   - Consistent blank line spacing between blocks
 *   - No trailing whitespace
 *   - No formatting changes to raw passthrough tag contents
 *     (<script>, <style>, <pre>, <textarea>, <code>)
 *
 * Usage:
 *   int result = eka_fmt("app.eka", false);  // format in-place
 *   int result = eka_fmt("app.eka", true);   // check only
 *
 * Returns 0 if already formatted (or formatting succeeded).
 * Returns 1 if --check and file is not formatted.
 * Returns -1 on error.
 */

int eka_fmt(const char *filepath, bool check_only);

#endif /* FMT_H */
