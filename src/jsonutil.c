#define _POSIX_C_SOURCE 200809L
#include "jsonutil.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* allocation / constructors                                               */
/* ---------------------------------------------------------------------- */

static JsonNode *node_new(JsonType type) {
    JsonNode *n = calloc(1, sizeof(*n));
    if (n == NULL) {
        return NULL;
    }
    n->type = type;
    return n;
}

JsonNode *json_null(void) { return node_new(JSON_NULL); }
JsonNode *json_bool(bool value) {
    JsonNode *n = node_new(JSON_BOOL);
    if (n) n->u.boolean = value;
    return n;
}
JsonNode *json_number(double value) {
    JsonNode *n = node_new(JSON_NUMBER);
    if (n) n->u.number = value;
    return n;
}

static char *dup_str(const char *s) {
    size_t len = strlen(s);
    char *p = malloc(len + 1);
    if (!p) return NULL;
    memcpy(p, s, len + 1);
    return p;
}

JsonNode *json_string(const char *value) {
    JsonNode *n = node_new(JSON_STRING);
    if (!n || !value) return n;
    n->u.string = dup_str(value);
    if (!n->u.string) { free(n); return NULL; }
    return n;
}

JsonNode *json_array_new(void) { return node_new(JSON_ARRAY); }
JsonNode *json_object_new(void) { return node_new(JSON_OBJECT); }

bool json_array_append(JsonNode *arr, JsonNode *item) {
    if (!arr || arr->type != JSON_ARRAY || !item) return false;
    JsonNode **items = realloc(arr->u.array.items,
                               (arr->u.array.count + 1) * sizeof(*items));
    if (!items) return false;
    arr->u.array.items = items;
    arr->u.array.items[arr->u.array.count++] = item;
    return true;
}

static bool object_find(const JsonNode *obj, const char *key, size_t *idx) {
    for (size_t i = 0; i < obj->u.object.count; ++i) {
        if (strcmp(obj->u.object.keys[i], key) == 0) {
            if (idx) *idx = i;
            return true;
        }
    }
    return false;
}

bool json_object_set(JsonNode *obj, const char *key, JsonNode *value) {
    if (!obj || obj->type != JSON_OBJECT || !key || !value) return false;
    size_t idx;
    if (object_find(obj, key, &idx)) {
        json_free(obj->u.object.values[idx]);
        obj->u.object.values[idx] = value;
        return true;
    }
    char **keys = realloc(obj->u.object.keys,
                          (obj->u.object.count + 1) * sizeof(*keys));
    JsonNode **vals = realloc(obj->u.object.values,
                              (obj->u.object.count + 1) * sizeof(*vals));
    if (!keys || !vals) {
        free(keys); free(vals); return false;
    }
    obj->u.object.keys = keys;
    obj->u.object.values = vals;
    char *kcopy = dup_str(key);
    if (!kcopy) return false;
    obj->u.object.keys[obj->u.object.count] = kcopy;
    obj->u.object.values[obj->u.object.count] = value;
    obj->u.object.count++;
    return true;
}

JsonNode *json_object_get(const JsonNode *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT || !key) return NULL;
    size_t idx;
    return object_find(obj, key, &idx) ? obj->u.object.values[idx] : NULL;
}

bool json_object_has(const JsonNode *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return false;
    return object_find(obj, key, NULL);
}

bool json_object_delete(JsonNode *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return false;
    size_t idx;
    if (!object_find(obj, key, &idx)) return false;
    free(obj->u.object.keys[idx]);
    json_free(obj->u.object.values[idx]);
    memmove(&obj->u.object.keys[idx], &obj->u.object.keys[idx + 1],
            (obj->u.object.count - idx - 1) * sizeof(char *));
    memmove(&obj->u.object.values[idx], &obj->u.object.values[idx + 1],
            (obj->u.object.count - idx - 1) * sizeof(JsonNode *));
    obj->u.object.count--;
    return true;
}

/* ---------------------------------------------------------------------- */
/* dotted paths                                                            */
/* ---------------------------------------------------------------------- */

JsonNode *json_path_get(const JsonNode *root, const char *dotted) {
    if (!root || !dotted) return NULL;
    char *path = dup_str(dotted);
    if (!path) return NULL;
    const JsonNode *cur = root;
    char *save = NULL;
    for (char *tok = strtok_r(path, ".", &save);
         tok != NULL;
         tok = strtok_r(NULL, ".", &save)) {
        if (!cur || cur->type != JSON_OBJECT) { cur = NULL; break; }
        cur = json_object_get(cur, tok);
    }
    free(path);
    /* cast away const: callers treat returned nodes as borrowed */
    return (JsonNode *)(uintptr_t)cur;
}

bool json_path_set(JsonNode *root, const char *dotted, JsonNode *value) {
    if (!root || !dotted || !value || root->type != JSON_OBJECT) return false;

    /* Tokenise first, then re-fetch the parent from the root after every
     * insert (json_object_set may realloc its member arrays). */
    char *parts[64];
    int nparts = 0;
    const char *q = dotted;
    while (*q && nparts < 64) {
        const char *dot = strchr(q, '.');
        size_t len = dot ? (size_t)(dot - q) : strlen(q);
        char *seg = malloc(len + 1);
        if (!seg) return false;
        memcpy(seg, q, len); seg[len] = '\0';
        parts[nparts++] = seg;
        if (!dot) break;
        q = dot + 1;
    }
    bool ok = true;
    for (int i = 0; i < nparts; ++i) {
        JsonNode *parent = root;
        for (int j = 0; j < i; ++j) {
            parent = json_object_get(parent, parts[j]);
            if (!parent || parent->type != JSON_OBJECT) { ok = false; goto done; }
        }
        if (i == nparts - 1) {
            if (!json_object_set(parent, parts[i], value)) ok = false;
            goto done;
        }
        JsonNode *child = json_object_get(parent, parts[i]);
        if (!child || child->type != JSON_OBJECT) {
            child = json_object_new();
            if (!child || !json_object_set(parent, parts[i], child)) {
                if (child) json_free(child);
                ok = false; goto done;
            }
        }
    }
done:
    for (int i = 0; i < nparts; ++i) free(parts[i]);
    return ok;
}

/* ---------------------------------------------------------------------- */
/* parser                                                                  */
/* ---------------------------------------------------------------------- */

static void perror_set(JsonParser *p, const char *msg) {
    if (p && p->error[0] == '\0') {
        snprintf(p->error, sizeof(p->error), "%s", msg);
    }
}

static void skip_ws(JsonParser *p) {
    while (p->cur < p->end) {
        char c = *p->cur;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->cur++;
        else break;
    }
}

static JsonNode *parse_value(JsonParser *p);

static char *parse_string_raw(JsonParser *p) {
    if (p->cur >= p->end || *p->cur != '"') {
        perror_set(p, "expected string");
        return NULL;
    }
    p->cur++;
    size_t cap = 16, len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    while (p->cur < p->end && *p->cur != '"') {
        char c = *p->cur++;
        if (c == '\\') {
            if (p->cur >= p->end) goto fail;
            char e = *p->cur++;
            switch (e) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    /* decode \uXXXX (BMP); surrogates pair as \uXXXX\uXXXX */
                    unsigned int code = 0;
                    for (int i = 0; i < 4; ++i) {
                        if (p->cur >= p->end) goto fail;
                        char h = *p->cur++;
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= (unsigned)(h - 'A' + 10);
                        else goto fail;
                    }
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        if (p->cur + 6 <= p->end && p->cur[0] == '\\'
                            && p->cur[1] == 'u') {
                            p->cur += 2;
                            unsigned int lo = 0;
                            for (int i = 0; i < 4; ++i) {
                                char h = *p->cur++;
                                lo <<= 4;
                                if (h >= '0' && h <= '9') lo |= (unsigned)(h - '0');
                                else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
                                else goto fail;
                            }
                            code = 0x10000 + ((code - 0xD800) << 10)
                                   + (lo - 0xDC00);
                        } else {
                            goto fail;
                        }
                    }
                    char buf[4];
                    size_t n = 0;
                    if (code < 0x80) buf[n++] = (char)code;
                    else if (code < 0x800) {
                        buf[n++] = (char)(0xC0 | (code >> 6));
                        buf[n++] = (char)(0x80 | (code & 0x3F));
                    } else if (code < 0x10000) {
                        buf[n++] = (char)(0xE0 | (code >> 12));
                        buf[n++] = (char)(0x80 | ((code >> 6) & 0x3F));
                        buf[n++] = (char)(0x80 | (code & 0x3F));
                    } else {
                        buf[n++] = (char)(0xF0 | (code >> 18));
                        buf[n++] = (char)(0x80 | ((code >> 12) & 0x3F));
                        buf[n++] = (char)(0x80 | ((code >> 6) & 0x3F));
                        buf[n++] = (char)(0x80 | (code & 0x3F));
                    }
                    if (len + n + 1 > cap) {
                        while (len + n + 1 > cap) cap *= 2;
                        char *grown = realloc(out, cap);
                        if (!grown) goto fail;
                        out = grown;
                    }
                    memcpy(out + len, buf, n);
                    len += n;
                    continue;
                }
                default: goto fail;
            }
        }
        if (len + 2 > cap) {
            cap *= 2;
            char *grown = realloc(out, cap);
            if (!grown) goto fail;
            out = grown;
        }
        out[len++] = c;
    }
    if (p->cur >= p->end) { perror_set(p, "unterminated string"); goto fail; }
    p->cur++; /* closing quote */
    out[len] = '\0';
    return out;
fail:
    free(out);
    return NULL;
}

static JsonNode *parse_string_node(JsonParser *p) {
    char *s = parse_string_raw(p);
    if (!s) return NULL;
    JsonNode *n = node_new(JSON_STRING);
    if (!n) { free(s); return NULL; }
    n->u.string = s;
    return n;
}

static JsonNode *parse_number(JsonParser *p) {
    const char *start = p->cur;
    if (*p->cur == '-') p->cur++;
    while (p->cur < p->end && (isdigit((unsigned char)*p->cur)
           || *p->cur == '.' || *p->cur == 'e' || *p->cur == 'E'
           || *p->cur == '+' || *p->cur == '-')) {
        p->cur++;
    }
    char buf[64];
    size_t n = (size_t)(p->cur - start);
    if (n == 0 || n >= sizeof(buf)) { perror_set(p, "bad number"); return NULL; }
    memcpy(buf, start, n); buf[n] = '\0';
    JsonNode *node = node_new(JSON_NUMBER);
    if (!node) return NULL;
    node->u.number = strtod(buf, NULL);
    return node;
}

static bool parse_literal(JsonParser *p, const char *lit) {
    size_t n = strlen(lit);
    if ((size_t)(p->end - p->cur) < n || strncmp(p->cur, lit, n) != 0) {
        perror_set(p, "bad literal");
        return false;
    }
    p->cur += n;
    return true;
}

static JsonNode *parse_array(JsonParser *p) {
    p->cur++; /* [ */
    JsonNode *arr = json_array_new();
    if (!arr) return NULL;
    skip_ws(p);
    if (p->cur < p->end && *p->cur == ']') { p->cur++; return arr; }
    for (;;) {
        skip_ws(p);
        JsonNode *item = parse_value(p);
        if (!item) goto fail;
        if (!json_array_append(arr, item)) { json_free(item); goto fail; }
        skip_ws(p);
        if (p->cur >= p->end) { perror_set(p, "unterminated array"); goto fail; }
        if (*p->cur == ',') { p->cur++; continue; }
        if (*p->cur == ']') { p->cur++; break; }
        perror_set(p, "expected , or ]"); goto fail;
    }
    return arr;
fail:
    json_free(arr);
    return NULL;
}

static JsonNode *parse_object(JsonParser *p) {
    p->cur++; /* { */
    JsonNode *obj = json_object_new();
    if (!obj) return NULL;
    skip_ws(p);
    if (p->cur < p->end && *p->cur == '}') { p->cur++; return obj; }
    for (;;) {
        skip_ws(p);
        char *key = parse_string_raw(p);
        if (!key) goto fail;
        skip_ws(p);
        if (p->cur >= p->end || *p->cur != ':') {
            free(key); perror_set(p, "expected :"); goto fail;
        }
        p->cur++;
        skip_ws(p);
        JsonNode *val = parse_value(p);
        if (!val) { free(key); goto fail; }
        if (!json_object_set(obj, key, val)) {
            free(key); json_free(val); goto fail;
        }
        free(key);
        skip_ws(p);
        if (p->cur >= p->end) { perror_set(p, "unterminated object"); goto fail; }
        if (*p->cur == ',') { p->cur++; continue; }
        if (*p->cur == '}') { p->cur++; break; }
        perror_set(p, "expected , or }"); goto fail;
    }
    return obj;
fail:
    json_free(obj);
    return NULL;
}

static JsonNode *parse_value(JsonParser *p) {
    skip_ws(p);
    if (p->cur >= p->end) { perror_set(p, "unexpected end"); return NULL; }
    char c = *p->cur;
    if (c == '{') return parse_object(p);
    if (c == '[') return parse_array(p);
    if (c == '"') return parse_string_node(p);
    if (c == '-' || isdigit((unsigned char)c)) return parse_number(p);
    if (c == 't' || c == 'f') {
        bool is_true = (c == 't');
        if (!parse_literal(p, is_true ? "true" : "false")) return NULL;
        return json_bool(is_true);
    }
    if (c == 'n') {
        if (!parse_literal(p, "null")) return NULL;
        return json_null();
    }
    perror_set(p, "unexpected character");
    return NULL;
}

JsonNode *json_parse_len(const char *text, size_t len) {
    JsonParser p = {.cur = text, .end = text + len, .error = {0}};
    skip_ws(&p);
    JsonNode *root = parse_value(&p);
    if (!root) return NULL;
    skip_ws(&p);
    if (p.cur != p.end) {
        perror_set(&p, "trailing data");
        json_free(root);
        return NULL;
    }
    return root;
}

JsonNode *json_parse(const char *text) {
    return text ? json_parse_len(text, strlen(text)) : NULL;
}

/* ---------------------------------------------------------------------- */
/* emitter                                                                 */
/* ---------------------------------------------------------------------- */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool ok;
} EmitBuf;

static bool eb_reserve(EmitBuf *b, size_t extra) {
    size_t needed = b->len + extra + 1;
    if (needed <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < needed) {
        if (cap > (size_t)-1 / 2) { b->ok = false; return false; }
        cap *= 2;
    }
    char *grown = realloc(b->buf, cap);
    if (!grown) { b->ok = false; return false; }
    b->buf = grown;
    b->cap = cap;
    return true;
}

static bool eb_put(EmitBuf *b, const char *s, size_t n) {
    if (!eb_reserve(b, n)) return false;
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
    return true;
}

static bool eb_putc(EmitBuf *b, char c) { return eb_put(b, &c, 1); }

static bool emit_string(EmitBuf *b, const char *s) {
    if (!eb_putc(b, '"')) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        unsigned char c = *p;
        switch (c) {
            case '"': if (!eb_put(b, "\\\"", 2)) return false; break;
            case '\\': if (!eb_put(b, "\\\\", 2)) return false; break;
            case '\b': if (!eb_put(b, "\\b", 2)) return false; break;
            case '\f': if (!eb_put(b, "\\f", 2)) return false; break;
            case '\n': if (!eb_put(b, "\\n", 2)) return false; break;
            case '\r': if (!eb_put(b, "\\r", 2)) return false; break;
            case '\t': if (!eb_put(b, "\\t", 2)) return false; break;
            default:
                if (c < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    if (!eb_put(b, esc, 6)) return false;
                } else if (!eb_putc(b, (char)c)) {
                    return false;
                }
        }
    }
    return eb_putc(b, '"');
}

static bool emit_node(EmitBuf *b, const JsonNode *n, bool pretty, int depth);

static bool emit_indent(EmitBuf *b, int depth) {
    for (int i = 0; i < depth; ++i) {
        if (!eb_put(b, "  ", 2)) return false;
    }
    return true;
}

static bool emit_number_node(EmitBuf *b, double v) {
    char tmp[40];
    if (v == (long long)v && v > -1e15 && v < 1e15) {
        snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    } else {
        snprintf(tmp, sizeof(tmp), "%.17g", v);
    }
    return eb_put(b, tmp, strlen(tmp));
}

static bool emit_node(EmitBuf *b, const JsonNode *n, bool pretty, int depth) {
    switch (n->type) {
        case JSON_NULL: return eb_put(b, "null", 4);
        case JSON_BOOL:
            return n->u.boolean ? eb_put(b, "true", 4)
                                : eb_put(b, "false", 5);
        case JSON_NUMBER: return emit_number_node(b, n->u.number);
        case JSON_STRING: return emit_string(b, n->u.string);
        case JSON_ARRAY:
            if (n->u.array.count == 0) return eb_put(b, "[]", 2);
            if (!eb_putc(b, '[')) return false;
            for (size_t i = 0; i < n->u.array.count; ++i) {
                if (pretty) { if (!eb_putc(b, '\n') || !emit_indent(b, depth + 1)) return false; }
                if (!emit_node(b, n->u.array.items[i], pretty, depth + 1)) return false;
                if (i + 1 < n->u.array.count && !eb_putc(b, ',')) return false;
            }
            if (pretty) { if (!eb_putc(b, '\n') || !emit_indent(b, depth)) return false; }
            return eb_putc(b, ']');
        case JSON_OBJECT:
            if (n->u.object.count == 0) return eb_put(b, "{}", 2);
            if (!eb_putc(b, '{')) return false;
            for (size_t i = 0; i < n->u.object.count; ++i) {
                if (pretty) { if (!eb_putc(b, '\n') || !emit_indent(b, depth + 1)) return false; }
                if (!emit_string(b, n->u.object.keys[i])) return false;
                if (!eb_put(b, pretty ? ": " : ":", pretty ? 2 : 1)) return false;
                if (!emit_node(b, n->u.object.values[i], pretty, depth + 1)) return false;
                if (i + 1 < n->u.object.count && !eb_putc(b, ',')) return false;
            }
            if (pretty) { if (!eb_putc(b, '\n') || !emit_indent(b, depth)) return false; }
            return eb_putc(b, '}');
    }
    return false;
}

static char *emit_root(const JsonNode *node, bool pretty, size_t *out_len) {
    EmitBuf b = {.buf = NULL, .len = 0, .cap = 0, .ok = true};
    if (!emit_node(&b, node, pretty, 0) || !b.ok) {
        free(b.buf);
        return NULL;
    }
    if (out_len) *out_len = b.len;
    return b.buf;
}

char *json_emit(const JsonNode *node, size_t *out_len) {
    return emit_root(node, false, out_len);
}
char *json_emit_pretty(const JsonNode *node, size_t *out_len) {
    return emit_root(node, true, out_len);
}

/* ---------------------------------------------------------------------- */
/* free / clone / equal                                                    */
/* ---------------------------------------------------------------------- */

void json_free(JsonNode *n) {
    if (!n) return;
    switch (n->type) {
        case JSON_STRING: free(n->u.string); break;
        case JSON_ARRAY:
            for (size_t i = 0; i < n->u.array.count; ++i)
                json_free(n->u.array.items[i]);
            free(n->u.array.items);
            break;
        case JSON_OBJECT:
            for (size_t i = 0; i < n->u.object.count; ++i) {
                free(n->u.object.keys[i]);
                json_free(n->u.object.values[i]);
            }
            free(n->u.object.keys);
            free(n->u.object.values);
            break;
        default: break;
    }
    free(n);
}

JsonNode *json_clone(const JsonNode *n) {
    if (!n) return NULL;
    switch (n->type) {
        case JSON_NULL: return json_null();
        case JSON_BOOL: return json_bool(n->u.boolean);
        case JSON_NUMBER: return json_number(n->u.number);
        case JSON_STRING: return json_string(n->u.string);
        case JSON_ARRAY: {
            JsonNode *arr = json_array_new();
            if (!arr) return NULL;
            for (size_t i = 0; i < n->u.array.count; ++i) {
                JsonNode *c = json_clone(n->u.array.items[i]);
                if (!c || !json_array_append(arr, c)) {
                    if (c) json_free(c);
                    json_free(arr); return NULL;
                }
            }
            return arr;
        }
        case JSON_OBJECT: {
            JsonNode *obj = json_object_new();
            if (!obj) return NULL;
            for (size_t i = 0; i < n->u.object.count; ++i) {
                JsonNode *c = json_clone(n->u.object.values[i]);
                if (!c || !json_object_set(obj, n->u.object.keys[i], c)) {
                    if (c) json_free(c);
                    json_free(obj); return NULL;
                }
            }
            return obj;
        }
    }
    return NULL;
}

bool json_equal(const JsonNode *a, const JsonNode *b) {
    if (a == b) return true;
    if (!a || !b || a->type != b->type) return false;
    switch (a->type) {
        case JSON_NULL: return true;
        case JSON_BOOL: return a->u.boolean == b->u.boolean;
        case JSON_NUMBER: return a->u.number == b->u.number;
        case JSON_STRING: return strcmp(a->u.string, b->u.string) == 0;
        case JSON_ARRAY:
            if (a->u.array.count != b->u.array.count) return false;
            for (size_t i = 0; i < a->u.array.count; ++i) {
                if (!json_equal(a->u.array.items[i], b->u.array.items[i]))
                    return false;
            }
            return true;
        case JSON_OBJECT:
            if (a->u.object.count != b->u.object.count) return false;
            for (size_t i = 0; i < a->u.object.count; ++i) {
                JsonNode *bv = json_object_get(b, a->u.object.keys[i]);
                if (!bv || !json_equal(a->u.object.values[i], bv))
                    return false;
            }
            return true;
    }
    return false;
}
