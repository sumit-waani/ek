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
#include <openssl/hmac.h>
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

/* XML/HTML escaping — returns malloc'd buffer, caller must free */
static char *xml_escape(const char *s) {
    size_t slen = strlen(s);
    /* Worst case: every char becomes 6-char entity */
    char *ebuf = malloc(slen * 6 + 1);
    if (!ebuf) return NULL;
    size_t out = 0;
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '&':  memcpy(ebuf + out, "&amp;", 5); out += 5; break;
        case '<':  memcpy(ebuf + out, "&lt;", 4);  out += 4; break;
        case '>':  memcpy(ebuf + out, "&gt;", 4);  out += 4; break;
        case '"':  memcpy(ebuf + out, "&quot;", 6); out += 6; break;
        default:   ebuf[out++] = *p; break;
        }
    }
    ebuf[out] = '\0';
    return ebuf;
}

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

static eka_value_t response_cookie(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)ctx;
    CHECK_ARGC(2);
    const char *name = arg_string(argc, args, 0, NULL);
    const char *value = arg_string(argc, args, 1, "");
    if (!name) return eka_nil();

    if (vm->response_state.cookie_count >= EKA_MAX_COOKIES) return eka_nil();
    int ci = vm->response_state.cookie_count++;

    strncpy(vm->response_state.cookies[ci].name, name, 63);
    vm->response_state.cookies[ci].name[63] = '\0';
    strncpy(vm->response_state.cookies[ci].value, value, 255);
    vm->response_state.cookies[ci].value[255] = '\0';

    /* Defaults */
    vm->response_state.cookies[ci].max_age = -1;  /* session cookie */
    vm->response_state.cookies[ci].http_only = true;
    vm->response_state.cookies[ci].secure = false;
    strcpy(vm->response_state.cookies[ci].same_site, "Lax");
    strcpy(vm->response_state.cookies[ci].path, "/");
    vm->response_state.cookies[ci].domain[0] = '\0';

    /* Options map (3rd arg) */
    if (argc >= 3 && eka_obj_is_type(args[2], OBJ_MAP)) {
        eka_map_t *opts = eka_as_map(args[2]);

        eka_string_t *k;
        eka_value_t v;

        k = eka_string_intern("maxAge", 6);
        v = eka_map_get(opts, k);
        if (eka_is_int(v)) vm->response_state.cookies[ci].max_age = (int)eka_as_int(v);
        else if (eka_is_number(v)) vm->response_state.cookies[ci].max_age = (int)eka_as_number(v);

        k = eka_string_intern("httpOnly", 8);
        v = eka_map_get(opts, k);
        if (eka_is_bool(v)) vm->response_state.cookies[ci].http_only = eka_as_bool(v);

        k = eka_string_intern("secure", 6);
        v = eka_map_get(opts, k);
        if (eka_is_bool(v)) vm->response_state.cookies[ci].secure = eka_as_bool(v);

        k = eka_string_intern("sameSite", 8);
        v = eka_map_get(opts, k);
        if (eka_obj_is_type(v, OBJ_STRING)) {
            strncpy(vm->response_state.cookies[ci].same_site,
                    eka_as_string(v)->data, 15);
            vm->response_state.cookies[ci].same_site[15] = '\0';
        }

        k = eka_string_intern("path", 4);
        v = eka_map_get(opts, k);
        if (eka_obj_is_type(v, OBJ_STRING)) {
            strncpy(vm->response_state.cookies[ci].path,
                    eka_as_string(v)->data, 127);
            vm->response_state.cookies[ci].path[127] = '\0';
        }

        k = eka_string_intern("domain", 6);
        v = eka_map_get(opts, k);
        if (eka_obj_is_type(v, OBJ_STRING)) {
            strncpy(vm->response_state.cookies[ci].domain,
                    eka_as_string(v)->data, 127);
            vm->response_state.cookies[ci].domain[127] = '\0';
        }
    }

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
    (void)ctx;
    if (argc < 1 || !eka_obj_is_type(args[0], OBJ_STRING)) {
        return (argc >= 2) ? args[1] : eka_nil();
    }
    eka_string_t *name = eka_as_string(args[0]);
    /* Search route params in VM */
    for (int i = 0; i < vm->route_param_count; i++) {
        if (vm->route_params[i].name_len == name->length &&
            memcmp(vm->route_params[i].name, name->data, name->length) == 0) {
            return eka_string_val(eka_string_new(vm->route_params[i].value,
                                                  vm->route_params[i].value_len));
        }
    }
    return (argc >= 2) ? args[1] : eka_nil();
}

static eka_value_t request_file(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx; (void)argc; (void)args;
    /* Multipart file upload not yet implemented */
    return eka_nil();
}

/* ================================================================
 * 9. http — outbound HTTP client (libcurl)
 *
 *    http.get(url)           → response map
 *    http.post(url, options) → response map
 *    http.put / delete / patch
 *
 *    Response: { status, body, headers (map), json() }
 * ================================================================ */

#include <curl/curl.h>

/* Simple write callback for curl: append to a dynamic buffer */
struct curl_buf {
    char *data;
    size_t len;
    size_t cap;
};

static size_t curl_write_cb(void *ptr, size_t sz, size_t nmemb, void *userdata) {
    struct curl_buf *buf = (struct curl_buf *)userdata;
    size_t total = sz * nmemb;
    if (buf->len + total + 1 > buf->cap) {
        size_t new_cap = buf->cap ? buf->cap * 2 : 4096;
        while (new_cap < buf->len + total + 1) new_cap *= 2;
        char *new_data = realloc(buf->data, new_cap);
        if (!new_data) return 0;
        buf->data = new_data;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

/* Header callback: collect response headers */
struct curl_hdr_buf {
    char *data;
    size_t len;
    size_t cap;
};

static size_t curl_hdr_cb(void *ptr, size_t sz, size_t nmemb, void *userdata) {
    struct curl_hdr_buf *hdr = (struct curl_hdr_buf *)userdata;
    size_t total = sz * nmemb;
    if (total < 2) return total;  /* skip empty lines */
    if (hdr->len + total + 1 > hdr->cap) {
        size_t new_cap = hdr->cap ? hdr->cap * 2 : 2048;
        while (new_cap < hdr->len + total + 1) new_cap *= 2;
        char *new_data = realloc(hdr->data, new_cap);
        if (!new_data) return 0;
        hdr->data = new_data;
        hdr->cap = new_cap;
    }
    memcpy(hdr->data + hdr->len, ptr, total);
    hdr->len += total;
    hdr->data[hdr->len] = '\0';
    return total;
}

/* Native json() for http response — ctx holds {data, len} */
static eka_value_t http_resp_json_fn(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)argc; (void)args;
    if (!ctx) return eka_nil();
    struct curl_resp_ctx { char *data; size_t len; } *jctx = ctx;
    yyjson_doc *doc = yyjson_read_opts(jctx->data, jctx->len, 0, NULL, NULL);
    if (!doc) return eka_nil();
    eka_value_t result = yyjson_to_eka(yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    return result;
}

static eka_value_t http_request(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    const char *url = arg_string(argc, args, 0, NULL);
    if (!url) return eka_nil();

    /* Method is implicit in which function was called; for now, use GET as default.
     * This is a generic request handler used by http.get/post/etc. via ctx. */
    CURL *curl = curl_easy_init();
    if (!curl) return eka_nil();

    struct curl_buf body_buf = {0};
    struct curl_hdr_buf hdr_buf = {0};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body_buf);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_hdr_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hdr_buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Eka/1.0");

    /* Set method via ctx (pointer-as-int encoding) */
    const char *method = (const char *)ctx;
    if (method && strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
    } else if (method) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    }

    /* For non-GET: read options map (arg #1) for body, headers, timeout */
    struct curl_slist *headers = NULL;
    if (argc >= 2 && eka_obj_is_type(args[1], OBJ_MAP)) {
        eka_map_t *opts = eka_as_map(args[1]);

        /* Body */
        eka_string_t *body_key = eka_string_intern("body", 4);
        eka_value_t body_val = eka_map_get(opts, body_key);
        if (eka_obj_is_type(body_val, OBJ_STRING)) {
            eka_string_t *b = eka_as_string(body_val);
            const char *bdata = b->data;
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)b->length);
            curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, bdata);
        }

        /* Headers map */
        eka_string_t *hdrs_key = eka_string_intern("headers", 7);
        eka_value_t hdrs_val = eka_map_get(opts, hdrs_key);
        if (eka_obj_is_type(hdrs_val, OBJ_MAP)) {
            eka_map_t *hdr_map = eka_as_map(hdrs_val);
            for (uint32_t i = 0; i < hdr_map->capacity; i++) {
                eka_map_entry_t *e = &hdr_map->entries[i];
                if (e->key && !eka_map_entry_is_tombstone(e->key)) {
                    eka_string_t *vs = eka_value_to_string(e->value);
                    size_t nlen = e->key->length + vs->length + 4;
                    char *hdr_line = malloc(nlen);
                    snprintf(hdr_line, nlen, "%s: %s", e->key->data, vs->data);
                    headers = curl_slist_append(headers, hdr_line);
                    free(hdr_line);
                }
            }
        }

        /* Timeout */
        eka_string_t *timeout_key = eka_string_intern("timeout", 7);
        eka_value_t timeout_val = eka_map_get(opts, timeout_key);
        if (eka_is_int(timeout_val)) {
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)eka_as_int(timeout_val));
        }
    }

    if (headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    CURLcode curl_res = curl_easy_perform(curl);

    /* Get status code */
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    (void)curl_res;

    /* Build response map */
    eka_map_t *resp = eka_map_new(8);
    map_set_cstr(resp, "status", eka_int((int64_t)status));

    if (body_buf.data) {
        map_set_cstr(resp, "body", eka_string_val(
            eka_string_take(body_buf.data, body_buf.len)));
    } else {
        map_set_cstr(resp, "body", eka_string_val(eka_string_new("", 0)));
    }

    /* Parse headers into a map */
    eka_map_t *resp_headers = eka_map_new(8);
    if (hdr_buf.data) {
        /* Parse "Name: value\r\n" lines */
        char *h = hdr_buf.data;
        char *end = hdr_buf.data + hdr_buf.len;
        while (h < end) {
            char *line_end = strstr(h, "\r\n");
            if (!line_end) line_end = end;
            char *colon = (char *)memchr(h, ':', (size_t)(line_end - h));
            if (colon && colon < line_end) {
                size_t nlen = (size_t)(colon - h);
                char *val = colon + 1;
                while (val < line_end && *val == ' ') val++;
                size_t vlen = (size_t)(line_end - val);
                if (nlen > 0 && vlen > 0) {
                    eka_string_t *hname = eka_string_new(h, nlen);
                    eka_map_set(resp_headers, hname,
                                eka_string_val(eka_string_new(val, vlen)));
                }
            }
            h = line_end + 2;
            if (h >= end) break;
        }
        free(hdr_buf.data);
    }
    map_set_cstr(resp, "headers", eka_map_val(resp_headers));

    /* json() method as native — context holds body string pointer */
    {
        struct curl_resp_ctx { char *data; size_t len; } *jctx =
            malloc(sizeof(struct curl_resp_ctx));
        jctx->data = body_buf.data;  /* transfer ownership */
        jctx->len = body_buf.len;
        body_buf.data = NULL;  /* prevent double-free */
        map_set_cstr(resp, "json", make_native(http_resp_json_fn, jctx, "json"));
    }

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return eka_map_val(resp);
}

static eka_value_t http_get_fn(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    return http_request(vm, (void *)"GET", argc, args);
}
static eka_value_t http_post_fn(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    return http_request(vm, (void *)"POST", argc, args);
}
static eka_value_t http_put_fn(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    return http_request(vm, (void *)"PUT", argc, args);
}
static eka_value_t http_delete_fn(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    return http_request(vm, (void *)"DELETE", argc, args);
}
static eka_value_t http_patch_fn(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    return http_request(vm, (void *)"PATCH", argc, args);
}

/* ================================================================
 * 10. fs — sandboxed file system
 *
 *    fs.read / write / append / move / copy / exists / delete / mkdir / list
 *
 *    All paths are confined to the project root (directory of the .eka file).
 *    Symlinks are resolved via realpath() — symlink escapes are blocked.
 * ================================================================ */

/* Project root — set once at startup via eka_fs_set_project_root() */
static char fs_project_root[4096];
static size_t fs_project_root_len;

#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Set the project root from the .eka file path.
 * Extracts the directory, resolves to absolute path. */
void eka_fs_set_project_root(const char *filepath) {
    if (!filepath || !filepath[0]) {
        /* Fallback: use current working directory */
        if (getcwd(fs_project_root, sizeof(fs_project_root))) {
            fs_project_root_len = strlen(fs_project_root);
        }
        return;
    }

    /* Extract directory from file path */
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", filepath);

    /* Find last slash */
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
    } else {
        /* No slash — file is in current directory */
        strcpy(dir, ".");
    }

    /* Resolve to absolute path */
    char resolved[4096];
    if (realpath(dir, resolved)) {
        snprintf(fs_project_root, sizeof(fs_project_root), "%s", resolved);
        fs_project_root_len = strlen(fs_project_root);
    } else {
        /* If resolution fails, try CWD */
        if (getcwd(fs_project_root, sizeof(fs_project_root))) {
            fs_project_root_len = strlen(fs_project_root);
        }
    }
}

/* Resolve a user-supplied relative path against the project root.
 * Returns a malloc'd absolute path on success, NULL if path is blocked.
 *
 * Resolution strategy:
 * 1. Reject NULL, empty, absolute paths, and paths containing ".."
 * 2. Join project_root + "/" + user_path
 * 3. Try realpath() — works if path exists (resolves symlinks)
 * 4. If path doesn't exist: realpath() the parent, append basename
 * 5. Check resolved path starts with project_root
 */
static char *fs_resolve_path(const char *user_path) {
    if (!user_path || !user_path[0]) return NULL;

    /* Reject absolute paths */
    if (user_path[0] == '/') return NULL;

    /* Reject .. components — we check before resolution to block
     * obvious traversal attempts early. Realpath handles the rest. */
    if (strstr(user_path, "..") != NULL) return NULL;

    /* If project root not set, allow paths as-is (test mode) */
    if (fs_project_root_len == 0) {
        char *dup = strdup(user_path);
        return dup;
    }

    /* Build full path: project_root + "/" + user_path */
    char full_path[PATH_MAX];
    int n = snprintf(full_path, sizeof(full_path), "%s/%s",
                     fs_project_root, user_path);
    if (n < 0 || (size_t)n >= sizeof(full_path)) return NULL;

    /* Try realpath — resolves symlinks, normalizes . and .. */
    char resolved[PATH_MAX];
    if (realpath(full_path, resolved)) {
        /* Path exists and resolved — check confinement */
        if (strncmp(resolved, fs_project_root, fs_project_root_len) != 0) {
            return NULL;  /* escaped project root */
        }
        /* Must be exactly root or start with root + "/" */
        if (resolved[fs_project_root_len] != '\0' &&
            resolved[fs_project_root_len] != '/') {
            return NULL;
        }
        return strdup(resolved);
    }

    /* Path doesn't exist yet (write target) — resolve parent directory */
    char parent[PATH_MAX];
    char fname[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", full_path);

    /* Split into parent dir + filename */
    char *last_slash = strrchr(parent, '/');
    if (!last_slash) return NULL;
    *last_slash = '\0';
    snprintf(fname, sizeof(fname), "%s", last_slash + 1);

    /* Resolve parent */
    char parent_resolved[PATH_MAX];
    if (!realpath(parent, parent_resolved)) return NULL;

    /* Check parent is within project root */
    if (strncmp(parent_resolved, fs_project_root, fs_project_root_len) != 0) {
        return NULL;
    }
    if (parent_resolved[fs_project_root_len] != '\0' &&
        parent_resolved[fs_project_root_len] != '/') {
        return NULL;
    }

    /* Build final path: resolved_parent + "/" + filename */
    char *result = malloc(strlen(parent_resolved) + 1 + strlen(fname) + 1);
    if (!result) return NULL;
    sprintf(result, "%s/%s", parent_resolved, fname);
    return result;
}

/* Free a resolved path (alias for free, for clarity) */
static void fs_free_path(char *path) {
    free(path);
}

static eka_value_t fs_read(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    const char *user_path = arg_string(argc, args, 0, NULL);
    char *path = fs_resolve_path(user_path);
    if (!path) return eka_nil();

    FILE *f = fopen(path, "rb");
    fs_free_path(path);
    if (!f) return eka_nil();
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 10 * 1024 * 1024) { fclose(f); return eka_nil(); }  /* 10MB limit */
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return eka_nil(); }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return eka_string_val(eka_string_take(buf, n));
}

static eka_value_t fs_write(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(2);
    const char *user_path = arg_string(argc, args, 0, NULL);
    const char *content = arg_string(argc, args, 1, "");
    char *path = fs_resolve_path(user_path);
    if (!path) return eka_nil();
    FILE *f = fopen(path, "wb");
    fs_free_path(path);
    if (!f) return eka_nil();
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return eka_nil();
}

static eka_value_t fs_append(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(2);
    const char *user_path = arg_string(argc, args, 0, NULL);
    const char *content = arg_string(argc, args, 1, "");
    char *path = fs_resolve_path(user_path);
    if (!path) return eka_nil();
    FILE *f = fopen(path, "ab");
    fs_free_path(path);
    if (!f) return eka_nil();
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return eka_nil();
}

static eka_value_t fs_move(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(2);
    const char *from_user = arg_string(argc, args, 0, NULL);
    const char *to_user = arg_string(argc, args, 1, NULL);
    char *from = fs_resolve_path(from_user);
    if (!from) return eka_nil();
    char *to = fs_resolve_path(to_user);
    if (!to) { fs_free_path(from); return eka_nil(); }
    rename(from, to);
    fs_free_path(from);
    fs_free_path(to);
    return eka_nil();
}

static eka_value_t fs_copy(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(2);
    const char *from_user = arg_string(argc, args, 0, NULL);
    const char *to_user = arg_string(argc, args, 1, NULL);
    char *from = fs_resolve_path(from_user);
    if (!from) return eka_nil();
    char *to = fs_resolve_path(to_user);
    if (!to) { fs_free_path(from); return eka_nil(); }

    FILE *src = fopen(from, "rb");
    if (!src) { fs_free_path(from); fs_free_path(to); return eka_nil(); }
    FILE *dst = fopen(to, "wb");
    if (!dst) { fclose(src); fs_free_path(from); fs_free_path(to); return eka_nil(); }

    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        fwrite(buf, 1, n, dst);
    }
    fclose(src); fclose(dst);
    fs_free_path(from);
    fs_free_path(to);
    return eka_nil();
}

static eka_value_t fs_exists(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    const char *user_path = arg_string(argc, args, 0, NULL);
    char *path = fs_resolve_path(user_path);
    if (!path) return eka_bool(false);
    bool exists = (access(path, F_OK) == 0);
    fs_free_path(path);
    return eka_bool(exists);
}

static eka_value_t fs_delete(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    const char *user_path = arg_string(argc, args, 0, NULL);
    char *path = fs_resolve_path(user_path);
    if (!path) return eka_nil();
    unlink(path);
    fs_free_path(path);
    return eka_nil();
}

static eka_value_t fs_mkdir(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    const char *user_path = arg_string(argc, args, 0, NULL);
    char *path = fs_resolve_path(user_path);
    if (!path) return eka_nil();
    mkdir(path, 0755);
    fs_free_path(path);
    return eka_nil();
}

static eka_value_t fs_list(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    const char *user_path = arg_string(argc, args, 0, ".");
    char *path = fs_resolve_path(user_path);
    if (!path) return eka_list_val(eka_list_new(0));

    DIR *d = opendir(path);
    if (!d) { fs_free_path(path); return eka_list_val(eka_list_new(0)); }
    eka_list_t *list = eka_list_new(16);
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        eka_list_push(list, eka_string_val(eka_string_new(ent->d_name, strlen(ent->d_name))));
    }
    closedir(d);
    fs_free_path(path);
    return eka_list_val(list);
}

/* ================================================================
 * 14. base64 (via OpenSSL EVP)
 * ================================================================ */

#include <openssl/evp.h>

static eka_value_t base64_encode(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    const char *input = arg_string(argc, args, 0, "");
    size_t in_len = strlen(input);

    size_t out_len = ((in_len + 2) / 3) * 4 + 1;
    char *buf = malloc(out_len);
    if (!buf) return eka_nil();

    int actual_len = 0;
    EVP_ENCODE_CTX *ctx_evp = EVP_ENCODE_CTX_new();
    EVP_EncodeInit(ctx_evp);
    EVP_EncodeUpdate(ctx_evp, (unsigned char *)buf, &actual_len,
                     (const unsigned char *)input, (int)in_len);
    int tmp = 0;
    EVP_EncodeFinal(ctx_evp, (unsigned char *)(buf + actual_len), &tmp);
    actual_len += tmp;
    EVP_ENCODE_CTX_free(ctx_evp);

    buf[actual_len] = '\0';
    return eka_string_val(eka_string_take(buf, (size_t)actual_len));
}

static eka_value_t base64_decode(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    const char *input = arg_string(argc, args, 0, "");
    size_t in_len = strlen(input);

    char *buf = malloc(in_len + 1);
    if (!buf) return eka_nil();

    int actual_len = 0;
    EVP_ENCODE_CTX *ctx_evp = EVP_ENCODE_CTX_new();
    EVP_DecodeInit(ctx_evp);
    EVP_DecodeUpdate(ctx_evp, (unsigned char *)buf, &actual_len,
                     (const unsigned char *)input, (int)in_len);
    int tmp = 0;
    EVP_DecodeFinal(ctx_evp, (unsigned char *)(buf + actual_len), &tmp);
    actual_len += tmp;
    EVP_ENCODE_CTX_free(ctx_evp);

    if (actual_len < 0) { free(buf); return eka_nil(); }
    buf[actual_len] = '\0';
    return eka_string_val(eka_string_take(buf, (size_t)actual_len));
}

/* ================================================================
 * 15. url
 * ================================================================ */

static eka_value_t url_parse(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    const char *url = arg_string(argc, args, 0, "");
    eka_map_t *map = eka_map_new(8);

    /* Simple URL parser */
    const char *p = url;

    /* Scheme */
    const char *scheme_start = p;
    while (*p && *p != ':' && *p != '/') p++;
    if (*p == ':' && p[1] == '/' && p[2] == '/') {
        map_set_cstr(map, "scheme", eka_string_val(
            eka_string_new(scheme_start, (size_t)(p - scheme_start))));
        p += 3; /* skip :// */
    } else {
        map_set_cstr(map, "scheme", eka_string_val(eka_string_new("", 0)));
        p = url;
    }

    /* Host */
    const char *host_start = p;
    while (*p && *p != '/' && *p != '?' && *p != '#' && *p != ':') p++;
    map_set_cstr(map, "host", eka_string_val(
        eka_string_new(host_start, (size_t)(p - host_start))));

    /* Path */
    const char *path_start = p;
    while (*p && *p != '?' && *p != '#') p++;
    size_t path_len = (size_t)(p - path_start);
    if (path_len == 0) {
        map_set_cstr(map, "path", eka_string_val(eka_string_new("/", 1)));
    } else {
        map_set_cstr(map, "path", eka_string_val(eka_string_new(path_start, path_len)));
    }

    /* Query */
    if (*p == '?') {
        p++;
        const char *q_start = p;
        while (*p && *p != '#') p++;
        eka_map_t *query = eka_map_new(8);
        /* Parse key=value pairs */
        const char *qs = q_start;
        while (qs < p) {
            const char *eq = (const char *)memchr(qs, '=', (size_t)(p - qs));
            const char *amp = (const char *)memchr(qs, '&', (size_t)(p - qs));
            if (!amp) amp = p;
            if (eq && eq < amp) {
                eka_string_t *k = eka_string_new(qs, (size_t)(eq - qs));
                eka_string_t *v = eka_string_new(eq + 1, (size_t)(amp - eq - 1));
                eka_map_set(query, k, eka_string_val(v));
            }
            qs = amp + 1;
        }
        map_set_cstr(map, "query", eka_map_val(query));
    } else {
        map_set_cstr(map, "query", eka_map_val(eka_map_new(4)));
    }

    /* Fragment */
    if (*p == '#') {
        p++;
        map_set_cstr(map, "fragment", eka_string_val(eka_string_new(p, strlen(p))));
    } else {
        map_set_cstr(map, "fragment", eka_nil());
    }

    return eka_map_val(map);
}

static eka_value_t url_build(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    if (!eka_obj_is_type(args[0], OBJ_MAP)) return eka_nil();
    eka_map_t *m = eka_as_map(args[0]);

    char buf[2048];
    size_t out = 0;

    /* Scheme */
    eka_string_t *scheme_key = eka_string_intern("scheme", 6);
    eka_value_t scheme = eka_map_get(m, scheme_key);
    if (eka_obj_is_type(scheme, OBJ_STRING)) {
        eka_string_t *s = eka_as_string(scheme);
        out += (size_t)snprintf(buf + out, sizeof(buf) - out, "%.*s://",
                                (int)s->length, s->data);
    }

    /* Host */
    eka_string_t *host_key = eka_string_intern("host", 4);
    eka_value_t host = eka_map_get(m, host_key);
    if (eka_obj_is_type(host, OBJ_STRING)) {
        eka_string_t *s = eka_as_string(host);
        out += (size_t)snprintf(buf + out, sizeof(buf) - out, "%.*s",
                                (int)s->length, s->data);
    }

    /* Path */
    eka_string_t *path_key = eka_string_intern("path", 4);
    eka_value_t path = eka_map_get(m, path_key);
    if (eka_obj_is_type(path, OBJ_STRING)) {
        eka_string_t *s = eka_as_string(path);
        out += (size_t)snprintf(buf + out, sizeof(buf) - out, "%.*s",
                                (int)s->length, s->data);
    }

    /* Query (map → key=value&...) */
    eka_string_t *query_key = eka_string_intern("query", 5);
    eka_value_t query = eka_map_get(m, query_key);
    if (eka_obj_is_type(query, OBJ_MAP)) {
        eka_map_t *qm = eka_as_map(query);
        bool first = true;
        for (uint32_t i = 0; i < qm->capacity; i++) {
            eka_map_entry_t *e = &qm->entries[i];
            if (e->key && !eka_map_entry_is_tombstone(e->key)) {
                eka_string_t *vs = eka_value_to_string(e->value);
                out += (size_t)snprintf(buf + out, sizeof(buf) - out,
                    "%s%.*s=%.*s",
                    first ? "?" : "&",
                    (int)e->key->length, e->key->data,
                    (int)vs->length, vs->data);
                first = false;
            }
        }
    }

    buf[out] = '\0';
    return eka_string_val(eka_string_new(buf, out));
}

/* ================================================================
 * 16. session — cookie-based sessions (SQLite backend)
 *
 * Lifecycle:
 *   1. Server parses Cookie header → extracts eka_session=<id>
 *   2. Server loads session from SQLite into vm->session_data (map)
 *   3. Route handler calls session.get/set/delete/clear (mutate map)
 *   4. Server saves vm->session_data back to SQLite if dirty
 *   5. Server sets Set-Cookie header if session is new or dirty
 *
 * The builtins below operate on vm->session_data (in-memory map).
 * They set vm->session_dirty = true on mutations.
 * ================================================================ */

/* Initialize session DB — creates the eka_sessions table if it doesn't exist.
 * Called once at server startup. Returns the sqlite3* handle. */
void eka_session_init_db(eka_vm_t *vm) {
    if (vm->session_db) return;

    sqlite3 *db = NULL;
    int rc = sqlite3_open("eka_sessions.db", &db);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }

    /* WAL mode for concurrency */
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);

    /* Create table */
    const char *sql =
        "CREATE TABLE IF NOT EXISTS eka_sessions ("
        "  session_id TEXT PRIMARY KEY,"
        "  data TEXT NOT NULL DEFAULT '{}',"
        "  created_at INTEGER NOT NULL,"
        "  expires_at INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_eka_sessions_expires "
        "ON eka_sessions(expires_at);";
    char *err = NULL;
    rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (err) sqlite3_free(err);

    vm->session_db = db;
}

/* Load session data from SQLite into vm->session_data.
 * If session_id is empty or not found, creates an empty map.
 * Sets vm->session_is_new if no existing session found. */
void eka_session_load(eka_vm_t *vm) {
    vm->session_data = eka_map_new(8);
    vm->session_dirty = false;
    vm->session_is_new = true;

    if (vm->session_id[0] == '\0') return;

    eka_session_init_db(vm);
    sqlite3 *db = (sqlite3 *)vm->session_db;
    if (!db) return;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT data FROM eka_sessions WHERE session_id = ? AND expires_at > ?",
            -1, &stmt, NULL) != SQLITE_OK) {
        return;
    }

    int64_t now = (int64_t)time(NULL);
    sqlite3_bind_text(stmt, 1, vm->session_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, now);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *json_str = (const char *)sqlite3_column_text(stmt, 0);
        int json_len = sqlite3_column_bytes(stmt, 0);

        /* Parse JSON into the session map */
        yyjson_doc *doc = yyjson_read_opts((char *)json_str, (size_t)json_len, 0, NULL, NULL);
        if (doc) {
            yyjson_val *root = yyjson_doc_get_root(doc);
            if (yyjson_get_type(root) == YYJSON_TYPE_OBJ) {
                yyjson_val *key, *val;
                yyjson_obj_iter iter;
                yyjson_obj_iter_init(root, &iter);
                while ((key = yyjson_obj_iter_next(&iter))) {
                    val = yyjson_obj_iter_get_val(key);
                    const char *kstr = yyjson_get_str(key);
                    size_t klen = yyjson_get_len(key);
                    eka_string_t *ekey = eka_string_intern(kstr, klen);

                    /* Convert value — support string, int, number, bool, nil */
                    eka_value_t eka_val;
                    switch (yyjson_get_type(val)) {
                    case YYJSON_TYPE_STR:
                        eka_val = eka_string_val(eka_string_new(
                            yyjson_get_str(val), yyjson_get_len(val)));
                        break;
                    case YYJSON_TYPE_NUM:
                        if (yyjson_get_subtype(val) == YYJSON_SUBTYPE_UINT)
                            eka_val = eka_int((int64_t)yyjson_get_uint(val));
                        else {
                            double d = yyjson_get_real(val);
                            if (d == (double)(int64_t)d)
                                eka_val = eka_int((int64_t)d);
                            else
                                eka_val = eka_number(d);
                        }
                        break;
                    case YYJSON_TYPE_BOOL:
                        eka_val = eka_bool(yyjson_get_bool(val));
                        break;
                    default:
                        eka_val = eka_nil();
                        break;
                    }
                    eka_map_set(vm->session_data, ekey, eka_val);
                }
                vm->session_is_new = false;
            }
            yyjson_doc_free(doc);
        }
    }

    sqlite3_finalize(stmt);
}

/* Save vm->session_data to SQLite.
 * Generates a new session_id if session_is_new. */
void eka_session_save(eka_vm_t *vm) {
    if (!vm->session_dirty && !vm->session_is_new) return;
    if (!vm->session_data) return;

    eka_session_init_db(vm);
    sqlite3 *db = (sqlite3 *)vm->session_db;
    if (!db) return;

    /* Generate session ID for new sessions */
    if (vm->session_id[0] == '\0') {
        unsigned char buf[16];
        RAND_bytes(buf, sizeof(buf));
        for (int i = 0; i < 16; i++)
            sprintf(vm->session_id + i * 2, "%02x", buf[i]);
        vm->session_id[32] = '\0';
    }

    /* Serialize session map to JSON */
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    eka_map_t *map = vm->session_data;
    for (uint32_t i = 0; i < map->capacity; i++) {
        eka_map_entry_t *e = &map->entries[i];
        if (e->key && !eka_map_entry_is_tombstone(e->key)) {
            yyjson_mut_val *jval;
            if (eka_is_nil(e->value)) {
                jval = yyjson_mut_null(doc);
            } else if (eka_is_bool(e->value)) {
                jval = yyjson_mut_bool(doc, eka_as_bool(e->value));
            } else if (eka_is_int(e->value)) {
                jval = yyjson_mut_int(doc, eka_as_int(e->value));
            } else if (eka_is_number(e->value)) {
                jval = yyjson_mut_real(doc, eka_as_number(e->value));
            } else if (eka_obj_is_type(e->value, OBJ_STRING)) {
                eka_string_t *s = eka_as_string(e->value);
                jval = yyjson_mut_strncpy(doc, s->data, s->length);
            } else {
                /* Skip non-primitive values (lists, maps, etc.) */
                continue;
            }
            yyjson_mut_obj_add(obj,
                yyjson_mut_strncpy(doc, e->key->data, e->key->length),
                jval);
        }
    }
    yyjson_mut_doc_set_root(doc, obj);

    size_t json_len;
    char *json_str = yyjson_mut_write(doc, 0, &json_len);

    int64_t now = (int64_t)time(NULL);
    int64_t expires = now + EKA_SESSION_TTL;

    /* Upsert: INSERT OR REPLACE */
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO eka_sessions (session_id, data, created_at, expires_at) "
            "VALUES (?, ?, ?, ?)",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, vm->session_id, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, json_str, (int)json_len, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, now);
        sqlite3_bind_int64(stmt, 4, expires);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    if (json_str) free(json_str);
    yyjson_mut_doc_free(doc);

    /* Periodic cleanup: 1% chance to purge expired sessions */
    if ((rand() % 100) == 0) {
        char *err = NULL;
        sqlite3_exec(db,
            "DELETE FROM eka_sessions WHERE expires_at < strftime('%s','now')",
            NULL, NULL, &err);
        if (err) sqlite3_free(err);
    }
}

static eka_value_t session_set(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    CHECK_ARGC(2);
    if (!eka_obj_is_type(args[0], OBJ_STRING)) return eka_nil();
    if (!vm->session_data) return eka_nil();

    eka_string_t *key = eka_as_string(args[0]);
    eka_map_set(vm->session_data, key, args[1]);
    vm->session_dirty = true;
    return eka_nil();
}

static eka_value_t session_get(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    CHECK_ARGC(1);
    if (!eka_obj_is_type(args[0], OBJ_STRING)) return eka_nil();
    if (!vm->session_data) return argc >= 2 ? args[1] : eka_nil();

    eka_string_t *key = eka_as_string(args[0]);
    eka_value_t val = eka_map_get(vm->session_data, key);
    if (eka_is_nil(val) && argc >= 2) return args[1];
    return val;
}

static eka_value_t session_delete(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    CHECK_ARGC(1);
    if (!eka_obj_is_type(args[0], OBJ_STRING)) return eka_nil();
    if (!vm->session_data) return eka_nil();

    eka_string_t *key = eka_as_string(args[0]);
    eka_map_delete(vm->session_data, key);
    vm->session_dirty = true;
    return eka_nil();
}

static eka_value_t session_clear(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)argc; (void)args;

    /* Delete the session row from SQLite */
    if (vm->session_db && vm->session_id[0] != '\0') {
        sqlite3 *db = (sqlite3 *)vm->session_db;
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db,
                "DELETE FROM eka_sessions WHERE session_id = ?",
                -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, vm->session_id, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    /* Reset in-memory state — replace with fresh empty map */
    vm->session_data = eka_map_new(4);
    vm->session_id[0] = '\0';
    vm->session_dirty = false;
    vm->session_is_new = true;
    return eka_nil();
}

static eka_value_t session_csrf(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)ctx; (void)argc; (void)args;
    /* Generate a CSRF token and store it in the session */
    unsigned char buf[16];
    RAND_bytes(buf, sizeof(buf));
    char hex[33];
    for (int i = 0; i < 16; i++) sprintf(hex + i*2, "%02x", buf[i]);
    hex[32] = '\0';

    /* Store the token in the session for later verification */
    if (vm->session_data) {
        eka_string_t *key = eka_string_intern("_csrf", 5);
        eka_map_set(vm->session_data, key,
                    eka_string_val(eka_string_new(hex, 32)));
        vm->session_dirty = true;
    }

    return eka_string_val(eka_string_new(hex, 32));
}

/* ================================================================
 * 18. email — SMTP via libcurl
 * ================================================================ */

static eka_value_t email_send(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    if (!eka_obj_is_type(args[0], OBJ_MAP)) return eka_nil();

    eka_map_t *opts = eka_as_map(args[0]);
    const char *to = NULL, *from = NULL, *subject = NULL, *body = NULL, *smtp_server = NULL;
    const char *cc = NULL, *bcc = NULL, *html_body = NULL;

    /* Extract fields */
    #define EMAIL_GET_STR(dest, fname) do { \
        eka_string_t *k = eka_string_intern(#fname, strlen(#fname)); \
        eka_value_t v = eka_map_get(opts, k); \
        if (eka_obj_is_type(v, OBJ_STRING)) dest = eka_as_string(v)->data; \
    } while(0)

    EMAIL_GET_STR(to, to);
    EMAIL_GET_STR(from, from);
    EMAIL_GET_STR(subject, subject);
    EMAIL_GET_STR(body, body);
    EMAIL_GET_STR(html_body, html);
    EMAIL_GET_STR(cc, cc);
    EMAIL_GET_STR(bcc, bcc);
    #undef EMAIL_GET_STR

    /* The options map uses "smtp_server" key, so check that too */
    {
        eka_string_t *k = eka_string_intern("smtp", 4);
        eka_value_t v = eka_map_get(opts, k);
        if (eka_obj_is_type(v, OBJ_STRING)) smtp_server = eka_as_string(v)->data;
    }

    if (!to || !from || !subject || !body || !smtp_server) return eka_nil();

    /* Build the email */
    CURL *curl = curl_easy_init();
    if (!curl) return eka_nil();

    struct curl_slist *recipients = NULL;
    recipients = curl_slist_append(recipients, to);
    if (cc && cc[0]) recipients = curl_slist_append(recipients, cc);
    if (bcc && bcc[0]) recipients = curl_slist_append(recipients, bcc);

    /* Build email body with headers */
    char email_buf[16384];
    int email_len = snprintf(email_buf, sizeof(email_buf),
        "From: %s\r\n"
        "To: %s\r\n"
        "Subject: %s\r\n"
        "%s\r\n"
        "%s%s",
        from, to, subject,
        html_body && html_body[0] ? "Content-Type: text/html; charset=UTF-8" :
                                    "Content-Type: text/plain; charset=UTF-8",
        body ? body : "",
        html_body && html_body[0] ? html_body : "");
    if (email_len < 0 || (size_t)email_len >= sizeof(email_buf)) {
        curl_slist_free_all(recipients);
        curl_easy_cleanup(curl);
        return eka_nil();  /* email too large */
    }

    char smtp_url[256];
    snprintf(smtp_url, sizeof(smtp_url), "smtp://%s", smtp_server);

    curl_easy_setopt(curl, CURLOPT_URL, smtp_url);
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, from);
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READDATA, email_buf);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE, (long)email_len);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode curl_res2 = curl_easy_perform(curl);
    (void)curl_res2;
    curl_slist_free_all(recipients);
    curl_easy_cleanup(curl);

    return eka_nil();
}

static eka_value_t validate_match(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args);

/* Forward declare regex_test for validate.match */
static eka_value_t regex_test(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args);

/* ================================================================
 * 19. validate
 * ================================================================ */

static eka_value_t validate_email(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    const char *s = arg_string(argc, args, 0, "");
    /* Simple email validation: contains @ and . after @, no spaces */
    const char *at = strchr(s, '@');
    if (!at || at == s) return eka_bool(false);
    const char *dot = strchr(at + 1, '.');
    if (!dot || dot == at + 1 || dot[1] == '\0') return eka_bool(false);
    if (strchr(s, ' ')) return eka_bool(false);
    return eka_bool(true);
}

static eka_value_t validate_url_fn(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    const char *s = arg_string(argc, args, 0, "");
    /* Simple URL validation: has :// and something after */
    const char *colon = strstr(s, "://");
    return eka_bool(colon != NULL && colon[3] != '\0' && colon[3] != '/');
}

static eka_value_t validate_required(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    if (eka_is_nil(args[0])) return eka_bool(false);
    if (eka_obj_is_type(args[0], OBJ_STRING))
        return eka_bool(eka_as_string(args[0])->length > 0);
    if (eka_is_bool(args[0])) return args[0];
    return eka_bool(true);
}

static eka_value_t validate_min_length(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(2);
    if (!eka_obj_is_type(args[0], OBJ_STRING)) return eka_bool(false);
    int64_t min = arg_int(argc, args, 1, 0);
    return eka_bool((int64_t)eka_as_string(args[0])->length >= min);
}

static eka_value_t validate_max_length(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(2);
    if (!eka_obj_is_type(args[0], OBJ_STRING)) return eka_bool(false);
    int64_t max = arg_int(argc, args, 1, 0);
    return eka_bool((int64_t)eka_as_string(args[0])->length <= max);
}

static eka_value_t validate_range(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(3);
    double val = eka_is_int(args[0]) ? (double)eka_as_int(args[0]) :
                 eka_is_number(args[0]) ? eka_as_number(args[0]) : 0.0;
    double lo = arg_int(argc, args, 1, 0) ? (double)arg_int(argc, args, 1, 0) :
                eka_is_number(args[1]) ? eka_as_number(args[1]) : 0.0;
    double hi = arg_int(argc, args, 2, 0) ? (double)arg_int(argc, args, 2, 0) :
                eka_is_number(args[2]) ? eka_as_number(args[2]) : 0.0;
    return eka_bool(val >= lo && val <= hi);
}

static eka_value_t validate_match(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(2);
    /* Delegate to regex.test */
    return regex_test(vm, ctx, argc, args);
}

/* ================================================================
 * 20. slug
 * ================================================================ */

static eka_value_t slug_make(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    const char *input = arg_string(argc, args, 0, "");
    size_t len = strlen(input);

    char *buf = malloc(len * 2 + 1); /* worst case: every char becomes - */
    if (!buf) return eka_nil();

    size_t out = 0;
    bool last_was_dash = true;  /* skip leading dashes */
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)input[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            buf[out++] = (char)c;
            last_was_dash = false;
        } else if (c >= 'A' && c <= 'Z') {
            buf[out++] = (char)(c + 32);
            last_was_dash = false;
        } else if (c == ' ' || c == '-' || c == '_' || c == '.' || c == ',') {
            if (!last_was_dash) {
                buf[out++] = '-';
                last_was_dash = true;
            }
        }
        /* All other chars (unicode, symbols) are skipped */
    }
    /* Trim trailing dash */
    while (out > 0 && buf[out - 1] == '-') out--;
    if (out == 0) {
        buf[out++] = '-'; /* at least a single dash */
    }
    buf[out] = '\0';
    return eka_string_val(eka_string_take(buf, out));
}

/* ================================================================
 * 21. i18n
 * ================================================================ */

static eka_value_t i18n_set(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    const char *lang = arg_string(argc, args, 0, "en");
    eka_vm_set_global(vm, "__i18n_lang",
                      eka_string_val(eka_string_new(lang, strlen(lang))));
    return eka_nil();
}

static eka_value_t i18n_t(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    const char *key = arg_string(argc, args, 0, NULL);
    if (!key) return eka_nil();

    /* In V1, i18n.t returns the key itself. Full translation file loading
     * requires FS access and JSON parsing at startup — deferred to V2. */
    if (argc >= 2 && eka_obj_is_type(args[1], OBJ_MAP)) {
        /* Simple interpolation: replace {{ name }} with value from map */
        eka_map_t *vars = eka_as_map(args[1]);
        char result[1024];
        size_t out = 0;
        const char *p = key;
        while (*p && out < sizeof(result) - 1) {
            if (p[0] == '{' && p[1] == '{') {
                const char *end = strstr(p + 2, "}}");
                if (end) {
                    /* Extract variable name */
                    const char *var = p + 2;
                    while (var < end && *var == ' ') var++;
                    size_t vlen = (size_t)(end - var);
                    while (vlen > 0 && var[vlen-1] == ' ') vlen--;

                    eka_string_t *vk = eka_string_new(var, vlen);
                    eka_value_t vv = eka_map_get(vars, vk);
                    eka_string_t *vs = eka_value_to_string(vv);
                    size_t to_copy = vs->length;
                    if (out + to_copy >= sizeof(result)) to_copy = sizeof(result) - out - 1;
                    memcpy(result + out, vs->data, to_copy);
                    out += to_copy;
                    p = end + 2;
                    continue;
                }
            }
            result[out++] = *p++;
        }
        result[out] = '\0';
        return eka_string_val(eka_string_new(result, out));
    }
    return eka_string_val(eka_string_new(key, strlen(key)));
}

/* ================================================================
 * 22. sse — Server-Sent Events
 * ================================================================ */

#include <uv.h>

static eka_value_t sse_connect(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)ctx; (void)argc; (void)args;
    if (!vm->current_client) return eka_nil();

    /* Add to sse_clients if not already registered */
    int idx = -1;
    for (int i = 0; i < vm->sse_client_count; i++) {
        if (vm->sse_clients[i] == vm->current_client) { idx = i; break; }
    }
    if (idx < 0 && vm->sse_client_count < EKA_MAX_SSE_CONNS) {
        idx = vm->sse_client_count++;
        vm->sse_clients[idx] = vm->current_client;
    }
    vm->sse_current_idx = idx;

    /* Set response content type to text/event-stream */
    vm->response_state.body_set = true;
    vm->response_state.body = strdup("");
    vm->response_state.body_len = 0;
    strncpy(vm->response_state.content_type, "text/event-stream", 63);
    vm->response_state.content_type_set = true;
    vm->response_state.status = 200;

    return eka_nil();
}

static eka_value_t sse_send(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)ctx;
    CHECK_ARGC(2);
    const char *event = arg_string(argc, args, 0, "message");
    const char *data = arg_string(argc, args, 1, "");
    if (!vm->current_client) return eka_nil();

    /* Build SSE frame: "event: X\ndata: Y\n\n" */
    char frame[4096];
    int frame_len = snprintf(frame, sizeof(frame),
        "event: %s\r\ndata: %s\r\n\r\n", event, data);

    /* Write to current client */
    uv_buf_t buf = uv_buf_init(frame, (unsigned int)frame_len);
    uv_write_t *wreq = malloc(sizeof(uv_write_t));
    if (!wreq) return eka_nil();
    wreq->data = NULL;
    uv_write(wreq, (uv_stream_t *)vm->current_client, &buf, 1, NULL);
    /* Note: write callback not set — we rely on the connection staying alive.
     * V1 limitation: no write completion tracking for SSE. */

    return eka_nil();
}

static eka_value_t sse_broadcast(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)ctx;
    CHECK_ARGC(2);
    const char *event = arg_string(argc, args, 0, "message");
    const char *data = arg_string(argc, args, 1, "");

    char frame[4096];
    int frame_len = snprintf(frame, sizeof(frame),
        "event: %s\r\ndata: %s\r\n\r\n", event, data);
    uv_buf_t buf = uv_buf_init(frame, (unsigned int)frame_len);

    for (int i = 0; i < vm->sse_client_count; i++) {
        uv_tcp_t *client = (uv_tcp_t *)vm->sse_clients[i];
        if (client) {
            /* Skip the current client if it called broadcast (they already see it
             * if they're connected; actually, broadcast should NOT skip current) */
            uv_write_t *wreq = malloc(sizeof(uv_write_t));
            if (wreq) {
                wreq->data = NULL;
                uv_write(wreq, (uv_stream_t *)client, &buf, 1, NULL);
            }
        }
    }
    return eka_nil();
}

static eka_value_t sse_count(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)ctx; (void)argc; (void)args;
    return eka_int(vm->sse_client_count);
}

/* ================================================================
 * 23. rss
 * ================================================================ */

static eka_value_t rss_generate(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    if (!eka_obj_is_type(args[0], OBJ_MAP)) return eka_nil();
    eka_map_t *opts = eka_as_map(args[0]);

    /* Get title, link, description, items */
    #define GET_STR(dest, field) do { \
        eka_string_t *k = eka_string_intern(field, strlen(field)); \
        eka_value_t v = eka_map_get(opts, k); \
        if (eka_obj_is_type(v, OBJ_STRING)) dest = eka_as_string(v)->data; \
    } while(0)

    const char *title = "", *link = "", *desc = "";
    GET_STR(title, "title");
    GET_STR(link, "link");
    GET_STR(desc, "description");
    #undef GET_STR

    char buf[16384];
    char *et = xml_escape(title);
    char *el = xml_escape(link);
    char *ed = xml_escape(desc);
    int out = snprintf(buf, sizeof(buf),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<rss version=\"2.0\">\n"
        "<channel>\n"
        "<title>%s</title>\n"
        "<link>%s</link>\n"
        "<description>%s</description>\n",
        et ? et : "", el ? el : "", ed ? ed : "");
    free(et); free(el); free(ed);

    /* Items */
    eka_string_t *items_key = eka_string_intern("items", 5);
    eka_value_t items_val = eka_map_get(opts, items_key);
    if (eka_obj_is_type(items_val, OBJ_LIST)) {
        eka_list_t *items = eka_as_list(items_val);
        for (uint32_t i = 0; i < items->length; i++) {
            if (eka_obj_is_type(items->items[i], OBJ_MAP)) {
                eka_map_t *item = eka_as_map(items->items[i]);
                const char *ititle = "", *ilink = "", *idesc = "", *idate = "";
                #define GET_ITEM(s, f) do { \
                    eka_string_t *k = eka_string_intern(f, strlen(f)); \
                    eka_value_t v = eka_map_get(item, k); \
                    if (eka_obj_is_type(v, OBJ_STRING)) s = eka_as_string(v)->data; \
                } while(0)
                GET_ITEM(ititle, "title");
                GET_ITEM(ilink, "link");
                GET_ITEM(idesc, "description");
                GET_ITEM(idate, "pubDate");
                #undef GET_ITEM

                char *iet = xml_escape(ititle);
                char *iel = xml_escape(ilink);
                char *ied = xml_escape(idesc);
                out += snprintf(buf + out, sizeof(buf) - out,
                    "<item>\n"
                    "<title>%s</title>\n"
                    "<link>%s</link>\n"
                    "<description>%s</description>\n"
                    "%s%s%s"
                    "</item>\n",
                    iet ? iet : "", iel ? iel : "", ied ? ied : "",
                    idate[0] ? "<pubDate>" : "", idate, idate[0] ? "</pubDate>\n" : "");
                free(iet); free(iel); free(ied);
            }
        }
    }
    out += snprintf(buf + out, sizeof(buf) - out, "</channel>\n</rss>\n");
    return eka_string_val(eka_string_new(buf, (size_t)out));
}

/* ================================================================
 * 24. sitemap
 * ================================================================ */

static eka_value_t sitemap_generate(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    if (!eka_obj_is_type(args[0], OBJ_LIST)) return eka_nil();
    eka_list_t *urls = eka_as_list(args[0]);

    char buf[16384];
    int out = snprintf(buf, sizeof(buf),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">\n");

    for (uint32_t i = 0; i < urls->length && out < (int)sizeof(buf) - 512; i++) {
        if (!eka_obj_is_type(urls->items[i], OBJ_MAP)) continue;
        eka_map_t *u = eka_as_map(urls->items[i]);

        const char *url = "", *lastmod = "", *changefreq = "", *priority = "";
        #define GU(f, s) do { \
            eka_string_t *k = eka_string_intern(f, strlen(f)); \
            eka_value_t v = eka_map_get(u, k); \
            if (eka_obj_is_type(v, OBJ_STRING)) s = eka_as_string(v)->data; \
        } while(0)
        GU("url", url);
        GU("lastmod", lastmod);
        GU("changefreq", changefreq);
        GU("priority", priority);
        #undef GU

        char *eurl = xml_escape(url);
        out += snprintf(buf + out, sizeof(buf) - out,
            "<url>\n"
            "<loc>%s</loc>\n"
            "%s%s%s"
            "%s%s%s"
            "%s%s%s"
            "</url>\n",
            eurl ? eurl : "",
            lastmod[0] ? "<lastmod>" : "", lastmod, lastmod[0] ? "</lastmod>\n" : "",
            changefreq[0] ? "<changefreq>" : "", changefreq, changefreq[0] ? "</changefreq>\n" : "",
            priority[0] ? "<priority>" : "", priority, priority[0] ? "</priority>\n" : "");
        free(eurl);
    }
    out += snprintf(buf + out, sizeof(buf) - out, "</urlset>\n");
    return eka_string_val(eka_string_new(buf, (size_t)out));
}

/* ================================================================
 * 13. regex — PCRE2
 * ================================================================ */

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

static eka_value_t regex_match(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(2);
    const char *pattern = arg_string(argc, args, 0, NULL);
    const char *subject = arg_string(argc, args, 1, "");
    if (!pattern) return eka_nil();

    int errcode;
    PCRE2_SIZE erroffset;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
                                    0, &errcode, &erroffset, NULL);
    if (!re) return eka_nil();

    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
    int rc = pcre2_match(re, (PCRE2_SPTR)subject, strlen(subject),
                         0, 0, md, NULL);

    eka_value_t result = eka_nil();
    if (rc > 0) {
        PCRE2_SIZE *ovec = pcre2_get_ovector_pointer(md);
        /* Return first capture group if present, else full match */
        size_t start = ovec[0];
        size_t end = ovec[1];
        result = eka_string_val(eka_string_new(subject + start, end - start));
    }

    pcre2_match_data_free(md);
    pcre2_code_free(re);
    return result;
}

static eka_value_t regex_replace(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(3);
    const char *pattern = arg_string(argc, args, 0, NULL);
    const char *replacement = arg_string(argc, args, 1, "");
    const char *subject = arg_string(argc, args, 2, "");
    if (!pattern) return args[2];

    int errcode;
    PCRE2_SIZE erroffset;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
                                    0, &errcode, &erroffset, NULL);
    if (!re) return args[2];

    size_t subj_len = strlen(subject);
    size_t repl_len = strlen(replacement);
    size_t max_out = subj_len * 2 + repl_len + 1;
    char *buf = malloc(max_out);
    if (!buf) { pcre2_code_free(re); return args[2]; }

    size_t out = 0;
    size_t offset = 0;
    pcre2_match_data *md = pcre2_match_data_create(32, NULL);

    while (offset < subj_len && out < max_out - repl_len - 1) {
        int rc = pcre2_match(re, (PCRE2_SPTR)(subject + offset),
                             subj_len - offset, 0, 0, md, NULL);
        if (rc <= 0) break;

        PCRE2_SIZE *ovec = pcre2_get_ovector_pointer(md);
        size_t match_start = ovec[0];
        size_t match_end = ovec[1];

        /* Copy everything before the match */
        memcpy(buf + out, subject + offset, match_start);
        out += match_start;

        /* Copy replacement */
        memcpy(buf + out, replacement, repl_len);
        out += repl_len;

        offset += match_end;
    }
    /* Copy remaining */
    if (offset < subj_len) {
        size_t remaining = subj_len - offset;
        if (out + remaining < max_out) {
            memcpy(buf + out, subject + offset, remaining);
            out += remaining;
        }
    }
    buf[out] = '\0';

    pcre2_match_data_free(md);
    pcre2_code_free(re);
    return eka_string_val(eka_string_take(buf, out));
}

static eka_value_t regex_test(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(2);
    const char *pattern = arg_string(argc, args, 0, NULL);
    const char *subject = arg_string(argc, args, 1, "");
    if (!pattern) return eka_bool(false);

    int errcode;
    PCRE2_SIZE erroffset;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
                                    0, &errcode, &erroffset, NULL);
    if (!re) return eka_bool(false);

    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
    int rc = pcre2_match(re, (PCRE2_SPTR)subject, strlen(subject),
                         0, 0, md, NULL);

    pcre2_match_data_free(md);
    pcre2_code_free(re);
    return eka_bool(rc > 0);
}

/* ================================================================
 * 26. math
 * ================================================================ */

static eka_value_t math_floor(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx; CHECK_ARGC(1);
    double d = eka_is_number(args[0]) ? eka_as_number(args[0]) :
               eka_is_int(args[0]) ? (double)eka_as_int(args[0]) : 0.0;
    return eka_number(floor(d));
}

static eka_value_t math_ceil(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx; CHECK_ARGC(1);
    double d = eka_is_number(args[0]) ? eka_as_number(args[0]) :
               eka_is_int(args[0]) ? (double)eka_as_int(args[0]) : 0.0;
    return eka_number(ceil(d));
}

static eka_value_t math_round(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx; CHECK_ARGC(1);
    double d = eka_is_number(args[0]) ? eka_as_number(args[0]) :
               eka_is_int(args[0]) ? (double)eka_as_int(args[0]) : 0.0;
    return eka_number(round(d));
}

static eka_value_t math_abs(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx; CHECK_ARGC(1);
    if (eka_is_int(args[0])) {
        int64_t v = eka_as_int(args[0]);
        return eka_int(v < 0 ? -v : v);
    }
    double d = eka_is_number(args[0]) ? eka_as_number(args[0]) : 0.0;
    return eka_number(d < 0 ? -d : d);
}

static eka_value_t math_min(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx; CHECK_ARGC(2);
    double a = eka_is_number(args[0]) ? eka_as_number(args[0]) :
               eka_is_int(args[0]) ? (double)eka_as_int(args[0]) : 0.0;
    double b = eka_is_number(args[1]) ? eka_as_number(args[1]) :
               eka_is_int(args[1]) ? (double)eka_as_int(args[1]) : 0.0;
    return eka_number(a < b ? a : b);
}

static eka_value_t math_max(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx; CHECK_ARGC(2);
    double a = eka_is_number(args[0]) ? eka_as_number(args[0]) :
               eka_is_int(args[0]) ? (double)eka_as_int(args[0]) : 0.0;
    double b = eka_is_number(args[1]) ? eka_as_number(args[1]) :
               eka_is_int(args[1]) ? (double)eka_as_int(args[1]) : 0.0;
    return eka_number(a > b ? a : b);
}

static eka_value_t math_pow(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx; CHECK_ARGC(2);
    double a = eka_is_number(args[0]) ? eka_as_number(args[0]) :
               eka_is_int(args[0]) ? (double)eka_as_int(args[0]) : 0.0;
    double b = eka_is_number(args[1]) ? eka_as_number(args[1]) :
               eka_is_int(args[1]) ? (double)eka_as_int(args[1]) : 0.0;
    return eka_number(pow(a, b));
}

static eka_value_t math_sqrt(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx; CHECK_ARGC(1);
    double d = eka_is_number(args[0]) ? eka_as_number(args[0]) :
               eka_is_int(args[0]) ? (double)eka_as_int(args[0]) : 0.0;
    return eka_number(sqrt(d));
}

static eka_value_t math_log(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx; CHECK_ARGC(1);
    double d = eka_is_number(args[0]) ? eka_as_number(args[0]) :
               eka_is_int(args[0]) ? (double)eka_as_int(args[0]) : 0.0;
    return eka_number(log(d));
}

static eka_value_t math_random(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx; (void)argc; (void)args;
    return eka_number((double)rand() / (double)RAND_MAX);
}

static eka_value_t math_random_int(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx; CHECK_ARGC(2);
    int64_t lo = arg_int(argc, args, 0, 0);
    int64_t hi = arg_int(argc, args, 1, 0);
    if (lo > hi) { int64_t t = lo; lo = hi; hi = t; }
    int64_t range = hi - lo + 1;
    return eka_int(lo + (int64_t)(rand() % (unsigned long)range));
}

/* ================================================================
 * number.parse — parse string to number
 * ================================================================ */

static eka_value_t number_parse(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    if (!eka_obj_is_type(args[0], OBJ_STRING)) return eka_nil();
    eka_string_t *s = eka_as_string(args[0]);
    if (s->length == 0) return eka_nil();

    char *endptr = NULL;
    double d = strtod(s->data, &endptr);
    if (endptr == s->data || *endptr != '\0') return eka_nil();

    /* Return as int if it's a whole number that fits */
    if (d == floor(d) && d >= -35184372088832.0 && d <= 35184372088831.0) {
        return eka_int((int64_t)d);
    }
    return eka_number(d);
}

/* ================================================================
 * datetime.parse — parse date string with format
 * ================================================================ */

static eka_value_t datetime_parse(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(2);
    const char *str = arg_string(argc, args, 0, NULL);
    const char *fmt = arg_string(argc, args, 1, NULL);
    if (!str || !fmt) return eka_nil();

    struct tm t;
    memset(&t, 0, sizeof(t));

    /* Use strptime for parsing */
    char *result = strptime(str, fmt, &t);
    if (!result) return eka_nil();

    /* Return a map with the parsed components */
    eka_map_t *dt_map = eka_map_new(8);
    map_set_cstr(dt_map, "year",   eka_int(t.tm_year + 1900));
    map_set_cstr(dt_map, "month",  eka_int(t.tm_mon + 1));
    map_set_cstr(dt_map, "day",    eka_int(t.tm_mday));
    map_set_cstr(dt_map, "hour",   eka_int(t.tm_hour));
    map_set_cstr(dt_map, "minute", eka_int(t.tm_min));
    map_set_cstr(dt_map, "second", eka_int(t.tm_sec));

    /* Store the raw timestamp and provide a format method */
    time_t ts = mktime(&t);
    map_set_cstr(dt_map, "timestamp", eka_int((int64_t)ts));

    return eka_map_val(dt_map);
}

/* ================================================================
 * crypto.hmac — HMAC computation
 * ================================================================ */

static eka_value_t crypto_hmac(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(3);
    const char *algo = arg_string(argc, args, 0, "sha256");
    const char *key  = arg_string(argc, args, 1, "");
    const char *msg  = arg_string(argc, args, 2, "");

    const EVP_MD *md = NULL;
    if (strcmp(algo, "sha256") == 0) md = EVP_sha256();
    else if (strcmp(algo, "sha512") == 0) md = EVP_sha512();
    else return eka_nil();

    unsigned char hmac_buf[EVP_MAX_MD_SIZE];
    unsigned int hmac_len = 0;
    HMAC(md, key, (int)strlen(key),
         (unsigned char *)msg, strlen(msg),
         hmac_buf, &hmac_len);

    /* Convert to hex string */
    char *hex = malloc(hmac_len * 2 + 1);
    if (!hex) return eka_nil();
    for (unsigned int i = 0; i < hmac_len; i++) {
        snprintf(hex + i * 2, 3, "%02x", hmac_buf[i]);
    }
    hex[hmac_len * 2] = '\0';

    eka_string_t *result = eka_string_take(hex, hmac_len * 2);
    return eka_string_val(result);
}

/* ================================================================
 * crypto.sha512 — SHA-512 hash
 * ================================================================ */

static eka_value_t crypto_sha512(eka_vm_t *vm, void *ctx, int argc, eka_value_t *args) {
    (void)vm; (void)ctx;
    CHECK_ARGC(1);
    const char *input = arg_string(argc, args, 0, "");

    unsigned char hash[SHA512_DIGEST_LENGTH];
    SHA512((unsigned char *)input, strlen(input), hash);

    char *hex = malloc(SHA512_DIGEST_LENGTH * 2 + 1);
    if (!hex) return eka_nil();
    for (int i = 0; i < SHA512_DIGEST_LENGTH; i++) {
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    }
    hex[SHA512_DIGEST_LENGTH * 2] = '\0';

    eka_string_t *result = eka_string_take(hex, SHA512_DIGEST_LENGTH * 2);
    return eka_string_val(result);
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

    /* number.parse */
    {
        eka_map_t *num_map = eka_map_new(4);
        map_set_cstr(num_map, "parse", make_native(number_parse, NULL, "parse"));
        eka_vm_set_global(vm, "number", eka_map_val(num_map));
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
        eka_map_t *crypto_map = eka_map_new(8);
        map_set_cstr(crypto_map, "sha256",      make_native(crypto_sha256, NULL, "sha256"));
        map_set_cstr(crypto_map, "sha512",      make_native(crypto_sha512, NULL, "sha512"));
        map_set_cstr(crypto_map, "randomBytes", make_native(crypto_random_bytes, NULL, "randomBytes"));
        map_set_cstr(crypto_map, "hmac",        make_native(crypto_hmac, NULL, "hmac"));
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
        eka_map_t *dt_map = eka_map_new(8);
        map_set_cstr(dt_map, "now",   make_native(datetime_now, NULL, "now"));
        map_set_cstr(dt_map, "parse", make_native(datetime_parse, NULL, "parse"));
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

    /* 9. http */
    {
        eka_map_t *http_map = eka_map_new(8);
        map_set_cstr(http_map, "get",    make_native(http_get_fn, NULL, "get"));
        map_set_cstr(http_map, "post",   make_native(http_post_fn, NULL, "post"));
        map_set_cstr(http_map, "put",    make_native(http_put_fn, NULL, "put"));
        map_set_cstr(http_map, "delete", make_native(http_delete_fn, NULL, "delete"));
        map_set_cstr(http_map, "patch",  make_native(http_patch_fn, NULL, "patch"));
        eka_vm_set_global(vm, "http", eka_map_val(http_map));
    }

    /* 10. fs */
    {
        eka_map_t *fs_map = eka_map_new(16);
        map_set_cstr(fs_map, "read",   make_native(fs_read, NULL, "read"));
        map_set_cstr(fs_map, "write",  make_native(fs_write, NULL, "write"));
        map_set_cstr(fs_map, "append", make_native(fs_append, NULL, "append"));
        map_set_cstr(fs_map, "move",   make_native(fs_move, NULL, "move"));
        map_set_cstr(fs_map, "copy",   make_native(fs_copy, NULL, "copy"));
        map_set_cstr(fs_map, "exists", make_native(fs_exists, NULL, "exists"));
        map_set_cstr(fs_map, "delete", make_native(fs_delete, NULL, "delete"));
        map_set_cstr(fs_map, "mkdir",  make_native(fs_mkdir, NULL, "mkdir"));
        map_set_cstr(fs_map, "list",   make_native(fs_list, NULL, "list"));
        eka_vm_set_global(vm, "fs", eka_map_val(fs_map));
    }

    /* 14. base64 */
    {
        eka_map_t *b64_map = eka_map_new(4);
        map_set_cstr(b64_map, "encode", make_native(base64_encode, NULL, "encode"));
        map_set_cstr(b64_map, "decode", make_native(base64_decode, NULL, "decode"));
        eka_vm_set_global(vm, "base64", eka_map_val(b64_map));
    }

    /* 15. url */
    {
        eka_map_t *url_map = eka_map_new(4);
        map_set_cstr(url_map, "parse", make_native(url_parse, NULL, "parse"));
        map_set_cstr(url_map, "build", make_native(url_build, NULL, "build"));
        eka_vm_set_global(vm, "url", eka_map_val(url_map));
    }

    /* 16. session */
    {
        eka_map_t *sess_map = eka_map_new(8);
        map_set_cstr(sess_map, "set",    make_native(session_set, NULL, "set"));
        map_set_cstr(sess_map, "get",    make_native(session_get, NULL, "get"));
        map_set_cstr(sess_map, "delete", make_native(session_delete, NULL, "delete"));
        map_set_cstr(sess_map, "clear",  make_native(session_clear, NULL, "clear"));
        map_set_cstr(sess_map, "csrf",   make_native(session_csrf, NULL, "csrf"));
        eka_vm_set_global(vm, "session", eka_map_val(sess_map));
    }

    /* 18. email */
    {
        eka_map_t *email_map = eka_map_new(4);
        map_set_cstr(email_map, "send", make_native(email_send, NULL, "send"));
        eka_vm_set_global(vm, "email", eka_map_val(email_map));
    }

    /* 19. validate */
    {
        eka_map_t *val_map = eka_map_new(16);
        map_set_cstr(val_map, "email",     make_native(validate_email, NULL, "email"));
        map_set_cstr(val_map, "url",       make_native(validate_url_fn, NULL, "url"));
        map_set_cstr(val_map, "required",  make_native(validate_required, NULL, "required"));
        map_set_cstr(val_map, "minLength", make_native(validate_min_length, NULL, "minLength"));
        map_set_cstr(val_map, "maxLength", make_native(validate_max_length, NULL, "maxLength"));
        map_set_cstr(val_map, "range",     make_native(validate_range, NULL, "range"));
        map_set_cstr(val_map, "match",     make_native(validate_match, NULL, "match"));
        eka_vm_set_global(vm, "validate", eka_map_val(val_map));
    }

    /* 20. slug */
    {
        eka_map_t *slug_map = eka_map_new(4);
        map_set_cstr(slug_map, "make", make_native(slug_make, NULL, "make"));
        eka_vm_set_global(vm, "slug", eka_map_val(slug_map));
    }

    /* 21. i18n */
    {
        eka_map_t *i18n_map = eka_map_new(4);
        map_set_cstr(i18n_map, "set", make_native(i18n_set, NULL, "set"));
        map_set_cstr(i18n_map, "t",   make_native(i18n_t, NULL, "t"));
        eka_vm_set_global(vm, "i18n", eka_map_val(i18n_map));
    }

    /* 22. sse */
    {
        eka_map_t *sse_map = eka_map_new(8);
        map_set_cstr(sse_map, "connect",   make_native(sse_connect, NULL, "connect"));
        map_set_cstr(sse_map, "send",      make_native(sse_send, NULL, "send"));
        map_set_cstr(sse_map, "broadcast", make_native(sse_broadcast, NULL, "broadcast"));
        map_set_cstr(sse_map, "count",     make_native(sse_count, NULL, "count"));
        eka_vm_set_global(vm, "sse", eka_map_val(sse_map));
    }

    /* 23. rss */
    {
        eka_map_t *rss_map = eka_map_new(4);
        map_set_cstr(rss_map, "generate", make_native(rss_generate, NULL, "generate"));
        eka_vm_set_global(vm, "rss", eka_map_val(rss_map));
    }

    /* 24. sitemap */
    {
        eka_map_t *sitemap_map = eka_map_new(4);
        map_set_cstr(sitemap_map, "generate", make_native(sitemap_generate, NULL, "generate"));
        eka_vm_set_global(vm, "sitemap", eka_map_val(sitemap_map));
    }

    /* 13. regex */
    {
        eka_map_t *regex_map = eka_map_new(4);
        map_set_cstr(regex_map, "match",   make_native(regex_match, NULL, "match"));
        map_set_cstr(regex_map, "replace", make_native(regex_replace, NULL, "replace"));
        map_set_cstr(regex_map, "test",    make_native(regex_test, NULL, "test"));
        eka_vm_set_global(vm, "regex", eka_map_val(regex_map));
    }

    /* 26. math */
    {
        eka_map_t *math_map = eka_map_new(16);
        map_set_cstr(math_map, "floor",     make_native(math_floor, NULL, "floor"));
        map_set_cstr(math_map, "ceil",      make_native(math_ceil, NULL, "ceil"));
        map_set_cstr(math_map, "round",     make_native(math_round, NULL, "round"));
        map_set_cstr(math_map, "abs",       make_native(math_abs, NULL, "abs"));
        map_set_cstr(math_map, "min",       make_native(math_min, NULL, "min"));
        map_set_cstr(math_map, "max",       make_native(math_max, NULL, "max"));
        map_set_cstr(math_map, "pow",       make_native(math_pow, NULL, "pow"));
        map_set_cstr(math_map, "sqrt",      make_native(math_sqrt, NULL, "sqrt"));
        map_set_cstr(math_map, "log",       make_native(math_log, NULL, "log"));
        map_set_cstr(math_map, "random",    make_native(math_random, NULL, "random"));
        map_set_cstr(math_map, "randomInt", make_native(math_random_int, NULL, "randomInt"));
        eka_vm_set_global(vm, "math", eka_map_val(math_map));
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
    map_set_cstr(resp_map, "cookie",   make_native(response_cookie, NULL, "cookie"));

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
