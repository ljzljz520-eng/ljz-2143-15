#define _POSIX_C_SOURCE 200809L
#include "config.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "config_registry.h"
#include "geometry.h"
#include "migrate.h"

#define MAX_RESTORE_EVENTS 64
#define STATE_NAME "state.json"
#define TMP_NAME "state.json.tmp"
#define JOURNAL_NAME "journal.jsonl"
#define BACKUP_NAME "state.backup.json"
#define RESTORE_NAME "restore.jsonl"
#define DIAG_DIR "diag"

struct ConfigStore {
    char *dir;
    char *device_id;
    ConfigHooks hooks;
    int commit_calls;
    int lock_fd;
    long long hlc;
    JsonNode *root;
    RestoreEvent events[MAX_RESTORE_EVENTS];
    int event_count;
    bool used_fallback;
};

/* ---------------------------------------------------------------------- */
/* small helpers                                                           */
/* ---------------------------------------------------------------------- */

static long long default_clock(void *user) {
    (void)user;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long long now_ms(ConfigStore *s) {
    if (s->hooks.now_ms) return s->hooks.now_ms(s->hooks.clock_user);
    return default_clock(NULL);
}

static char *join_path(ConfigStore *s, const char *name) {
    size_t n = strlen(s->dir) + strlen(name) + 2;
    char *p = malloc(n);
    if (!p) return NULL;
    snprintf(p, n, "%s/%s", s->dir, name);
    return p;
}

static int mkdir_p(const char *path) {
    char tmp[1024];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int lock_acquire(ConfigStore *s) {
    if (s->lock_fd < 0) return 0;
    return flock(s->lock_fd, LOCK_EX) == 0;
}
static void lock_release(ConfigStore *s) {
    if (s->lock_fd >= 0) flock(s->lock_fd, LOCK_UN);
}

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    return buf;
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static bool fsync_dir(const char *dir) {
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

/* ---------------------------------------------------------------------- */
/* restore records                                                         */
/* ---------------------------------------------------------------------- */

static void record_restore(ConfigStore *s, RestoreKind kind,
                           const char *detail, const char *quarantine) {
    if (s->event_count < MAX_RESTORE_EVENTS) {
        RestoreEvent *e = &s->events[s->event_count++];
        e->kind = kind;
        e->ts_ms = now_ms(s);
        snprintf(e->detail, sizeof(e->detail), "%s", detail ? detail : "");
        snprintf(e->quarantine_path, sizeof(e->quarantine_path),
                 "%s", quarantine ? quarantine : "");
    }
    char *rp = join_path(s, RESTORE_NAME);
    if (rp) {
        FILE *f = fopen(rp, "ab");
        if (f) {
            const char *kind_names[] = {
                "boot-fresh", "tmp-discarded", "state-corrupt-quarantined",
                "recovered-from-backup", "fallback-defaults",
                "journal-replayed", "journal-truncated",
                "schema-migrated", "newer-schema-preserved"};
            const char *kn = kind >= 0
                             && kind < (int)(sizeof(kind_names)
                                             / sizeof(kind_names[0]))
                             ? kind_names[kind] : "unknown";
            fprintf(f, "{\"at\":%lld,\"kind\":\"%s\",\"detail\":",
                    now_ms(s), kn);
            /* JSON-safe detail */
            fputc('"', f);
            if (detail) {
                for (const char *p = detail; *p; ++p) {
                    if (*p == '"' || *p == '\\') fputc('\\', f);
                    if (*p == '\n') { fputs("\\n", f); continue; }
                    fputc(*p, f);
                }
            }
            fprintf(f, "\"");
            if (quarantine && *quarantine)
                fprintf(f, ",\"quarantine\":\"%s\"", quarantine);
            fputs("}\n", f);
            fclose(f);
        }
        free(rp);
    }
}

/* Copy a corrupt file into diag/, never deleting the original on the copy
 * path itself (caller decides removal). Returns the quarantine path string
 * in `out` (capacity out_size). */
static bool quarantine_copy(ConfigStore *s, const char *abs_src,
                            const char *why, char *out, size_t out_size) {
    char diag[1100];
    snprintf(diag, sizeof(diag), "%s/%s", s->dir, DIAG_DIR);
    mkdir_p(diag);

    const char *base = strrchr(abs_src, '/');
    base = base ? base + 1 : abs_src;
    snprintf(out, out_size, "%s/%s/quarantine-%lld-%s",
             s->dir, DIAG_DIR, now_ms(s), base);

    FILE *in = fopen(abs_src, "rb");
    if (!in) {
        snprintf(out, out_size, "%s/%s/missing-%lld-%s",
                 s->dir, DIAG_DIR, now_ms(s), base);
        record_restore(s, RESTORE_STATE_CORRUPT_QUARANTINED, why, out);
        return false;
    }
    FILE *qf = fopen(out, "wb");
    if (!qf) { fclose(in); return false; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, qf);
    fclose(in);
    fclose(qf);
    record_restore(s, RESTORE_STATE_CORRUPT_QUARANTINED, why, out);
    return true;
}

/* ---------------------------------------------------------------------- */
/* default document                                                        */
/* ---------------------------------------------------------------------- */

static JsonNode *default_document(const char *device_id) {
    const char *json =
        "{"
        "\"schema_version\":3,"
        "\"device_id\":null,"
        "\"user_id\":null,"
        "\"group_id\":null,"
        "\"data\":{"
        "\"device\":{\"window\":{\"x\":-1,\"y\":-1,\"width\":1280,"
        "\"height\":720,\"maximized\":false}},"
        "\"profile\":{\"theme\":{\"background\":"
        "{\"uri\":\"assets/background.png\",\"mode\":\"fill\"},"
        "\"accent\":\"#2b6cb0\",\"dark\":false}},"
        "\"session\":{\"ui\":{\"density\":\"normal\",\"language\":\"en\"}}"
        "},"
        "\"stamps\":{},"
        "\"baselines\":{\"profile\":0,\"session\":0},"
        "\"dirty\":[],"
        "\"unknown_leaves\":{},"
        "\"hlc\":1"
        "}";
    JsonNode *root = json_parse(json);
    if (root && device_id) {
        json_object_set(root, "device_id", json_string(device_id));
    }
    return root;
}

/* ---------------------------------------------------------------------- */
/* journal replay                                                          */
/* ---------------------------------------------------------------------- */

static void stamp_leaf(ConfigStore *s, const char *path, const char *scope,
                       const JsonNode *event) {
    JsonNode *stamps = json_object_get(s->root, "stamps");
    if (!stamps) {
        stamps = json_object_new();
        json_object_set(s->root, "stamps", stamps);
    }
    const JsonNode *stamp = json_object_get(event, "stamp");
    const JsonNode *author = json_object_get(event, "author");
    const JsonNode *source = json_object_get(event, "source");
    JsonNode *meta = json_object_new();
    json_object_set(meta, "stamp",
                    stamp ? json_clone(stamp) : json_number(0));
    json_object_set(meta, "author",
                    author ? json_clone(author)
                           : json_string(s->device_id ? s->device_id
                                                      : "local"));
    json_object_set(meta, "source",
                    source ? json_clone(source) : json_string("user"));
    json_object_set(stamps, path, meta);
    (void)scope;
}

static void mark_dirty(ConfigStore *s, const char *path) {
    JsonNode *dirty = json_object_get(s->root, "dirty");
    if (!dirty || dirty->type != JSON_ARRAY) {
        dirty = json_array_new();
        json_object_set(s->root, "dirty", dirty);
    }
    for (size_t i = 0; i < dirty->u.array.count; ++i) {
        JsonNode *item = dirty->u.array.items[i];
        if (item->type == JSON_STRING && strcmp(item->u.string, path) == 0)
            return;
    }
    json_array_append(dirty, json_string(path));
}

static void apply_event_to_root(ConfigStore *s, const JsonNode *event) {
    const JsonNode *jscope = json_object_get(event, "scope");
    const JsonNode *jpath = json_object_get(event, "path");
    const JsonNode *jvalue = json_object_get(event, "value");
    const JsonNode *jstamped = json_object_get(event, "stamped");
    if (!jscope || jscope->type != JSON_STRING
        || !jpath || jpath->type != JSON_STRING || !jvalue) {
        return;
    }
    JsonNode *data = json_object_get(s->root, "data");
    JsonNode *seg = json_object_get(data, jscope->u.string);
    if (!seg) return;
    /* build "segment.path" key for json_path_set rooted at the section */
    char full[600];
    snprintf(full, sizeof(full), "%s", jpath->u.string);
    json_path_set(seg, full, json_clone(jvalue));
    if (jstamped && jstamped->type == JSON_BOOL && jstamped->u.boolean) {
        stamp_leaf(s, jpath->u.string, jscope->u.string, event);
        mark_dirty(s, jpath->u.string);
    }
}

static void replay_journal(ConfigStore *s) {
    char *jp = join_path(s, JOURNAL_NAME);
    if (!jp) return;
    FILE *f = fopen(jp, "rb");
    free(jp);
    if (!f) return;

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int replayed = 0;
    bool corrupt = false;
    long corrupt_at = -1;
    long line_no = 0;
    while ((len = getline(&line, &cap, f)) != -1) {
        line_no++;
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;
        if (line[0] == '#') continue;   /* checkpoint marker */
        JsonNode *event = json_parse(line);
        if (!event) { corrupt = true; corrupt_at = line_no; break; }
        apply_event_to_root(s, event);
        json_free(event);
        replayed++;
    }
    free(line);
    fclose(f);

    if (replayed > 0) {
        char detail[128];
        snprintf(detail, sizeof(detail), "replayed %d durable mutation(s)",
                 replayed);
        record_restore(s, RESTORE_JOURNAL_REPLAYED, detail, NULL);
    }
    if (corrupt) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "journal corrupt at line %ld; tail moved to diag",
                 corrupt_at);
        record_restore(s, RESTORE_JOURNAL_TRUNCATED, detail, NULL);
        /* The active journal is conservatively checkpointed: committed state
         * is already in state.json, and the bad tail must not block startup. */
        char *jp2 = join_path(s, JOURNAL_NAME);
        if (jp2) {
            char diagdir[1100];
            snprintf(diagdir, sizeof(diagdir), "%s/%s", s->dir, DIAG_DIR);
            mkdir_p(diagdir);
            char diag[1200];
            snprintf(diag, sizeof(diag), "%s/journal-tail-%lld.jsonl",
                     diagdir, now_ms(s));
            rename(jp2, diag);
            FILE *nf = fopen(jp2, "wb");
            if (nf) { fprintf(nf, "# checkpoint %lld\n", now_ms(s)); fclose(nf); }
            free(jp2);
        }
    }
}

/* ---------------------------------------------------------------------- */
/* durable commit                                                          */
static bool fault_dies_now(ConfigStore *s, const char *stage) {
    if (!s->hooks.fail_stage || !stage) return false;
    if (strcmp(s->hooks.fail_stage, stage) != 0) return false;
    if (s->hooks.fail_on_call <= 0) return false;
    return s->commit_calls == s->hooks.fail_on_call;
}

/* ---------------------------------------------------------------------- */

static bool write_atomic(ConfigStore *s, const char *payload,
                         size_t payload_len) {
    char *tmp = join_path(s, TMP_NAME);
    char *state = join_path(s, STATE_NAME);
    char *backup = join_path(s, BACKUP_NAME);
    bool ok = false;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) goto out;
    if (fault_dies_now(s, "tmp")) {
        /* Power loss with the temp file half-written: never rename it. */
        size_t half = payload_len / 2;
        (void)!write(fd, payload, half);
        fsync(fd);
        close(fd);
        goto out;
    }
    size_t written = 0;
    while (written < payload_len) {
        ssize_t w = write(fd, payload + written, payload_len - written);
        if (w <= 0) { close(fd); goto out; }
        written += (size_t)w;
    }
    if (fsync(fd) != 0) { close(fd); goto out; }
    close(fd);

    if (file_exists(state)) {
        FILE *a = fopen(state, "rb"), *b = fopen(backup, "wb");
        if (a && b) {
            char buf[4096]; size_t n;
            while ((n = fread(buf, 1, sizeof(buf), a)) > 0)
                fwrite(buf, 1, n, b);
        }
        if (a) fclose(a);
        if (b) fclose(b);
    }

    if (fault_dies_now(s, "rename"))
        goto out;  /* tmp fully durable, rename never happened */

    if (rename(tmp, state) != 0) goto out;
    fsync_dir(s->dir);
    ok = true;
out:
    free(tmp); free(state); free(backup);
    return ok;
}

static bool append_journal(ConfigStore *s, const char *line) {
    char *jp = join_path(s, JOURNAL_NAME);
    if (!jp) return false;
    int fd = open(jp, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) { free(jp); return false; }
    size_t n = strlen(line);
    if (fault_dies_now(s, "journal")) {
        (void)!write(fd, line, n / 2);   /* torn append */
        fsync(fd);
        close(fd);
        free(jp);
        return false;                   /* simulate sudden power loss */
    }
    size_t written = 0;
    while (written < n) {
        ssize_t w = write(fd, line + written, n - written);
        if (w <= 0) { close(fd); free(jp); return false; }
        written += (size_t)w;
    }
    fsync(fd);
    close(fd);
    free(jp);
    return true;
}

static long long tick_hlc(ConfigStore *s) {
    s->hlc += 1;
    /* json_object_set frees the previous value; do not free it ourselves. */
    json_object_set(s->root, "hlc", json_number((double)s->hlc));
    return s->hlc;
}

static void apply_in_memory(ConfigStore *s, const char *scope,
                            const char *path, JsonNode *value, bool synced,
                            const char *source, long long stamp) {
    JsonNode *data = json_object_get(s->root, "data");
    JsonNode *seg = json_object_get(data, scope);
    json_path_set(seg, path, json_clone(value));
    if (synced) {
        JsonNode *stamps = json_object_get(s->root, "stamps");
        JsonNode *meta = json_object_new();
        json_object_set(meta, "stamp", json_number((double)stamp));
        json_object_set(meta, "author",
                        json_string(s->device_id ? s->device_id : "local"));
        json_object_set(meta, "source",
                        json_string(source ? source : "user"));
        json_object_set(stamps, path, meta);
        mark_dirty(s, path);
    }
}

static bool commit_mutation(ConfigStore *s, const char *scope,
                            const char *path, JsonNode *value, bool synced,
                            const char *source) {
    s->commit_calls++;
    bool locked = lock_acquire(s);
    long long stamp = synced ? tick_hlc(s) : 0;
    /* Apply in-memory first so the durable snapshot contains the mutation. */
    apply_in_memory(s, scope, path, value, synced, source, stamp);

    /* 1. WAL append + fsync */
    size_t vlen = 0;
    char *vjson = json_emit(value, &vlen);
    if (!vjson) return false;
    const char *author = s->device_id ? s->device_id : "local";
    size_t cap = 256 + strlen(path) + strlen(scope) + vlen + strlen(author)
                 + (source ? strlen(source) : 0);
    char *line = malloc(cap);
    if (!line) { free(vjson); return false; }
    if (synced) {
        snprintf(line, cap,
            "{\"type\":\"mut\",\"seq\":%lld,\"scope\":\"%s\",\"path\":\"%s\","
            "\"value\":%s,\"stamped\":true,\"stamp\":%lld,\"author\":\"%s\","
            "\"source\":\"%s\"}\n",
            now_ms(s), scope, path, vjson, stamp, author,
            source ? source : "user");
    } else {
        snprintf(line, cap,
            "{\"type\":\"mut\",\"seq\":%lld,\"scope\":\"%s\",\"path\":\"%s\","
            "\"value\":%s,\"stamped\":false}\n",
            now_ms(s), scope, path, vjson);
    }
    free(vjson);
    if (!append_journal(s, line)) { free(line); return false; }
    free(line);

    /* 2. state.tmp + fsync + rename */
    size_t plen = 0;
    char *payload = json_emit_pretty(s->root, &plen);
    if (!payload) return false;
    bool ok = write_atomic(s, payload, plen);
    free(payload);

    /* 3. checkpoint marker */
    if (ok) {
        char *jp = join_path(s, JOURNAL_NAME);
        if (jp) {
            FILE *f = fopen(jp, "ab");
            if (f) {
                fprintf(f, "# checkpoint %lld\n", now_ms(s));
                fflush(f);
                fclose(f);
            }
            free(jp);
        }
    }
    if (locked) lock_release(s);
    return ok;
}

/* ---------------------------------------------------------------------- */
/* open / load / recover                                                   */
/* ---------------------------------------------------------------------- */

static JsonNode *load_valid_json_file(const char *path) {
    size_t len = 0;
    char *text = read_file(path, &len);
    if (!text) return NULL;
    JsonNode *node = json_parse(text);
    free(text);
    if (!node || node->type != JSON_OBJECT) {
        if (node) json_free(node);
        return NULL;
    }
    if (!json_object_get(node, "data")
        || !json_object_get(node, "stamps")
        || !json_object_get(node, "baselines")) {
        json_free(node);
        return NULL;
    }
    return node;
}

static void mark_synced_leaves_fallback(ConfigStore *s) {
    JsonNode *stamps = json_object_get(s->root, "stamps");
    for (int i = 0; i < kFieldRegistryCount; ++i) {
        const FieldSpec *spec = &kFieldRegistry[i];
        if (!spec->synced) continue;
        JsonNode *meta = json_object_new();
        json_object_set(meta, "stamp", json_number(0));
        json_object_set(meta, "author", json_string("local"));
        json_object_set(meta, "source", json_string("fallback"));
        json_object_set(stamps, spec->path, meta);
    }
}

ConfigStore *config_open(const char *dir, const char *device_id,
                         const ConfigHooks *hooks) {
    ConfigStore *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->dir = strdup(dir);
    s->device_id = device_id ? strdup(device_id) : NULL;
    if (hooks) s->hooks = *hooks;
    char *tmp = NULL;
    char *state = NULL;
    char *backup = NULL;
    if (mkdir_p(s->dir) != 0) goto fail;
    char diag[1100];
    snprintf(diag, sizeof(diag), "%s/%s", s->dir, DIAG_DIR);
    mkdir_p(diag);

    tmp = join_path(s, TMP_NAME);
    state = join_path(s, STATE_NAME);
    backup = join_path(s, BACKUP_NAME);
    char qpath[1200];

    /* A leftover tmp is an interrupted rewrite: never adopt it. */
    if (file_exists(tmp)) {
        quarantine_copy(s, tmp, "discarded tmp from interrupted write",
                        qpath, sizeof(qpath));
        record_restore(s, RESTORE_TMP_DISCARDED,
                       "state.json.tmp discarded on startup", qpath);
        unlink(tmp);
    }

    JsonNode *root = NULL;
    bool state_existed = file_exists(state);
    bool backup_existed = file_exists(backup);
    if (state_existed) {
        root = load_valid_json_file(state);
        if (!root) {
            quarantine_copy(s, state, "state.json unparseable",
                            qpath, sizeof(qpath));
            unlink(state);
        }
    }

    if (!root && file_exists(backup)) {
        root = load_valid_json_file(backup);
        if (root) {
            record_restore(s, RESTORE_RECOVERED_FROM_BACKUP,
                           "loaded state.backup.json", NULL);
        } else {
            quarantine_copy(s, backup, "backup unparseable",
                            qpath, sizeof(qpath));
            unlink(backup);
        }
    }

    bool first_boot = false;
    if (!root) {
        first_boot = !state_existed && !backup_existed;
        root = default_document(device_id);
        if (!root) goto fail;
        if (first_boot) {
            record_restore(s, RESTORE_BOOT_FRESH, "initial defaults", NULL);
        } else {
            s->used_fallback = true;
            mark_synced_leaves_fallback(s);
            record_restore(s, RESTORE_FALLBACK_DEFAULTS,
                "fallback defaults only; tagged source=fallback and not "
                "broadcast until the user edits", qpath);
        }
    } else if (device_id) {
        JsonNode *id = json_object_get(root, "device_id");
        if (!id || id->type != JSON_STRING)
            json_object_set(root, "device_id", json_string(device_id));
    }

    /* Schema migration / forward compatibility. */
    long long schema = 0;
    const JsonNode *sv = json_object_get(root, "schema_version");
    if (sv && sv->type == JSON_NUMBER) schema = (long long)sv->u.number;
    if (schema > CONFIG_KNOWN_SCHEMA) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "schema v%lld newer than known v%d; kept verbatim",
                 schema, CONFIG_KNOWN_SCHEMA);
        record_restore(s, RESTORE_NEWER_SCHEMA_PRESERVED, detail, NULL);
    } else if (schema < CONFIG_KNOWN_SCHEMA) {
        JsonNode *migrated = config_migrate(root, CONFIG_KNOWN_SCHEMA);
        if (migrated) {
            char detail[128];
            snprintf(detail, sizeof(detail), "migrated v%lld -> v%d",
                     schema, CONFIG_KNOWN_SCHEMA);
            record_restore(s, RESTORE_SCHEMA_MIGRATED, detail, NULL);
            json_free(root);
            root = migrated;
        }
    }

    s->root = root;
    const JsonNode *hlc = json_object_get(root, "hlc");
    if (hlc && hlc->type == JSON_NUMBER)
        s->hlc = (long long)hlc->u.number;

    replay_journal(s);

    /* Persist any recovery/migration result for subsequent boots. */
    size_t plen = 0;
    char *payload = json_emit_pretty(s->root, &plen);
    if (payload) {
        write_atomic(s, payload, plen);
        free(payload);
    }

    free(tmp); free(state); free(backup);
    lock_release(s);
    return s;

fail:
    lock_release(s);
    free(tmp); free(state); free(backup);
    free(s->dir); free(s->device_id);
    free(s);
    return NULL;
}

void config_close(ConfigStore *store) {
    if (!store) return;
    json_free(store->root);
    free(store->dir);
    free(store->device_id);
    free(store);
}

/* ---------------------------------------------------------------------- */
/* getters / setters                                                       */
/* ---------------------------------------------------------------------- */

const JsonNode *config_get(ConfigStore *store, const char *path) {
    const FieldSpec *spec = field_lookup(path);
    JsonNode *data = json_object_get(store->root, "data");
    if (spec) {
        JsonNode *seg = json_object_get(data,
            spec->scope == SCOPE_DEVICE ? "device"
            : spec->scope == SCOPE_PROFILE ? "profile" : "session");
        return seg ? json_path_get(seg, path) : NULL;
    }
    /* Unknown path: search profile then session, preserve semantics. */
    JsonNode *prof = json_object_get(data, "profile");
    const JsonNode *n = prof ? json_path_get(prof, path) : NULL;
    if (n) return n;
    JsonNode *sess = json_object_get(data, "session");
    return sess ? json_path_get(sess, path) : NULL;
}

bool config_get_int(ConfigStore *store, const char *path, long long fallback,
                    long long *out) {
    const JsonNode *n = config_get(store, path);
    if (n && n->type == JSON_NUMBER) { *out = (long long)n->u.number; return true; }
    *out = fallback;
    return false;
}

bool config_get_string(ConfigStore *store, const char *path,
                       const char **out) {
    const JsonNode *n = config_get(store, path);
    if (n && n->type == JSON_STRING) { *out = n->u.string; return true; }
    return false;
}

bool config_get_bool(ConfigStore *store, const char *path, bool fallback) {
    const JsonNode *n = config_get(store, path);
    if (n && n->type == JSON_BOOL) return n->u.boolean;
    return fallback;
}

static bool set_value(ConfigStore *store, const char *path, JsonNode *value,
                      bool synced, const char *source) {
    const FieldSpec *spec = field_lookup(path);
    FieldScope scope = spec ? spec->scope : SCOPE_UNKNOWN;
    if (synced) {
        if (scope != SCOPE_PROFILE && scope != SCOPE_SESSION) {
            json_free(value); return false;
        }
    } else {
        if (scope != SCOPE_DEVICE) { json_free(value); return false; }
    }
    const char *seg = scope == SCOPE_DEVICE ? "device"
                      : scope == SCOPE_PROFILE ? "profile" : "session";
    return commit_mutation(store, seg, path, value, synced, source);
}

bool config_set_local(ConfigStore *store, const char *path, JsonNode *value) {
    return set_value(store, path, value, false, "user");
}
bool config_set_local_int(ConfigStore *store, const char *path, long long v) {
    return config_set_local(store, path, json_number((double)v));
}
bool config_set_local_string(ConfigStore *store, const char *path,
                             const char *v) {
    return config_set_local(store, path, json_string(v));
}
bool config_set_synced(ConfigStore *store, const char *path, JsonNode *value) {
    return set_value(store, path, value, true, "user");
}
bool config_set_synced_int(ConfigStore *store, const char *path, long long v) {
    return config_set_synced(store, path, json_number((double)v));
}
bool config_set_synced_string(ConfigStore *store, const char *path,
                              const char *v) {
    return config_set_synced(store, path, json_string(v));
}

int config_restore_events(ConfigStore *store, const RestoreEvent **out) {
    *out = store->events;
    return store->event_count;
}

const JsonNode *config_root(ConfigStore *store) { return store->root; }

bool config_used_fallback(ConfigStore *store) { return store->used_fallback; }

long long config_base_version(ConfigStore *store, const char *segment) {
    const JsonNode *b = json_object_get(store->root, "baselines");
    const JsonNode *v = b ? json_object_get(b, segment) : NULL;
    return v && v->type == JSON_NUMBER ? (long long)v->u.number : 0;
}

bool config_apply_segment(ConfigStore *store, const char *segment,
                          const JsonNode *doc, long long base_version) {
    JsonNode *data = json_object_get(store->root, "data");
    JsonNode *current = json_object_get(data, segment);
    /* Merge leaf-by-leaf onto the existing tree. Device section never passed,
     * but defensively skip any device-prefixed dotted key in a profile doc. */
    JsonNode *replacement = json_clone(doc);
    if (!replacement) return false;
    /* json_object_set frees the previous value; the server-opaque unknown bag
     * lives in its own "unknown_leaves" slot and is therefore untouched. */
    (void)current;
    json_object_set(data, segment, replacement);

    JsonNode *baselines = json_object_get(store->root, "baselines");
    json_object_set(baselines, segment, json_number((double)base_version));

    /* Clear dirty flags for synced leaves in this segment; rejected edits are
     * re-added by the caller when the server reports them. */
    JsonNode *dirty = json_object_get(store->root, "dirty");
    if (dirty && dirty->type == JSON_ARRAY) {
        JsonNode *kept = json_array_new();
        for (size_t i = 0; i < dirty->u.array.count; ++i) {
            JsonNode *item = dirty->u.array.items[i];
            bool in_segment = false;
            if (item->type == JSON_STRING) {
                FieldScope sc = field_scope(item->u.string);
                in_segment = (strcmp(segment, "profile") == 0
                              && sc == SCOPE_PROFILE)
                             || (strcmp(segment, "session") == 0
                                 && sc == SCOPE_SESSION);
            }
            if (in_segment) {
                json_free(item);
            } else {
                json_array_append(kept, item);
            }
        }
        json_free(dirty);
        json_object_set(store->root, "dirty", kept);
    }

    size_t plen = 0;
    char *payload = json_emit_pretty(store->root, &plen);
    bool ok = payload && write_atomic(store, payload, plen);
    free(payload);
    return ok;
}

JsonNode *config_pending_changes(ConfigStore *store) {
    JsonNode *out = json_object_new();
    const JsonNode *dirty = json_object_get(store->root, "dirty");
    const JsonNode *stamps = json_object_get(store->root, "stamps");
    if (!dirty || dirty->type != JSON_ARRAY) return out;
    for (size_t i = 0; i < dirty->u.array.count; ++i) {
        JsonNode *item = dirty->u.array.items[i];
        if (item->type != JSON_STRING) continue;
        const char *path = item->u.string;
        if (!field_is_synced(path)) continue;   /* device leaves never sync */
        const JsonNode *value = config_get(store, path);
        if (!value) continue;
        const JsonNode *meta = json_object_get(stamps, path);
        const JsonNode *src = meta ? json_object_get(meta, "source") : NULL;
        if (src && src->type == JSON_STRING
            && strcmp(src->u.string, "fallback") == 0) {
            continue;   /* auto fallback must not be broadcast */
        }
        JsonNode *entry = json_object_new();
        json_object_set(entry, "value", json_clone(value));
        json_object_set(entry, "stamp",
                        meta && json_object_get(meta, "stamp")
                            ? json_clone(json_object_get(meta, "stamp"))
                            : json_number(0));
        json_object_set(entry, "author",
                        meta && json_object_get(meta, "author")
                            ? json_clone(json_object_get(meta, "author"))
                            : json_string(store->device_id
                                          ? store->device_id : "local"));
        json_object_set(out, path, entry);
    }
    return out;
}
