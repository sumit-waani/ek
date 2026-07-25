#define _GNU_SOURCE
#include "builtins/builtins.h"
#include "core/obj.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <math.h>

/* External dependencies */
#include <sqlite3.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <cmark.h>

/* yyjson */
#include "../../vendor/yyjson.h"

/* ================================================================
 * Helpers
 * ================================================================ */

static eka_value_t make_native(eka_native_fn_t fn, void *ctx, const char *name) {
    return eka_native_val(eka_native_new(fn, ctx, name));
}

static void map_set_cstr(eka_map_t *map, const char *key_str, eka_value_t val) {
    eka_string_t *key = eka_string_intern(key_str, strlen(key_str));
    eka_map_set(map, key, val);
}

/* Get string arg at index, or empty string if missing/wrong type */
static const char *arg_string(int argc, eka_value_t *args, int idx,
                              const char *default_val) {
    if (idx >= argc) return default_val;
    if (eka_obj_is_type(args[idx], OBJ_STRING))
        return eka_as_string(args[idx])->data;
    return default_val;
}

/* Get int arg at index, or default */
static int64_t arg_int(int argc, eka_value_t *args, int idx, int64_t default_val) {
    if (idx >= argc) return default_val;
    if (eka_is_int(args[idx])) return eka_as_int(args[idx]);
    if (eka_is_number(args[idx])) return (int64_t)eka_as_number(args[idx]);
    return default_val;
}

/* Get bool arg at index, or default */
static bool arg_bool(int argc, eka_value_t *args, int idx, bool default_val) {
    if (idx >= argc) return default_val;
    if (eka_is_bool(args[idx])) return eka_as_bool(args[idx]);
    return default_val;
}

/* Validate arg count */
#define CHECK_ARGC(n) do { if (argc < (n)) return eka_nil(); } while(0)

/* ================================================================
 * 1. print
 *
 *    print("Hello")        → prints to stdout
 *    print(someValue)      → auto-stringifies
 * ================================================================ */

static eka_value_t builtin_print(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    for (int i = 0; i < argc; i++) {
        eka_string_t *s = eka_value_to_string(args[i]);
        if (i > 0) printf(" ");
        fwrite(s->data, 1, s->length, stdout);
    }
    printf("\n");
    return eka_nil();
}

/* ================================================================
 * 4. sqlite
 *
 *    let db = sqlite.open("app.db")
 *    db.query(sql, params?) → list<map>
 *    db.exec(sql, params?)  → nil
 *    db.lastId()            → number
 * ================================================================ */

static eka_value_t sqlite_query(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args);
static eka_value_t sqlite_exec(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args);
static eka_value_t sqlite_last_id(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args);

static eka_value_t sqlite_open(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)ctx;
    CHECK_ARGC(1);
    if (!eka_obj_is_type(args[0], OBJ_STRING)) return eka_nil();

    eka_string_t *filename = eka_as_string(args[0]);

    if (vm->db_conn_count >= EKA_MAX_DB_CONNS) {
        fprintf(stderr, "eka: too many database connections (max %d)\n", EKA_MAX_DB_CONNS);
        return eka_nil();
    }

    sqlite3 *db = NULL;
    int rc = sqlite3_open(filename->data, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "eka: sqlite.open error: %s\n", sqlite3_errmsg(db));
        if (db) sqlite3_close(db);
        return eka_nil();
    }

    /* Enable WAL mode */
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);

    int conn_idx = vm->db_conn_count++;
    vm->db_conns[conn_idx].db = db;
    vm->db_conns[conn_idx].last_id = 0;

    /* Build the db map with methods */
    eka_map_t *db_map = eka_map_new(8);
    map_set_cstr(db_map, "query",  make_native(sqlite_query,  (void *)(intptr_t)conn_idx, "query"));
    map_set_cstr(db_map, "exec",   make_native(sqlite_exec,   (void *)(intptr_t)conn_idx, "exec"));
    map_set_cstr(db_map, "lastId", make_native(sqlite_last_id, (void *)(intptr_t)conn_idx, "lastId"));
    /* Store conn_idx for internal use */
    map_set_cstr(db_map, "__db_idx", eka_int(conn_idx));

    return eka_map_val(db_map);
}

static eka_value_t sqlite_query(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm;
    CHECK_ARGC(1);
    int idx = (int)(intptr_t)ctx;
    if (idx < 0 || idx >= vm->db_conn_count || !vm->db_conns[idx].db) return eka_nil();

    sqlite3 *db = vm->db_conns[idx].db;
    const char *sql = arg_string(argc, args, 0, NULL);
    if (!sql) return eka_nil();

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return eka_nil();
    }

    /* Bind parameters if provided */
    if (argc >= 2 && eka_obj_is_type(args[1], OBJ_LIST)) {
        eka_list_t *params = eka_as_list(args[1]);
        for (uint32_t i = 0; i < params->length && (int)i < sqlite3_bind_parameter_count(stmt); i++) {
            eka_value_t p = params->items[i];
            if (eka_is_int(p)) {
                sqlite3_bind_int64(stmt, i + 1, eka_as_int(p));
            } else if (eka_is_number(p)) {
                sqlite3_bind_double(stmt, i + 1, eka_as_number(p));
            } else if (eka_obj_is_type(p, OBJ_STRING)) {
                eka_string_t *s = eka_as_string(p);
                sqlite3_bind_text(stmt, i + 1, s->data, (int)s->length, SQLITE_TRANSIENT);
            } else if (eka_is_nil(p)) {
                sqlite3_bind_null(stmt, i + 1);
            } else if (eka_is_bool(p)) {
                sqlite3_bind_int(stmt, i + 1, eka_as_bool(p) ? 1 : 0);
            }
        }
    }

    /* Build result list */
    eka_list_t *rows = eka_list_new(8);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int col_count = sqlite3_column_count(stmt);
        eka_map_t *row = eka_map_new((uint32_t)col_count + 1);

        for (int c = 0; c < col_count; c++) {
            const char *col_name = sqlite3_column_name(stmt, c);
            eka_string_t *key = eka_string_intern(col_name, strlen(col_name));
            eka_value_t val;

            switch (sqlite3_column_type(stmt, c)) {
            case SQLITE_INTEGER:
                val = eka_int(sqlite3_column_int64(stmt, c));
                break;
            case SQLITE_FLOAT:
                val = eka_number(sqlite3_column_double(stmt, c));
                break;
            case SQLITE_TEXT: {
                const unsigned char *text = sqlite3_column_text(stmt, c);
                int text_len = sqlite3_column_bytes(stmt, c);
                val = eka_string_val(eka_string_new((const char *)text, (size_t)text_len));
                break;
            }
            case SQLITE_NULL:
                val = eka_nil();
                break;
            default:
                val = eka_nil();
                break;
            }
            eka_map_set(row, key, val);
        }
        eka_list_push(rows, eka_map_val(row));
    }

    sqlite3_finalize(stmt);
    return eka_list_val(rows);
}

static eka_value_t sqlite_exec(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm;
    CHECK_ARGC(1);
    int idx = (int)(intptr_t)ctx;
    if (idx < 0 || idx >= vm->db_conn_count || !vm->db_conns[idx].db) return eka_nil();

    sqlite3 *db = vm->db_conns[idx].db;
    const char *sql = arg_string(argc, args, 0, NULL);
    if (!sql) return eka_nil();

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return eka_nil();
    }

    /* Bind parameters if provided */
    if (argc >= 2 && eka_obj_is_type(args[1], OBJ_LIST)) {
        eka_list_t *params = eka_as_list(args[1]);
        for (uint32_t i = 0; i < params->length && (int)i < sqlite3_bind_parameter_count(stmt); i++) {
            eka_value_t p = params->items[i];
            if (eka_is_int(p)) {
                sqlite3_bind_int64(stmt, i + 1, eka_as_int(p));
            } else if (eka_is_number(p)) {
                sqlite3_bind_double(stmt, i + 1, eka_as_number(p));
            } else if (eka_obj_is_type(p, OBJ_STRING)) {
                eka_string_t *s = eka_as_string(p);
                sqlite3_bind_text(stmt, i + 1, s->data, (int)s->length, SQLITE_TRANSIENT);
            } else if (eka_is_nil(p)) {
                sqlite3_bind_null(stmt, i + 1);
            } else if (eka_is_bool(p)) {
                sqlite3_bind_int(stmt, i + 1, eka_as_bool(p) ? 1 : 0);
            }
        }
    }

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        fprintf(stderr, "eka: sqlite exec error: %s\n", sqlite3_errmsg(db));
        return eka_nil();
    }
    vm->db_conns[idx].last_id = sqlite3_last_insert_rowid(db);
    return eka_nil();
}

static eka_value_t sqlite_last_id(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)argc; (void)args;
    int idx = (int)(intptr_t)ctx;
    if (idx < 0 || idx >= vm->db_conn_count || !vm->db_conns[idx].db) return eka_int(0);
    return eka_int(vm->db_conns[idx].last_id);
}

/* ================================================================
 * 5. json
 *
 *    json.parse(str)    → map/list
 *    json.stringify(val) → string
 *    json.stringify(val, true) → pretty-printed string
 * ================================================================ */

/* Forward declaration — recursive converter */
static eka_value_t yyjson_to_eka(yyjson_val *v);
static yyjson_mut_val *eka_to_yyjson(yyjson_mut_doc *doc, eka_value_t v);

static eka_value_t json_parse_fn(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    if (!eka_obj_is_type(args[0], OBJ_STRING)) return eka_nil();

    eka_string_t *s = eka_as_string(args[0]);
    yyjson_doc *doc = yyjson_read_opts((char *)s->data, s->length, 0, NULL, NULL);
    if (!doc) return eka_nil();

    eka_value_t result = yyjson_to_eka(yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    return result;
}

static eka_value_t json_stringify_fn(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    bool pretty = arg_bool(argc, args, 1, false);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = eka_to_yyjson(doc, args[0]);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_write_flag flags = YYJSON_WRITE_NOFLAG;
    if (pretty) flags |= YYJSON_WRITE_PRETTY;

    size_t json_len;
    char *json_str = yyjson_mut_write(doc, flags, &json_len);
    yyjson_mut_doc_free(doc);

    if (!json_str) return eka_string_val(eka_string_new("null", 4));

    eka_string_t *result = eka_string_take(json_str, json_len);
    return eka_string_val(result);
}

/* Recursively convert yyjson immutable val → eka_value_t */
static eka_value_t yyjson_to_eka(yyjson_val *v) {
    if (!v) return eka_nil();

    switch (yyjson_get_type(v)) {
    case YYJSON_TYPE_NULL:
        return eka_nil();
    case YYJSON_TYPE_BOOL:
        return eka_bool(yyjson_get_bool(v));
    case YYJSON_TYPE_NUM: {
        switch (yyjson_get_subtype(v)) {
        case YYJSON_SUBTYPE_UINT:
            return eka_int((int64_t)yyjson_get_uint(v));
        default: {
            double d = yyjson_get_real(v);
            /* Small-enough integer? Store as int */
            if (d == floor(d) && d >= -35184372088832.0 && d <= 35184372088831.0) {
                return eka_int((int64_t)d);
            }
            return eka_number(d);
        }
        }
    }
    case YYJSON_TYPE_STR: {
        const char *str = yyjson_get_str(v);
        size_t len = yyjson_get_len(v);
        return eka_string_val(eka_string_new(str, len));
    }
    case YYJSON_TYPE_ARR: {
        eka_list_t *list = eka_list_new(8);
        yyjson_val *item;
        yyjson_arr_iter iter;
        yyjson_arr_iter_init(v, &iter);
        while ((item = yyjson_arr_iter_next(&iter))) {
            eka_list_push(list, yyjson_to_eka(item));
        }
        return eka_list_val(list);
    }
    case YYJSON_TYPE_OBJ: {
        eka_map_t *map = eka_map_new(16);
        yyjson_val *key, *val;
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(v, &iter);
        while ((key = yyjson_obj_iter_next(&iter))) {
            val = yyjson_obj_iter_get_val(key);
            const char *kstr = yyjson_get_str(key);
            size_t klen = yyjson_get_len(key);
            eka_string_t *ekey = eka_string_intern(kstr, klen);
            eka_map_set(map, ekey, yyjson_to_eka(val));
        }
        return eka_map_val(map);
    }
    default:
        return eka_nil();
    }
}

/* Recursively convert eka_value_t → yyjson mutable val */
static yyjson_mut_val *eka_to_yyjson(yyjson_mut_doc *doc, eka_value_t v) {
    if (eka_is_nil(v)) {
        return yyjson_mut_null(doc);
    }
    if (eka_is_bool(v)) {
        return yyjson_mut_bool(doc, eka_as_bool(v));
    }
    if (eka_is_int(v)) {
        return yyjson_mut_int(doc, eka_as_int(v));
    }
    if (eka_is_number(v)) {
        return yyjson_mut_real(doc, eka_as_number(v));
    }
    if (eka_obj_is_type(v, OBJ_STRING)) {
        eka_string_t *s = eka_as_string(v);
        return yyjson_mut_strncpy(doc, s->data, s->length);
    }
    if (eka_obj_is_type(v, OBJ_LIST)) {
        eka_list_t *list = eka_as_list(v);
        yyjson_mut_val *arr = yyjson_mut_arr(doc);
        for (uint32_t i = 0; i < list->length; i++) {
            yyjson_mut_arr_append(arr, eka_to_yyjson(doc, list->items[i]));
        }
        return arr;
    }
    if (eka_obj_is_type(v, OBJ_MAP)) {
        eka_map_t *map = eka_as_map(v);
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        for (uint32_t i = 0; i < map->capacity; i++) {
            eka_map_entry_t *e = &map->entries[i];
            if (e->key && !eka_map_entry_is_tombstone(e->key)) {
                /* Skip internal keys starting with __ */
                if (e->key->length >= 2 && e->key->data[0] == '_' && e->key->data[1] == '_')
                    continue;
                yyjson_mut_obj_add(obj,
                    yyjson_mut_strncpy(doc, e->key->data, e->key->length),
                    eka_to_yyjson(doc, e->value));
            }
        }
        return obj;
    }
    return yyjson_mut_null(doc);
}

/* ================================================================
 * 8. html
 *
 *    html.escape(str)  → escaped string
 *    html.raw(str)     → RawString (bypasses auto-escaping)
 * ================================================================ */

static eka_value_t html_escape(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    if (!eka_obj_is_type(args[0], OBJ_STRING)) return args[0];

    eka_string_t *s = eka_as_string(args[0]);
    /* Worst case: every char becomes 6-char entity (&amp; etc.) */
    size_t max_out = s->length * 6 + 1;
    char *buf = malloc(max_out);
    if (!buf) return args[0];

    size_t out = 0;
    for (size_t i = 0; i < s->length; i++) {
        switch (s->data[i]) {
        case '&':  memcpy(buf + out, "&amp;", 5);  out += 5; break;
        case '<':  memcpy(buf + out, "&lt;", 4);   out += 4; break;
        case '>':  memcpy(buf + out, "&gt;", 4);   out += 4; break;
        case '"':  memcpy(buf + out, "&quot;", 6); out += 6; break;
        case '\'': memcpy(buf + out, "&#x27;", 6); out += 6; break;
        default:   buf[out++] = s->data[i]; break;
        }
    }
    buf[out] = '\0';
    eka_string_t *result = eka_string_take(buf, out);
    return eka_string_val(result);
}

static eka_value_t html_raw(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    if (!eka_obj_is_type(args[0], OBJ_STRING)) return eka_nil();
    eka_string_t *s = eka_as_string(args[0]);
    return eka_rawstring_val(eka_rawstring_new(s->data, s->length));
}

/* ================================================================
 * 25. str
 *
 *    str.len(s) / str.lower(s) / str.upper(s) / str.trim(s)
 *    str.split(s, delim) / str.replace(s, old, new)
 *    str.substr(s, start, len?) / str.contains(s, sub)
 *    str.index(s, sub) / str.starts(s, prefix) / str.ends(s, suffix)
 * ================================================================ */

static eka_value_t str_len(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    if (eka_obj_is_type(args[0], OBJ_STRING))
        return eka_int((int64_t)eka_as_string(args[0])->length);
    return eka_int(0);
}

static eka_value_t str_lower(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    if (!eka_obj_is_type(args[0], OBJ_STRING)) return args[0];
    eka_string_t *s = eka_as_string(args[0]);
    char *buf = malloc(s->length + 1);
    if (!buf) return args[0];
    for (size_t i = 0; i < s->length; i++) {
        char c = s->data[i];
        buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    buf[s->length] = '\0';
    return eka_string_val(eka_string_take(buf, s->length));
}

static eka_value_t str_upper(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    if (!eka_obj_is_type(args[0], OBJ_STRING)) return args[0];
    eka_string_t *s = eka_as_string(args[0]);
    char *buf = malloc(s->length + 1);
    if (!buf) return args[0];
    for (size_t i = 0; i < s->length; i++) {
        char c = s->data[i];
        buf[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    buf[s->length] = '\0';
    return eka_string_val(eka_string_take(buf, s->length));
}

static eka_value_t str_trim(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    if (!eka_obj_is_type(args[0], OBJ_STRING)) return args[0];
    eka_string_t *s = eka_as_string(args[0]);
    size_t start = 0, end = s->length;
    while (start < end && (s->data[start] == ' ' || s->data[start] == '\t' ||
           s->data[start] == '\n' || s->data[start] == '\r')) start++;
    while (end > start && (s->data[end-1] == ' ' || s->data[end-1] == '\t' ||
           s->data[end-1] == '\n' || s->data[end-1] == '\r')) end--;
    return eka_string_val(eka_string_new(s->data + start, end - start));
}

static eka_value_t str_split(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(2);
    if (!eka_obj_is_type(args[0], OBJ_STRING)) return eka_list_val(eka_list_new(0));
    const char *delim = arg_string(argc, args, 1, "");
    eka_string_t *s = eka_as_string(args[0]);
    size_t dlen = strlen(delim);

    eka_list_t *list = eka_list_new(8);
    if (dlen == 0) {
        /* Split by empty string → each character */
        for (size_t i = 0; i < s->length; i++) {
            eka_list_push(list, eka_string_val(eka_string_new(&s->data[i], 1)));
        }
        return eka_list_val(list);
    }

    size_t pos = 0;
    while (pos <= s->length) {
        const char *found = (const char *)memmem(s->data + pos, s->length - pos,
                                                  delim, dlen);
        size_t end = found ? (size_t)(found - s->data) : s->length;
        eka_list_push(list, eka_string_val(eka_string_new(s->data + pos, end - pos)));
        if (!found) break;
        pos = end + dlen;
    }
    return eka_list_val(list);
}

static eka_value_t str_replace(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(3);
    const char *haystack = arg_string(argc, args, 0, "");
    const char *needle   = arg_string(argc, args, 1, "");
    const char *repl     = arg_string(argc, args, 2, "");

    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    size_t rlen = strlen(repl);

    if (nlen == 0) return args[0];

    /* Count occurrences */
    size_t count = 0;
    const char *p = haystack;
    while ((p = strstr(p, needle))) { count++; p += nlen; }

    size_t out_len = hlen + count * (rlen > nlen ? rlen - nlen : 0) + 1;
    char *buf = malloc(out_len);
    if (!buf) return args[0];

    size_t wi = 0;
    const char *r = haystack;
    while (1) {
        const char *f = strstr(r, needle);
        if (!f) {
            size_t remaining = hlen - (size_t)(r - haystack);
            memcpy(buf + wi, r, remaining);
            wi += remaining;
            break;
        }
        size_t prefix = (size_t)(f - r);
        memcpy(buf + wi, r, prefix); wi += prefix;
        memcpy(buf + wi, repl, rlen); wi += rlen;
        r = f + nlen;
    }
    buf[wi] = '\0';
    return eka_string_val(eka_string_take(buf, wi));
}

static eka_value_t str_substr(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(2);
    if (!eka_obj_is_type(args[0], OBJ_STRING)) return eka_string_val(eka_string_new("", 0));
    eka_string_t *s = eka_as_string(args[0]);
    int64_t start = arg_int(argc, args, 1, 0);
    int64_t len = (argc >= 3) ? arg_int(argc, args, 2, (int64_t)s->length) : (int64_t)s->length;

    /* Clamp */
    if (start < 0) start += s->length;
    if (start < 0) start = 0;
    if ((uint64_t)start > s->length) start = s->length;
    if (len < 0) len = 0;
    if ((uint64_t)(start + len) > s->length) len = (int64_t)(s->length - (size_t)start);

    return eka_string_val(eka_string_new(s->data + start, (size_t)len));
}

static eka_value_t str_contains(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(2);
    const char *s = arg_string(argc, args, 0, "");
    const char *sub = arg_string(argc, args, 1, "");
    return eka_bool(strstr(s, sub) != NULL);
}

static eka_value_t str_index(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(2);
    const char *s = arg_string(argc, args, 0, "");
    const char *sub = arg_string(argc, args, 1, "");
    const char *found = strstr(s, sub);
    if (!found) return eka_nil();
    return eka_int((int64_t)(found - s));
}

static eka_value_t str_starts(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(2);
    const char *s = arg_string(argc, args, 0, "");
    const char *prefix = arg_string(argc, args, 1, "");
    size_t plen = strlen(prefix);
    return eka_bool(strncmp(s, prefix, plen) == 0);
}

static eka_value_t str_ends(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(2);
    const char *s = arg_string(argc, args, 0, "");
    const char *suffix = arg_string(argc, args, 1, "");
    size_t slen = strlen(s);
    size_t suflen = strlen(suffix);
    if (suflen > slen) return eka_bool(false);
    return eka_bool(memcmp(s + slen - suflen, suffix, suflen) == 0);
}

/* ================================================================
 * 6. crypto
 *
 *    crypto.sha256(str)       → hex string
 *    crypto.randomBytes(n)    → hex string
 * ================================================================ */

static eka_value_t crypto_sha256(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    const char *input = arg_string(argc, args, 0, "");
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)input, strlen(input), hash);

    char hex[SHA256_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(hex + i * 2, "%02x", hash[i]);
    }
    hex[SHA256_DIGEST_LENGTH * 2] = '\0';
    return eka_string_val(eka_string_new(hex, SHA256_DIGEST_LENGTH * 2));
}

static eka_value_t crypto_random_bytes(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    int64_t n = arg_int(argc, args, 0, 16);
    if (n < 0) n = 0;
    if (n > 1024) n = 1024; /* safety limit */

    unsigned char *buf = malloc((size_t)n);
    if (!buf) return eka_string_val(eka_string_new("", 0));
    RAND_bytes(buf, (int)n);

    char *hex = malloc((size_t)(n * 2 + 1));
    if (!hex) { free(buf); return eka_string_val(eka_string_new("", 0)); }
    for (int64_t i = 0; i < n; i++) {
        sprintf(hex + i * 2, "%02x", buf[i]);
    }
    hex[n * 2] = '\0';
    free(buf);
    return eka_string_val(eka_string_take(hex, (size_t)(n * 2)));
}

/* ================================================================
 * 11. env
 *
 *    env.get("KEY")             → string or nil
 *    env.get("KEY", "default")  → string
 * ================================================================ */

static eka_value_t env_get(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    const char *key = arg_string(argc, args, 0, NULL);
    if (!key) return eka_nil();

    const char *val = getenv(key);
    if (val) {
        return eka_string_val(eka_string_new(val, strlen(val)));
    }
    /* Return default if provided */
    if (argc >= 2) return args[1];
    return eka_nil();
}

/* ================================================================
 * 12. datetime
 *
 *    datetime.now()           → map {year, month, day, hour, min, sec, ...}
 *    datetime.now().format("YYYY-MM-DD HH:mm:ss") → string
 * ================================================================ */

static eka_value_t datetime_format(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args);

static eka_value_t datetime_now(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx; (void)argc; (void)args;
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);

    eka_map_t *map = eka_map_new(16);
    map_set_cstr(map, "year",   eka_int(tm->tm_year + 1900));
    map_set_cstr(map, "month",  eka_int(tm->tm_mon + 1));
    map_set_cstr(map, "day",    eka_int(tm->tm_mday));
    map_set_cstr(map, "hour",   eka_int(tm->tm_hour));
    map_set_cstr(map, "minute", eka_int(tm->tm_min));
    map_set_cstr(map, "second", eka_int(tm->tm_sec));
    map_set_cstr(map, "weekday", eka_int(tm->tm_wday));
    map_set_cstr(map, "timestamp", eka_int((int64_t)t));
    map_set_cstr(map, "format", make_native(datetime_format, NULL, "format"));

    return eka_map_val(map);
}

static eka_value_t datetime_format(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    const char *fmt = arg_string(argc, args, 0, "%Y-%m-%d %H:%M:%S");

    time_t t = time(NULL);
    struct tm *tm = localtime(&t);

    /* Simple format token replacement */
    char buf[128];
    size_t out = 0;
    for (const char *p = fmt; *p && out < sizeof(buf) - 1; p++) {
        if (*p == 'Y' && p[1] == 'Y' && p[2] == 'Y' && p[3] == 'Y') {
            out += (size_t)snprintf(buf + out, sizeof(buf) - out, "%04d", tm->tm_year + 1900);
            p += 3;
        } else if (*p == 'Y' && p[1] == 'Y') {
            out += (size_t)snprintf(buf + out, sizeof(buf) - out, "%02d", (tm->tm_year + 1900) % 100);
            p += 1;
        } else if (*p == 'M' && p[1] == 'M') {
            out += (size_t)snprintf(buf + out, sizeof(buf) - out, "%02d", tm->tm_mon + 1);
            p += 1;
        } else if (*p == 'D' && p[1] == 'D') {
            out += (size_t)snprintf(buf + out, sizeof(buf) - out, "%02d", tm->tm_mday);
            p += 1;
        } else if (*p == 'H' && p[1] == 'H') {
            out += (size_t)snprintf(buf + out, sizeof(buf) - out, "%02d", tm->tm_hour);
            p += 1;
        } else if (*p == 'm' && p[1] == 'm') {
            out += (size_t)snprintf(buf + out, sizeof(buf) - out, "%02d", tm->tm_min);
            p += 1;
        } else if (*p == 's' && p[1] == 's') {
            out += (size_t)snprintf(buf + out, sizeof(buf) - out, "%02d", tm->tm_sec);
            p += 1;
        } else {
            buf[out++] = *p;
        }
    }
    buf[out] = '\0';
    return eka_string_val(eka_string_new(buf, out));
}

/* ================================================================
 * 7. markdown
 *
 *    markdown.parse(str) → HTML string
 * ================================================================ */

static eka_value_t markdown_parse(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    const char *input = arg_string(argc, args, 0, "");
    char *html = cmark_markdown_to_html(input, strlen(input),
                                         CMARK_OPT_DEFAULT);
    if (!html) return eka_string_val(eka_string_new("", 0));
    eka_string_t *result = eka_string_take(html, strlen(html));
    return eka_string_val(result);
}

/* ================================================================
 * 17. cache
 *
 *    cache.set("key", value)         → nil
 *    cache.set("key", value, ttl)    → nil
 *    cache.get("key")                → value or nil
 *    cache.delete("key")             → nil
 *    cache.clear()                   → nil
 * ================================================================ */

static eka_value_t cache_set(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)ctx;
    CHECK_ARGC(2);
    if (!eka_obj_is_type(args[0], OBJ_STRING)) return eka_nil();
    eka_string_t *key = eka_as_string(args[0]);
    eka_map_set(vm->cache_store, key, args[1]);
    /* TTL ignored for V1 simplicity */
    (void)(argc >= 3 ? args[2] : eka_nil());
    return eka_nil();
}

static eka_value_t cache_get(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)ctx;
    CHECK_ARGC(1);
    if (!eka_obj_is_type(args[0], OBJ_STRING)) return eka_nil();
    eka_string_t *key = eka_as_string(args[0]);
    return eka_map_get(vm->cache_store, key);
}

static eka_value_t cache_delete(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)ctx;
    CHECK_ARGC(1);
    if (!eka_obj_is_type(args[0], OBJ_STRING)) return eka_nil();
    eka_string_t *key = eka_as_string(args[0]);
    eka_map_delete(vm->cache_store, key);
    return eka_nil();
}

static eka_value_t cache_clear(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)ctx; (void)argc; (void)args;
    /* Replace cache with fresh map */
    vm->cache_store = eka_map_new(64);
    return eka_nil();
}

/* ================================================================
 * 2. request (per-request)
 *
 *    request.path            → string
 *    request.method          → string
 *    request.query("key")    → string or nil
 *    request.query("key", d) → string
 *    request.header("name")  → string or nil
 * ================================================================ */

static eka_value_t request_query(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)ctx;
    CHECK_ARGC(1);
    if (!vm->current_req || !vm->current_req->query) {
        return (argc >= 2) ? args[1] : eka_nil();
    }

    const char *key = arg_string(argc, args, 0, NULL);
    if (!key) return eka_nil();

    /* Simple query string parsing: key=value&key2=value2 */
    const char *q = vm->current_req->query;
    size_t klen = strlen(key);

    while (*q) {
        /* Skip & or leading chars */
        if (*q == '&') { q++; continue; }

        const char *eq = strchr(q, '=');
        const char *end = strchr(q, '&');
        if (!end) end = q + strlen(q);

        size_t this_klen = eq ? (size_t)(eq - q) : (size_t)(end - q);
        if (this_klen == klen && strncmp(q, key, klen) == 0) {
            if (eq && eq + 1 < end) {
                /* URL-decode in place (simple version) */
                size_t vlen = (size_t)(end - eq - 1);
                char *decoded = malloc(vlen + 1);
                if (decoded) {
                    size_t di = 0;
                    for (size_t si = 0; si < vlen; si++) {
                        char c = eq[1 + si];
                        if (c == '%' && si + 2 < vlen) {
                            char hex[3] = {eq[1 + si + 1], eq[1 + si + 2], '\0'};
                            decoded[di++] = (char)strtol(hex, NULL, 16);
                            si += 2;
                        } else if (c == '+') {
                            decoded[di++] = ' ';
                        } else {
                            decoded[di++] = c;
                        }
                    }
                    decoded[di] = '\0';
                    return eka_string_val(eka_string_take(decoded, di));
                }
            }
            return eka_string_val(eka_string_new("", 0));
        }

        /* Advance */
        q = end;
    }

    return (argc >= 2) ? args[1] : eka_nil();
}

static eka_value_t request_header(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)ctx;
    CHECK_ARGC(1);
    if (!vm->current_req) return eka_nil();
    const char *name = arg_string(argc, args, 0, NULL);
    if (!name) return eka_nil();

    const char *val = eka_http_get_header(vm->current_req, name);
    if (val) return eka_string_val(eka_string_new(val, strlen(val)));
    return eka_nil();
}

static eka_value_t request_form(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args);
static eka_value_t request_json_fn(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args);
static eka_value_t request_param(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args);

/* ================================================================
 * 3. response (per-request)
 * ================================================================ */

static eka_value_t response_status(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)ctx;
    CHECK_ARGC(1);
    int64_t s = arg_int(argc, args, 0, 200);
    if (s < 100 || s > 599) s = 500;
    vm->response_state.status = (int)s;
    return eka_nil();
}

static eka_value_t response_redirect(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)ctx;
    CHECK_ARGC(1);
    const char *loc = arg_string(argc, args, 0, "/");
    int status = (int)arg_int(argc, args, 1, 302);

    vm->response_state.is_redirect = true;
    strncpy(vm->response_state.redirect_location, loc,
            sizeof(vm->response_state.redirect_location) - 1);
    vm->response_state.redirect_location[sizeof(vm->response_state.redirect_location) - 1] = '\0';
    vm->response_state.redirect_status = status;
    return eka_nil();
}

static eka_value_t response_header(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)ctx;
    CHECK_ARGC(2);
    const char *name = arg_string(argc, args, 0, NULL);
    const char *value = arg_string(argc, args, 1, "");
    if (!name) return eka_nil();

    if (vm->response_state.header_count < EKA_MAX_RESP_HEADERS) {
        int h = vm->response_state.header_count++;
        strncpy(vm->response_state.extra_headers[h].name, name, 63);
        vm->response_state.extra_headers[h].name[63] = '\0';
        strncpy(vm->response_state.extra_headers[h].value, value, 255);
        vm->response_state.extra_headers[h].value[255] = '\0';
    }
    return eka_nil();
}

static eka_value_t response_html(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)ctx;
    CHECK_ARGC(1);
    const char *body = arg_string(argc, args, 0, "");
    size_t len = strlen(body);

    vm->response_state.body_set = true;
    vm->response_state.body = malloc(len + 1);
    if (vm->response_state.body) {
        memcpy(vm->response_state.body, body, len + 1);
        vm->response_state.body_len = len;
    }
    strncpy(vm->response_state.content_type, "text/html; charset=utf-8", 63);
    vm->response_state.content_type_set = true;
    return eka_nil();
}

static eka_value_t response_json(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)ctx;
    CHECK_ARGC(1);

    /* Use json.stringify to convert the value */
    eka_value_t json_str = json_stringify_fn(vm, NULL, 1, args);
    if (!eka_obj_is_type(json_str, OBJ_STRING)) return eka_nil();

    eka_string_t *s = eka_as_string(json_str);
    vm->response_state.body_set = true;
    vm->response_state.body = malloc(s->length + 1);
    if (vm->response_state.body) {
        memcpy(vm->response_state.body, s->data, s->length + 1);
        vm->response_state.body_len = s->length;
    }
    strncpy(vm->response_state.content_type, "application/json", 63);
    vm->response_state.content_type_set = true;
    return eka_nil();
}

/* ================================================================
 * request.form(), request.json(), request.param() stubs
 * ================================================================ */

static eka_value_t request_form(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx; (void)argc; (void)args;
    /* TODO: parse application/x-www-form-urlencoded body */
    return eka_map_val(eka_map_new(4));
}

static eka_value_t request_json_fn(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)ctx; (void)argc; (void)args;
    if (!vm->current_req || !vm->current_req->body || vm->current_req->body_len == 0)
        return eka_nil();

    yyjson_doc *doc = yyjson_read_opts((char *)vm->current_req->body, vm->current_req->body_len, 0, NULL, NULL);
    if (!doc) return eka_nil();
    eka_value_t result = yyjson_to_eka(yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    return result;
}

static eka_value_t request_param(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    /* Route parameters not yet implemented */
    return (argc >= 2) ? args[1] : eka_nil();
}

static eka_value_t request_file(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx; (void)argc; (void)args;
    /* Multipart file upload not yet implemented */
    return eka_nil();
}

/* ================================================================
 * Registration
 * ================================================================ */

void eka_builtins_register(eka_vm_t *vm) {
    /* 1. print — standalone function */
    eka_vm_set_global(vm, "print", make_native(builtin_print, NULL, "print"));

    /* 4. sqlite — map with open method */
    {
        eka_map_t *sqlite_map = eka_map_new(4);
        map_set_cstr(sqlite_map, "open", make_native(sqlite_open, NULL, "open"));
        eka_vm_set_global(vm, "sqlite", eka_map_val(sqlite_map));
    }

    /* 5. json */
    {
        eka_map_t *json_map = eka_map_new(4);
        map_set_cstr(json_map, "parse",     make_native(json_parse_fn, NULL, "parse"));
        map_set_cstr(json_map, "stringify", make_native(json_stringify_fn, NULL, "stringify"));
        eka_vm_set_global(vm, "json", eka_map_val(json_map));
    }

    /* 8. html */
    {
        eka_map_t *html_map = eka_map_new(4);
        map_set_cstr(html_map, "escape", make_native(html_escape, NULL, "escape"));
        map_set_cstr(html_map, "raw",    make_native(html_raw, NULL, "raw"));
        eka_vm_set_global(vm, "html", eka_map_val(html_map));
    }

    /* 25. str */
    {
        eka_map_t *str_map = eka_map_new(16);
        map_set_cstr(str_map, "len",      make_native(str_len, NULL, "len"));
        map_set_cstr(str_map, "lower",    make_native(str_lower, NULL, "lower"));
        map_set_cstr(str_map, "upper",    make_native(str_upper, NULL, "upper"));
        map_set_cstr(str_map, "trim",     make_native(str_trim, NULL, "trim"));
        map_set_cstr(str_map, "split",    make_native(str_split, NULL, "split"));
        map_set_cstr(str_map, "replace",  make_native(str_replace, NULL, "replace"));
        map_set_cstr(str_map, "substr",   make_native(str_substr, NULL, "substr"));
        map_set_cstr(str_map, "contains", make_native(str_contains, NULL, "contains"));
        map_set_cstr(str_map, "index",    make_native(str_index, NULL, "index"));
        map_set_cstr(str_map, "starts",   make_native(str_starts, NULL, "starts"));
        map_set_cstr(str_map, "ends",     make_native(str_ends, NULL, "ends"));
        eka_vm_set_global(vm, "str", eka_map_val(str_map));
    }

    /* 6. crypto */
    {
        eka_map_t *crypto_map = eka_map_new(4);
        map_set_cstr(crypto_map, "sha256",      make_native(crypto_sha256, NULL, "sha256"));
        map_set_cstr(crypto_map, "randomBytes", make_native(crypto_random_bytes, NULL, "randomBytes"));
        eka_vm_set_global(vm, "crypto", eka_map_val(crypto_map));
    }

    /* 11. env */
    {
        eka_map_t *env_map = eka_map_new(4);
        map_set_cstr(env_map, "get", make_native(env_get, NULL, "get"));
        eka_vm_set_global(vm, "env", eka_map_val(env_map));
    }

    /* 12. datetime */
    {
        eka_map_t *dt_map = eka_map_new(4);
        map_set_cstr(dt_map, "now", make_native(datetime_now, NULL, "now"));
        eka_vm_set_global(vm, "datetime", eka_map_val(dt_map));
    }

    /* 7. markdown */
    {
        eka_map_t *md_map = eka_map_new(4);
        map_set_cstr(md_map, "parse", make_native(markdown_parse, NULL, "parse"));
        eka_vm_set_global(vm, "markdown", eka_map_val(md_map));
    }

    /* 17. cache */
    {
        eka_map_t *cache_map = eka_map_new(8);
        map_set_cstr(cache_map, "set",    make_native(cache_set, NULL, "set"));
        map_set_cstr(cache_map, "get",    make_native(cache_get, NULL, "get"));
        map_set_cstr(cache_map, "delete", make_native(cache_delete, NULL, "delete"));
        map_set_cstr(cache_map, "clear",  make_native(cache_clear, NULL, "clear"));
        eka_vm_set_global(vm, "cache", eka_map_val(cache_map));
    }
}

void eka_builtins_setup_request(eka_vm_t *vm, eka_http_request_t *req) {
    vm->current_req = req;

    /* Reset response state */
    memset(&vm->response_state, 0, sizeof(vm->response_state));
    vm->response_state.status = 200;

    /* Build the request map */
    eka_map_t *req_map = eka_map_new(16);
    if (req) {
        map_set_cstr(req_map, "method", eka_string_val(
            eka_string_new(req->method, strlen(req->method))));
        map_set_cstr(req_map, "path", eka_string_val(
            eka_string_new(req->path, strlen(req->path))));
    } else {
        map_set_cstr(req_map, "method", eka_string_val(eka_string_new("GET", 3)));
        map_set_cstr(req_map, "path",   eka_string_val(eka_string_new("/", 1)));
    }
    map_set_cstr(req_map, "query",  make_native(request_query, NULL, "query"));
    map_set_cstr(req_map, "form",   make_native(request_form, NULL, "form"));
    map_set_cstr(req_map, "json",   make_native(request_json_fn, NULL, "json"));
    map_set_cstr(req_map, "file",   make_native(request_file, NULL, "file"));
    map_set_cstr(req_map, "header", make_native(request_header, NULL, "header"));
    map_set_cstr(req_map, "param",  make_native(request_param, NULL, "param"));

    eka_vm_set_global(vm, "request", eka_map_val(req_map));

    /* Build the response map */
    eka_map_t *resp_map = eka_map_new(16);
    map_set_cstr(resp_map, "status",   make_native(response_status, NULL, "status"));
    map_set_cstr(resp_map, "redirect", make_native(response_redirect, NULL, "redirect"));
    map_set_cstr(resp_map, "header",   make_native(response_header, NULL, "header"));
    map_set_cstr(resp_map, "html",     make_native(response_html, NULL, "html"));
    map_set_cstr(resp_map, "json",     make_native(response_json, NULL, "json"));

    eka_vm_set_global(vm, "response", eka_map_val(resp_map));
}

void eka_builtins_teardown_request(eka_vm_t *vm) {
    vm->current_req = NULL;
    /* Free response body if allocated */
    if (vm->response_state.body) {
        free(vm->response_state.body);
        vm->response_state.body = NULL;
    }
}
