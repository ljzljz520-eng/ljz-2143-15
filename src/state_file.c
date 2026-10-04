#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "state_file.h"
#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

/* ---------------- CRC32 ---------------- */

static unsigned int crc32_table[256];
static int crc32_ready = 0;

static void crc32_init(void) {
    for (unsigned int i = 0; i < 256; i++) {
        unsigned int c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_ready = 1;
}

static unsigned int crc32_of(const unsigned char *data, size_t len) {
    if (!crc32_ready) crc32_init();
    unsigned int c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = crc32_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---------------- 时间 ---------------- */

void state_now_iso(char *buf, size_t len) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    gmtime_r(&ts.tv_sec, &tmv);
    char base[32];
    strftime(base, sizeof base, "%Y-%m-%dT%H:%M:%S", &tmv);
    snprintf(buf, len, "%s.%03ld+00:00", base, ts.tv_nsec / 1000000L);
}

/* ---------------- 初始化 / 释放 ---------------- */

void state_init(AppState *st, const char *device_id, const char *user_id, int schema) {
    memset(st, 0, sizeof(*st));
    snprintf(st->device_id, sizeof st->device_id, "%s", device_id ? device_id : "dev-unknown");
    snprintf(st->user_id, sizeof st->user_id, "%s", user_id ? user_id : "user-unknown");
    st->schema_version = schema > 0 ? schema : CFG_SCHEMA_VERSION;
    st->server_seq = 0;
    st->doc = cfg_default_doc_for(st->schema_version);
    st->meta = j_new(J_OBJ);
    st->journal = j_new(J_ARR);
    st->fallback_fields = j_new(J_ARR);
    st->monitors = j_new(J_ARR);
    st->recovered = 0;
}

void state_free(AppState *st) {
    if (!st) return;
    j_free(st->doc);
    j_free(st->meta);
    j_free(st->journal);
    j_free(st->fallback_fields);
    j_free(st->monitors);
    memset(st, 0, sizeof(*st));
}

/* ---------------- 序列化 ---------------- */

static JVal *state_to_json(const AppState *st) {
    JVal *o = j_new(J_OBJ);
    j_obj_set(o, "device_id", j_str(st->device_id));
    j_obj_set(o, "user_id", j_str(st->user_id));
    j_obj_set(o, "epoch", j_str(st->epoch));
    j_obj_set(o, "server_seq", j_num((double)st->server_seq));
    j_obj_set(o, "schema_version", j_num(st->schema_version));
    j_obj_set(o, "doc", j_copy(st->doc));
    j_obj_set(o, "meta", j_copy(st->meta));
    j_obj_set(o, "journal", j_copy(st->journal));
    j_obj_set(o, "fallback_fields", j_copy(st->fallback_fields));
    j_obj_set(o, "monitors", j_copy(st->monitors));
    return o;
}

static int state_from_json(AppState *st, const JVal *o, char *err, size_t errlen) {
    if (!o || o->type != J_OBJ) {
        snprintf(err, errlen, "payload is not an object");
        return -1;
    }
    const char *device = j_as_str(j_obj_get((JVal *)o, "device_id"), st->device_id);
    const char *user = j_as_str(j_obj_get((JVal *)o, "user_id"), st->user_id);
    snprintf(st->device_id, sizeof st->device_id, "%s", device);
    snprintf(st->user_id, sizeof st->user_id, "%s", user);
    snprintf(st->epoch, sizeof st->epoch, "%s",
             j_as_str(j_obj_get((JVal *)o, "epoch"), ""));
    st->server_seq = (long)j_as_num(j_obj_get((JVal *)o, "server_seq"), 0);

    JVal *doc = j_obj_get((JVal *)o, "doc");
    if (!doc || doc->type != J_OBJ) {
        snprintf(err, errlen, "missing doc");
        return -1;
    }
    j_free(st->doc);
    st->doc = j_copy(doc);

    JVal *m;
    if ((m = j_obj_get((JVal *)o, "meta")) && m->type == J_OBJ) {
        j_free(st->meta);
        st->meta = j_copy(m);
    }
    if ((m = j_obj_get((JVal *)o, "journal")) && m->type == J_ARR) {
        j_free(st->journal);
        st->journal = j_copy(m);
    }
    if ((m = j_obj_get((JVal *)o, "fallback_fields")) && m->type == J_ARR) {
        j_free(st->fallback_fields);
        st->fallback_fields = j_copy(m);
    }
    if ((m = j_obj_get((JVal *)o, "monitors")) && m->type == J_ARR) {
        j_free(st->monitors);
        st->monitors = j_copy(m);
    }

    /* 可验证迁移：旧版本状态文件升级；校验失败按损坏处理 */
    char mig_err[128] = {0};
    if (cfg_migrate_doc(st->doc, st->schema_version, mig_err, sizeof mig_err) != 0) {
        snprintf(err, errlen, "migration failed: %s", mig_err);
        return -1;
    }
    return 0;
}

/* ---------------- 文件读写 ---------------- */

static char *read_whole_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

static void path_join(char *out, size_t len, const char *dir, const char *name) {
    snprintf(out, len, "%s/%s", dir, name);
}

/* 损坏文件隔离为诊断副本，保留现场供排查（绝不同步其内容）。 */
static void quarantine(const char *dir, const char *path) {
    char ts[32];
    state_now_iso(ts, sizeof ts);
    for (char *p = ts; *p; p++)
        if (*p == ':' || *p == '+') *p = '-';
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char dest[1024];
    snprintf(dest, sizeof dest, "%s/%s.corrupt-%s", dir, base, ts);
    rename(path, dest);
}

void state_log_recovery(const char *dir, const char *fmt, ...) {
    char path[1024];
    path_join(path, sizeof path, dir, STATE_RECOVERY_LOG);
    FILE *f = fopen(path, "a");
    if (!f) return;
    char ts[40];
    state_now_iso(ts, sizeof ts);
    fprintf(f, "%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* 解析单个状态文件；成功返回 0。 */
static int load_one(AppState *st, const char *path, char *err, size_t errlen) {
    size_t len = 0;
    char *raw = read_whole_file(path, &len);
    if (!raw) {
        snprintf(err, errlen, "cannot read %s", path);
        return -1;
    }
    unsigned long payload_len = 0, crc_expected = 0;
    char magic[16] = {0};
    int header_len = 0;
    if (sscanf(raw, "%15s %lu %lx%n", magic, &payload_len, &crc_expected, &header_len) != 3 ||
        strcmp(magic, "TERMREC1") != 0) {
        snprintf(err, errlen, "bad header in %s", path);
        free(raw);
        return -1;
    }
    const char *payload = raw + header_len;
    if (*payload == '\n') { payload++; }
    size_t actual_len = len - (size_t)(payload - raw);
    if (actual_len < payload_len) {
        snprintf(err, errlen, "truncated payload in %s (%zu < %lu)", path, actual_len, payload_len);
        free(raw);
        return -1;
    }
    unsigned long crc_actual = crc32_of((const unsigned char *)payload, payload_len);
    if (crc_actual != crc_expected) {
        snprintf(err, errlen, "crc mismatch in %s", path);
        free(raw);
        return -1;
    }
    char *payload_copy = strndup(payload, payload_len);
    free(raw);
    if (!payload_copy) {
        snprintf(err, errlen, "oom");
        return -1;
    }
    const char *jerr = NULL;
    JVal *parsed = j_parse(payload_copy, &jerr);
    free(payload_copy);
    if (!parsed) {
        snprintf(err, errlen, "json error in %s: %s", path, jerr ? jerr : "?");
        return -1;
    }
    int rc = state_from_json(st, parsed, err, errlen);
    j_free(parsed);
    return rc;
}

int state_load(AppState *st, const char *dir, char *err, size_t errlen) {
    char main_path[1024], bak_path[1024];
    path_join(main_path, sizeof main_path, dir, STATE_FILE_NAME);
    path_join(bak_path, sizeof bak_path, dir, STATE_BACKUP_NAME);

    char first_err[256] = {0};
    if (load_one(st, main_path, first_err, sizeof first_err) == 0) {
        st->recovered = 0;
        return 0;
    }
    if (access(main_path, F_OK) == 0) {
        /* 主文件存在但损坏（例如写一半断电）：隔离诊断副本 */
        quarantine(dir, main_path);
        state_log_recovery(dir, "corrupt main state quarantined: %s", first_err);
    }
    if (load_one(st, bak_path, err, errlen) == 0) {
        st->recovered = 1;
        snprintf(st->note, sizeof st->note, "recovered from backup: %s", first_err);
        snprintf(err, errlen, "recovered from backup: %s", first_err);
        state_log_recovery(dir, "recovered from backup: %s", first_err);
        return 1;
    }
    if (access(bak_path, F_OK) == 0) {
        quarantine(dir, bak_path);
        state_log_recovery(dir, "corrupt backup quarantined: %s", err);
    }
    /* 双重损坏 / 首次启动：内置兜底默认，全部字段标记 fallback（不同步） */
    st->recovered = 2;
    for (int i = 0; i < CFG_DEFAULTS_LEN; i++)
        j_arr_push(st->fallback_fields, j_str(CFG_DEFAULTS[i].field));
    snprintf(st->note, sizeof st->note, "fallback defaults in use: %s", first_err);
    snprintf(err, errlen, "fallback defaults: %s", first_err);
    state_log_recovery(dir, "fallback defaults in use: %s", first_err);
    return 2;
}

int state_save(AppState *st, const char *dir, char *err, size_t errlen) {
    JVal *payload_obj = state_to_json(st);
    char *payload = j_write(payload_obj);
    j_free(payload_obj);
    if (!payload) {
        snprintf(err, errlen, "serialize failed");
        return -1;
    }
    unsigned long crc = crc32_of((const unsigned char *)payload, strlen(payload));

    char tmp_path[1024], main_path[1024], bak_path[1024];
    path_join(tmp_path, sizeof tmp_path, dir, "state.json.tmp");
    path_join(main_path, sizeof main_path, dir, STATE_FILE_NAME);
    path_join(bak_path, sizeof bak_path, dir, STATE_BACKUP_NAME);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        snprintf(err, errlen, "cannot open tmp file");
        free(payload);
        return -1;
    }
    fprintf(f, "TERMREC1 %zu %lx\n", strlen(payload), crc);
    fputs(payload, f);
    free(payload);
    fflush(f);
    fsync(fileno(f));          /* 落盘后再改名，防写一半断电留下半文件 */
    fclose(f);

    if (access(main_path, F_OK) == 0)
        rename(main_path, bak_path);   /* 上一份完好记录留作备份 */
    if (rename(tmp_path, main_path) != 0) {
        snprintf(err, errlen, "rename failed");
        return -1;
    }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
    return 0;
}

/* ---------------- 本地修改 ---------------- */

int state_is_fallback(const AppState *st, const char *field) {
    if (!st->fallback_fields) return 0;
    for (int i = 0; i < st->fallback_fields->len; i++) {
        const char *f = j_as_str(st->fallback_fields->items[i], NULL);
        if (f && strcmp(f, field) == 0) return 1;
    }
    return 0;
}

void state_clear_fallback(AppState *st, const char *field) {
    JVal *arr = st->fallback_fields;
    if (!arr || arr->type != J_ARR) return;
    for (int i = 0; i < arr->len; i++) {
        const char *f = j_as_str(arr->items[i], NULL);
        if (f && strcmp(f, field) == 0) {
            j_free(arr->items[i]);
            for (int j = i; j < arr->len - 1; j++)
                arr->items[j] = arr->items[j + 1];
            arr->len--;
            return;
        }
    }
}

static double meta_version(const AppState *st, const char *field) {
    JVal *m = j_obj_get(st->meta, field);
    if (!m) return 0;
    return j_as_num(j_obj_get(m, "version"), 0);
}

void state_local_set(AppState *st, const char *field, JVal *value) {
    /* 调用方可能传入 doc 内部节点的指针（如 rejournal/窗口钳制），
     * 必须先复制再写 doc，否则 j_set_path 释放旧节点后 value 成悬垂指针。 */
    JVal *owned = j_copy(value);
    j_set_path(st->doc, field, j_copy(owned));

    char ts[40];
    state_now_iso(ts, sizeof ts);
    char source[192];
    snprintf(source, sizeof source, "%s:%s",
             cfg_owner_is_device(field) ? "device" : "user",
             cfg_owner_is_device(field) ? st->device_id : st->user_id);

    JVal *entry = j_new(J_OBJ);
    j_obj_set(entry, "field", j_str(field));
    j_obj_set(entry, "value", j_copy(owned));
    j_obj_set(entry, "base_version", j_num(meta_version(st, field)));
    j_obj_set(entry, "ts", j_str(ts));
    j_obj_set(entry, "source", j_str(source));
    j_arr_push(st->journal, entry);

    JVal *m = j_new(J_OBJ);
    j_obj_set(m, "version", j_num(meta_version(st, field)));
    j_obj_set(m, "source", j_str("local-pending"));
    j_obj_set(m, "ts", j_str(ts));
    j_obj_set(st->meta, field, m);

    state_clear_fallback(st, field);
    j_free(owned);
}

int state_journal_has(const AppState *st, const char *field) {
    if (!st->journal) return 0;
    for (int i = 0; i < st->journal->len; i++) {
        const char *f = j_as_str(j_obj_get(st->journal->items[i], "field"), NULL);
        if (f && strcmp(f, field) == 0) return 1;
    }
    return 0;
}
