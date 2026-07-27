#include "core/value.h"
#include "core/obj.h"

#include <stdlib.h>
#include <string.h>

/* HTML-escape a value for safe template interpolation.
 *
 * - RawString → returned as-is (trusted content, bypass escaping)
 * - String → escaped version (new allocation)
 * - Other types → stringify first, then escape
 *
 * Returns a new value. Caller owns the result. */
eka_value_t eka_html_escape_value(eka_value_t v) {
    /* RawString: pass through unchanged */
    if (eka_obj_is_type(v, OBJ_RAWSTRING)) {
        return v;
    }

    /* Get the string representation */
    eka_string_t *src;
    if (eka_obj_is_type(v, OBJ_STRING)) {
        src = eka_as_string(v);
    } else if (eka_is_nil(v)) {
        /* null → empty string, nothing to escape */
        return eka_string_val(eka_string_new("", 0));
    } else {
        src = eka_value_to_string(v);
    }

    /* Quick scan: if no special chars, return as-is (no allocation) */
    bool needs_escape = false;
    for (size_t i = 0; i < src->length; i++) {
        char c = src->data[i];
        if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') {
            needs_escape = true;
            break;
        }
    }
    if (!needs_escape) {
        return v;
    }

    /* Worst case: every char becomes 6-char entity (&#x27;) */
    size_t max_out = src->length * 6 + 1;
    char *buf = malloc(max_out);
    if (!buf) return v;

    size_t out = 0;
    for (size_t i = 0; i < src->length; i++) {
        switch (src->data[i]) {
        case '&':  memcpy(buf + out, "&amp;", 5);  out += 5; break;
        case '<':  memcpy(buf + out, "&lt;", 4);   out += 4; break;
        case '>':  memcpy(buf + out, "&gt;", 4);   out += 4; break;
        case '"':  memcpy(buf + out, "&quot;", 6); out += 6; break;
        case '\'': memcpy(buf + out, "&#x27;", 6); out += 6; break;
        default:   buf[out++] = src->data[i]; break;
        }
    }
    buf[out] = '\0';

    eka_string_t *result = eka_string_take(buf, out);
    return eka_string_val(result);
}
