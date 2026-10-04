#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "sync_client.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

/* ---------------- 最小 HTTP 客户端 ---------------- */

typedef struct {
    char host[256];
    int port;
    char path[512];
} Url;

static int parse_url(const char *url, Url *out, char *err, size_t errlen) {
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        size_t n = (size_t)(colon - p);
        if (n >= sizeof(out->host)) goto bad;
        memcpy(out->host, p, n);
        out->host[n] = '\0';
        out->port = atoi(colon + 1);
    } else {
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        if (n >= sizeof(out->host)) goto bad;
        memcpy(out->host, p, n);
        out->host[n] = '\0';
        out->port = 80;
    }
    snprintf(out->path, sizeof out->path, "%s", slash ? slash : "/");
    return 0;
bad:
    snprintf(err, errlen, "bad url: %s", url);
    return -1;
}

static int connect_to(const char *host, int port, char *err, size_t errlen) {
    char port_str[16];
    snprintf(port_str, sizeof port_str, "%d", port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        snprintf(err, errlen, "resolve failed: %s", host);
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) snprintf(err, errlen, "connect failed: %s:%d", host, port);
    return fd;
}

int http_request(const char *method, const char *url, const char *body,
                 char **resp_out, char *err, size_t errlen) {
    Url u;
    if (parse_url(url, &u, err, errlen) != 0) return -1;
    int fd = connect_to(u.host, u.port, err, errlen);
    if (fd < 0) return -1;

    size_t body_len = body ? strlen(body) : 0;
    char header[1024];
    int hn = snprintf(header, sizeof header,
        "%s %s HTTP/1.0\r\nHost: %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
        method, u.path, u.host, body_len);
    if (write(fd, header, (size_t)hn) != hn ||
        (body_len > 0 && write(fd, body, body_len) != (ssize_t)body_len)) {
        snprintf(err, errlen, "write failed");
        close(fd);
        return -1;
    }

    size_t cap = 1 << 16, len = 0;
    char *buf = malloc(cap);
    if (!buf) { close(fd); return -1; }
    for (;;) {
        if (len + 4096 + 1 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); close(fd); return -1; }
            buf = nb;
        }
        ssize_t n = read(fd, buf + len, 4096);
        if (n < 0) {
            snprintf(err, errlen, "read failed");
            free(buf);
            close(fd);
            return -1;
        }
        if (n == 0) break;
        len += (size_t)n;
    }
    close(fd);
    buf[len] = '\0';

    char *body_start = strstr(buf, "\r\n\r\n");
    if (!body_start) {
        snprintf(err, errlen, "bad http response");
        free(buf);
        return -1;
    }
    int status = 0;
    sscanf(buf, "HTTP/%*s %d", &status);
    body_start += 4;
    char *resp = strdup(body_start);
    free(buf);
    if (status < 200 || status >= 300) {
        snprintf(err, errlen, "http %d: %.120s", status, resp);
        free(resp);
        return -1;
    }
    *resp_out = resp;
    return 0;
}

/* ---------------- 合并辅助 ---------------- */

static double meta_version(const AppState *st, const char *field) {
    JVal *m = j_obj_get(st->meta, field);
    return m ? j_as_num(j_obj_get(m, "version"), 0) : 0;
}

static void set_meta(AppState *st, const char *field, double version, const char *source) {
    char ts[40];
    state_now_iso(ts, sizeof ts);
    JVal *m = j_new(J_OBJ);
    j_obj_set(m, "version", j_num(version));
    j_obj_set(m, "source", j_str(source));
    j_obj_set(m, "ts", j_str(ts));
    j_obj_set(st->meta, field, m);
}

/* 应用服务器下发的字段（增量或快照共用）。
 * 规则：
 *  - 本地有待推送 journal 的字段一律不动（本地变更优先，推送时由服务器裁决）；
 *  - 设备私有字段（machine.*）本地为准，仅本地无版本记录时才采用服务器存档；
 *  - 用户层/默认层字段按版本覆盖；force=1（resync）时无视本地版本强制采用，
 *    因为服务器回退/换库后版本号可能倒退，单调性假设已失效。 */
static void apply_remote_field(AppState *st, const char *field, JVal *info,
                               int is_snapshot, int force) {
    double version = j_as_num(j_obj_get(info, "version"), 0);
    const char *source = j_as_str(j_obj_get(info, "source"), "server");
    if (state_journal_has(st, field))
        return; /* 本地待推送变更优先 */
    if (cfg_owner_is_device(field)) {
        if (version <= meta_version(st, field))
            return; /* 本机坐标本地为准 */
    } else if (!force && version <= meta_version(st, field)) {
        return; /* 本地基线已不旧于服务器 */
    }
    if (is_snapshot && cfg_owner_is_device(field) && version == 0)
        return; /* 快照里的内置机器兜底值不覆盖本地 */
    JVal *value = j_obj_get(info, "value");
    if (!value) return;
    const char *jerr = NULL;
    JVal *parsed = j_parse(j_as_str(value, "null"), &jerr);
    if (!parsed) return;
    j_set_path(st->doc, field, parsed);
    set_meta(st, field, version, source);
    state_clear_fallback(st, field);
}

static void apply_fields_obj(AppState *st, JVal *fields, int is_snapshot, int force) {
    if (!fields || fields->type != J_OBJ) return;
    for (int i = 0; i < fields->olen; i++)
        apply_remote_field(st, fields->keys[i], fields->vals[i], is_snapshot, force);
}

/* ---------------- 推送 ---------------- */

/* 从 journal 中移除已裁决的条目（accepted/conflict_won/conflict_lost/rejected_fallback）。 */
static void consume_journal(AppState *st, JVal *results) {
    if (!results || results->type != J_ARR) return;
    for (int r = 0; r < results->len; r++) {
        JVal *res = results->items[r];
        const char *field = j_as_str(j_obj_get(res, "field"), "");
        const char *status = j_as_str(j_obj_get(res, "status"), "");
        double version = j_as_num(j_obj_get(res, "version"), 0);
        if (strcmp(status, "accepted") == 0 || strcmp(status, "conflict_won") == 0) {
            char source[192];
            snprintf(source, sizeof source, "%s:%s",
                     cfg_owner_is_device(field) ? "device" : "user",
                     cfg_owner_is_device(field) ? st->device_id : st->user_id);
            set_meta(st, field, version, source);
        }
        /* conflict_lost：等 pull 拿回胜方值；rejected_fallback：本就不该推 */
        for (int i = st->journal->len - 1; i >= 0; i--) {
            const char *jf = j_as_str(j_obj_get(st->journal->items[i], "field"), "");
            if (strcmp(jf, field) == 0) {
                j_free(st->journal->items[i]);
                for (int j = i; j < st->journal->len - 1; j++)
                    st->journal->items[j] = st->journal->items[j + 1];
                st->journal->len--;
            }
        }
    }
}

static int do_push(AppState *st, const char *server_url, int *resync_out,
                   char *log, size_t loglen, size_t *logoff) {
    if (st->journal->len == 0) return SYNC_OK;

    JVal *body = j_new(J_OBJ);
    j_obj_set(body, "device_id", j_str(st->device_id));
    j_obj_set(body, "user", j_str(st->user_id));
    j_obj_set(body, "schema_version", j_num(st->schema_version));
    j_obj_set(body, "base_seq", j_num((double)st->server_seq));
    j_obj_set(body, "epoch", j_str(st->epoch));
    j_obj_set(body, "changes", j_copy(st->journal));
    char *text = j_write(body);
    j_free(body);

    char url[600];
    snprintf(url, sizeof url, "%s/api/push", server_url);
    char *resp = NULL, err[256] = {0};
    int rc = http_request("POST", url, text, &resp, err, sizeof err);
    free(text);
    if (rc != 0) {
        *logoff += (size_t)snprintf(log + *logoff, loglen - *logoff,
                                    "push unreachable: %s; journal kept (%d entries)\n",
                                    err, st->journal->len);
        return SYNC_OFFLINE;
    }
    const char *jerr = NULL;
    JVal *parsed = j_parse(resp, &jerr);
    free(resp);
    if (!parsed) return SYNC_PROTOCOL;

    int pushed = st->journal->len;
    JVal *results = j_obj_get(parsed, "results");
    consume_journal(st, results);
    *resync_out = j_obj_get(parsed, "resync_required") &&
                  j_obj_get(parsed, "resync_required")->type == J_TRUE;
    JVal *seq = j_obj_get(parsed, "server_seq");
    if (seq) st->server_seq = (long)seq->num;
    JVal *epoch = j_obj_get(parsed, "epoch");
    if (epoch && epoch->type == J_STR)
        snprintf(st->epoch, sizeof st->epoch, "%s", epoch->str);
    *logoff += (size_t)snprintf(log + *logoff, loglen - *logoff,
                                "pushed %d journal entries\n", pushed);
    j_free(parsed);
    return SYNC_OK;
}

/* ---------------- 拉取 ---------------- */

static int do_pull(AppState *st, const char *server_url, long since_seq, int force,
                   int *resync_out, char *log, size_t loglen, size_t *logoff) {
    char url[600];
    snprintf(url, sizeof url, "%s/api/pull?device_id=%s&user=%s&since_seq=%ld&epoch=%s",
             server_url, st->device_id, st->user_id, since_seq, st->epoch);
    char *resp = NULL, err[256] = {0};
    if (http_request("GET", url, NULL, &resp, err, sizeof err) != 0) {
        *logoff += (size_t)snprintf(log + *logoff, loglen - *logoff,
                                    "pull unreachable: %s\n", err);
        return SYNC_OFFLINE;
    }
    const char *jerr = NULL;
    JVal *parsed = j_parse(resp, &jerr);
    free(resp);
    if (!parsed) return SYNC_PROTOCOL;

    int resync = j_obj_get(parsed, "resync_required") &&
                 j_obj_get(parsed, "resync_required")->type == J_TRUE;
    *resync_out = resync;
    if (resync) {
        const char *reason = j_as_str(j_obj_get(parsed, "reason"), "?");
        *logoff += (size_t)snprintf(log + *logoff, loglen - *logoff,
                                    "resync required: %s\n", reason);
        state_log_recovery(".", "server requested resync: %s", reason);
    }
    JVal *snapshot = j_obj_get(parsed, "snapshot");
    JVal *changed = j_obj_get(parsed, "changed_fields");
    if (snapshot) {
        apply_fields_obj(st, snapshot, 1, force);
        *logoff += (size_t)snprintf(log + *logoff, loglen - *logoff,
                                    "applied snapshot (%d fields)\n", snapshot->olen);
    } else if (changed) {
        apply_fields_obj(st, changed, 0, 0);
        *logoff += (size_t)snprintf(log + *logoff, loglen - *logoff,
                                    "applied %d changed fields\n", changed->olen);
    }
    JVal *seq = j_obj_get(parsed, "server_seq");
    if (seq) st->server_seq = (long)seq->num;
    JVal *epoch = j_obj_get(parsed, "epoch");
    if (epoch && epoch->type == J_STR)
        snprintf(st->epoch, sizeof st->epoch, "%s", epoch->str);
    j_free(parsed);
    return SYNC_OK;
}

/* resync 时把本机机器字段重新入队：服务器回退/换库后其设备层存档
 * 可能丢失，本机坐标需要重新建档（fallback 字段除外，绝不同步）。 */
static void rejournal_machine_fields(AppState *st) {
    for (int i = 0; i < CFG_DEFAULTS_LEN; i++) {
        const char *field = CFG_DEFAULTS[i].field;
        if (!cfg_owner_is_device(field)) continue;
        if (state_is_fallback(st, field)) continue;
        if (state_journal_has(st, field)) continue;
        JVal *v = j_get_path(st->doc, field);
        if (v) state_local_set(st, field, v);
    }
}

int sync_now(AppState *st, const char *server_url, char *log, size_t loglen) {
    size_t off = 0;
    log[0] = '\0';
    int resync = 0;
    /* 推送前的版本基线：之后的拉取必须从这里开始，
     * 否则推送响应里的新 seq 会盖住推送窗口期内其他方写入的变更。 */
    long base_seq = st->server_seq;

    for (int round = 0; round < 3; round++) {
        int rc = do_push(st, server_url, &resync, log, loglen, &off);
        if (rc != SYNC_OK) return rc;
        if (!resync) {
            rc = do_pull(st, server_url, base_seq, 0, &resync, log, loglen, &off);
            if (rc != SYNC_OK) return rc;
            if (!resync) break;
        }
        if (resync) {
            /* 服务器回退 / epoch 更换 / 长离线超出保留窗：
             * 快照强制刷新用户层与默认层（服务器历史可能倒退），
             * 本机机器字段重新建档。 */
            off += (size_t)snprintf(log + off, loglen - off,
                                    "resync: snapshot + re-register machine fields\n");
            rc = do_pull(st, server_url, 0, 1, &resync, log, loglen, &off);
            if (rc != SYNC_OK) return rc;
            rejournal_machine_fields(st);
            base_seq = st->server_seq;
            resync = 0;
        }
    }
    off += (size_t)snprintf(log + off, loglen - off, "sync ok: server_seq=%ld\n",
                            st->server_seq);
    return SYNC_OK;
}

/* ---------------- 诊断副本上传 ---------------- */

int sync_upload_diagnostics(const AppState *st, const char *dir,
                            const char *server_url, char *log, size_t loglen) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int uploaded = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strstr(de->d_name, ".corrupt-")) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size < 0 || size > 60000) { fclose(f); continue; }
        char *content = malloc((size_t)size + 1);
        if (!content) { fclose(f); continue; }
        size_t got = fread(content, 1, (size_t)size, f);
        fclose(f);
        content[got] = '\0';

        JVal *body = j_new(J_OBJ);
        j_obj_set(body, "device_id", j_str(st->device_id));
        j_obj_set(body, "kind", j_str("corrupt"));
        j_obj_set(body, "filename", j_str(de->d_name));
        j_obj_set(body, "content", j_str(content));
        free(content);
        char *text = j_write(body);
        j_free(body);

        char url[600];
        snprintf(url, sizeof url, "%s/api/diagnostics", server_url);
        char *resp = NULL, err[256] = {0};
        if (http_request("POST", url, text, &resp, err, sizeof err) == 0) {
            uploaded++;
            free(resp);
        } else {
            snprintf(log, loglen, "upload failed for %s: %s", de->d_name, err);
            free(text);
            closedir(d);
            return -1;
        }
        free(text);
    }
    closedir(d);
    snprintf(log, loglen, "uploaded %d diagnostic copies", uploaded);
    return uploaded;
}
