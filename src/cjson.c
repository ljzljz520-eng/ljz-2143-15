#include "cjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 构造 / 销毁 ---------------- */

JVal *j_new(JType type) {
    JVal *v = calloc(1, sizeof(JVal));
    if (v) v->type = type;
    return v;
}

JVal *j_num(double v) { JVal *j = j_new(J_NUM); if (j) j->num = v; return j; }

JVal *j_str(const char *s) {
    JVal *j = j_new(J_STR);
    if (!j) return NULL;
    j->str = malloc(strlen(s) + 1);
    if (!j->str) { free(j); return NULL; }
    strcpy(j->str, s);
    return j;
}

JVal *j_bool(int b) { return j_new(b ? J_TRUE : J_FALSE); }
JVal *j_null(void) { return j_new(J_NULL); }

void j_free(JVal *v) {
    if (!v) return;
    if (v->type == J_STR) free(v->str);
    if (v->type == J_ARR) {
        for (int i = 0; i < v->len; i++) j_free(v->items[i]);
        free(v->items);
    }
    if (v->type == J_OBJ) {
        for (int i = 0; i < v->olen; i++) {
            free(v->keys[i]);
            j_free(v->vals[i]);
        }
        free(v->keys);
        free(v->vals);
    }
    free(v);
}

JVal *j_copy(const JVal *v) {
    if (!v) return j_null();
    switch (v->type) {
    case J_NULL: case J_FALSE: case J_TRUE: return j_new(v->type);
    case J_NUM: return j_num(v->num);
    case J_STR: return j_str(v->str);
    case J_ARR: {
        JVal *a = j_new(J_ARR);
        for (int i = 0; i < v->len; i++) j_arr_push(a, j_copy(v->items[i]));
        return a;
    }
    case J_OBJ: {
        JVal *o = j_new(J_OBJ);
        for (int i = 0; i < v->olen; i++) j_obj_set(o, v->keys[i], j_copy(v->vals[i]));
        return o;
    }
    }
    return j_null();
}

/* ---------------- 容器操作 ---------------- */

void j_arr_push(JVal *arr, JVal *val) {
    if (!arr || arr->type != J_ARR || !val) return;
    if (arr->len == arr->cap) {
        int ncap = arr->cap ? arr->cap * 2 : 8;
        JVal **ni = realloc(arr->items, (size_t)ncap * sizeof(JVal *));
        if (!ni) return;
        arr->items = ni;
        arr->cap = ncap;
    }
    arr->items[arr->len++] = val;
}

static int obj_find(const JVal *obj, const char *key) {
    for (int i = 0; i < obj->olen; i++)
        if (strcmp(obj->keys[i], key) == 0) return i;
    return -1;
}

JVal *j_obj_get(const JVal *obj, const char *key) {
    if (!obj || obj->type != J_OBJ) return NULL;
    int i = obj_find(obj, key);
    return i >= 0 ? obj->vals[i] : NULL;
}

void j_obj_set(JVal *obj, const char *key, JVal *val) {
    if (!obj || obj->type != J_OBJ || !val) return;
    int i = obj_find(obj, key);
    if (i >= 0) {
        j_free(obj->vals[i]);
        obj->vals[i] = val;
        return;
    }
    if (obj->olen == obj->ocap) {
        int ncap = obj->ocap ? obj->ocap * 2 : 8;
        char **nk = realloc(obj->keys, (size_t)ncap * sizeof(char *));
        JVal **nv = realloc(obj->vals, (size_t)ncap * sizeof(JVal *));
        if (!nk || !nv) { free(nk); free(nv); return; }
        obj->keys = nk;
        obj->vals = nv;
        obj->ocap = ncap;
    }
    obj->keys[obj->olen] = malloc(strlen(key) + 1);
    if (!obj->keys[obj->olen]) return;
    strcpy(obj->keys[obj->olen], key);
    obj->vals[obj->olen] = val;
    obj->olen++;
}

void j_obj_del(JVal *obj, const char *key) {
    if (!obj || obj->type != J_OBJ) return;
    int i = obj_find(obj, key);
    if (i < 0) return;
    free(obj->keys[i]);
    j_free(obj->vals[i]);
    for (int j = i; j < obj->olen - 1; j++) {
        obj->keys[j] = obj->keys[j + 1];
        obj->vals[j] = obj->vals[j + 1];
    }
    obj->olen--;
}

/* ---------------- 路径访问 ---------------- */

JVal *j_get_path(JVal *root, const char *path) {
    JVal *cur = root;
    char buf[256];
    const char *p = path;
    while (cur && p && *p) {
        const char *dot = strchr(p, '.');
        size_t n = dot ? (size_t)(dot - p) : strlen(p);
        if (n >= sizeof(buf)) return NULL;
        memcpy(buf, p, n);
        buf[n] = '\0';
        cur = j_obj_get(cur, buf);
        p = dot ? dot + 1 : NULL;
    }
    return cur;
}

int j_set_path(JVal *root, const char *path, JVal *val) {
    if (!root || root->type != J_OBJ || !path || !val) return -1;
    JVal *cur = root;
    char buf[256];
    const char *p = path;
    while (p && *p) {
        const char *dot = strchr(p, '.');
        size_t n = dot ? (size_t)(dot - p) : strlen(p);
        if (n >= sizeof(buf)) return -1;
        memcpy(buf, p, n);
        buf[n] = '\0';
        if (!dot) {
            j_obj_set(cur, buf, val);
            return 0;
        }
        JVal *next = j_obj_get(cur, buf);
        if (!next || next->type != J_OBJ) {
            next = j_new(J_OBJ);
            j_obj_set(cur, buf, next);
        }
        cur = next;
        p = dot + 1;
    }
    return -1;
}

double j_as_num(const JVal *v, double dflt) {
    return (v && v->type == J_NUM) ? v->num : dflt;
}

const char *j_as_str(const JVal *v, const char *dflt) {
    return (v && v->type == J_STR) ? v->str : dflt;
}

/* ---------------- 解析 ---------------- */

typedef struct {
    const char *p;
    const char *err;
} Parser;

static void skip_ws(Parser *ps) {
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
        ps->p++;
}

static JVal *parse_value(Parser *ps);

static JVal *parse_string(Parser *ps) {
    /* 假定当前字符是 '"' */
    ps->p++;
    size_t cap = 16, n = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    while (*ps->p && *ps->p != '"') {
        char c = *ps->p++;
        if (c == '\\') {
            char e = *ps->p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case '/': c = '/'; break;
            case '\\': c = '\\'; break;
            case '"': c = '"'; break;
            case 'u': {
                /* 仅处理基本多文种平面内可转 ASCII 的部分，其余用 '?' 代替 */
                unsigned code = 0;
                for (int i = 0; i < 4 && ps->p[i]; i++) {
                    char h = ps->p[i];
                    code <<= 4;
                    if (h >= '0' && h <= '9') code |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') code |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') code |= (unsigned)(h - 'A' + 10);
                }
                ps->p += 4;
                c = (code < 0x80) ? (char)code : '?';
                break;
            }
            default: c = e; break;
            }
        }
        if (n + 1 >= cap) {
            cap *= 2;
            char *no = realloc(out, cap);
            if (!no) { free(out); return NULL; }
            out = no;
        }
        out[n++] = c;
    }
    if (*ps->p != '"') {
        free(out);
        ps->err = "unterminated string";
        return NULL;
    }
    ps->p++;
    out[n] = '\0';
    JVal *v = j_new(J_STR);
    if (!v) { free(out); return NULL; }
    v->str = out;
    return v;
}

static JVal *parse_number(Parser *ps) {
    char *end = NULL;
    double d = strtod(ps->p, &end);
    if (end == ps->p) {
        ps->err = "bad number";
        return NULL;
    }
    ps->p = end;
    return j_num(d);
}

static int expect(Parser *ps, const char *word) {
    size_t n = strlen(word);
    if (strncmp(ps->p, word, n) == 0) {
        ps->p += n;
        return 1;
    }
    return 0;
}

static JVal *parse_array(Parser *ps) {
    ps->p++; /* '[' */
    JVal *arr = j_new(J_ARR);
    skip_ws(ps);
    if (*ps->p == ']') { ps->p++; return arr; }
    for (;;) {
        JVal *item = parse_value(ps);
        if (!item) { j_free(arr); return NULL; }
        j_arr_push(arr, item);
        skip_ws(ps);
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == ']') { ps->p++; return arr; }
        ps->err = "expected ',' or ']'";
        j_free(arr);
        return NULL;
    }
}

static JVal *parse_object(Parser *ps) {
    ps->p++; /* '{' */
    JVal *obj = j_new(J_OBJ);
    skip_ws(ps);
    if (*ps->p == '}') { ps->p++; return obj; }
    for (;;) {
        skip_ws(ps);
        if (*ps->p != '"') {
            ps->err = "expected object key";
            j_free(obj);
            return NULL;
        }
        JVal *key = parse_string(ps);
        if (!key) { j_free(obj); return NULL; }
        skip_ws(ps);
        if (*ps->p != ':') {
            ps->err = "expected ':'";
            j_free(key);
            j_free(obj);
            return NULL;
        }
        ps->p++;
        JVal *val = parse_value(ps);
        if (!val) { j_free(key); j_free(obj); return NULL; }
        j_obj_set(obj, key->str, val);
        j_free(key);
        skip_ws(ps);
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == '}') { ps->p++; return obj; }
        ps->err = "expected ',' or '}'";
        j_free(obj);
        return NULL;
    }
}

static JVal *parse_value(Parser *ps) {
    skip_ws(ps);
    switch (*ps->p) {
    case '{': return parse_object(ps);
    case '[': return parse_array(ps);
    case '"': return parse_string(ps);
    case 't': if (expect(ps, "true")) return j_bool(1); break;
    case 'f': if (expect(ps, "false")) return j_bool(0); break;
    case 'n': if (expect(ps, "null")) return j_null(); break;
    default:
        if (*ps->p == '-' || (*ps->p >= '0' && *ps->p <= '9'))
            return parse_number(ps);
        break;
    }
    ps->err = "unexpected token";
    return NULL;
}

JVal *j_parse(const char *text, const char **err_out) {
    Parser ps = { text, NULL };
    JVal *v = parse_value(&ps);
    skip_ws(&ps);
    if (v && *ps.p != '\0') {
        ps.err = "trailing garbage";
        j_free(v);
        v = NULL;
    }
    if (err_out) *err_out = ps.err ? ps.err : "";
    return v;
}

/* ---------------- 序列化 ---------------- */

typedef struct {
    char *buf;
    size_t len, cap;
} SBuf;

static int sb_putn(SBuf *sb, const char *s, size_t n) {
    if (sb->len + n + 1 > sb->cap) {
        size_t ncap = sb->cap ? sb->cap : 64;
        while (sb->len + n + 1 > ncap) ncap *= 2;
        char *nb = realloc(sb->buf, ncap);
        if (!nb) return -1;
        sb->buf = nb;
        sb->cap = ncap;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
    return 0;
}

static int sb_puts(SBuf *sb, const char *s) { return sb_putn(sb, s, strlen(s)); }
static int sb_putc(SBuf *sb, char c) { return sb_putn(sb, &c, 1); }

static int write_value(SBuf *sb, const JVal *v);

static int write_string(SBuf *sb, const char *s) {
    sb_putc(sb, '"');
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"': sb_puts(sb, "\\\""); break;
        case '\\': sb_puts(sb, "\\\\"); break;
        case '\n': sb_puts(sb, "\\n"); break;
        case '\t': sb_puts(sb, "\\t"); break;
        case '\r': sb_puts(sb, "\\r"); break;
        default:
            if (c < 0x20) {
                char esc[8];
                snprintf(esc, sizeof esc, "\\u%04x", c);
                sb_puts(sb, esc);
            } else {
                sb_putc(sb, (char)c);
            }
        }
    }
    return sb_putc(sb, '"');
}

static int write_value(SBuf *sb, const JVal *v) {
    char numbuf[40];
    switch (v->type) {
    case J_NULL: return sb_puts(sb, "null");
    case J_FALSE: return sb_puts(sb, "false");
    case J_TRUE: return sb_puts(sb, "true");
    case J_NUM: {
        double d = v->num;
        long long as_int = (long long)d;
        if ((double)as_int == d)
            snprintf(numbuf, sizeof numbuf, "%lld", as_int);
        else
            snprintf(numbuf, sizeof numbuf, "%.17g", d);
        return sb_puts(sb, numbuf);
    }
    case J_STR: return write_string(sb, v->str);
    case J_ARR:
        sb_putc(sb, '[');
        for (int i = 0; i < v->len; i++) {
            if (i) sb_putc(sb, ',');
            write_value(sb, v->items[i]);
        }
        return sb_putc(sb, ']');
    case J_OBJ:
        sb_putc(sb, '{');
        for (int i = 0; i < v->olen; i++) {
            if (i) sb_putc(sb, ',');
            write_string(sb, v->keys[i]);
            sb_putc(sb, ':');
            write_value(sb, v->vals[i]);
        }
        return sb_putc(sb, '}');
    }
    return -1;
}

char *j_write(const JVal *v) {
    SBuf sb = {0};
    if (write_value(&sb, v) != 0) {
        free(sb.buf);
        return NULL;
    }
    if (!sb.buf) {
        sb.buf = malloc(1);
        if (sb.buf) sb.buf[0] = '\0';
    }
    return sb.buf;
}
