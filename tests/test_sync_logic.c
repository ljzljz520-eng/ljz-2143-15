/* Pure-logic C tests: no SDL required. Covers
 *  - JSON round trip + unknown-field preservation
 *  - verifiable migrations + forward compatibility
 *  - display unplug / dual->single geometry repair
 *  - half-written state recovery (injected power loss at every stage)
 *  - corrupt state quarantine + fallback leaves not broadcast
 *  - journal replay after crash
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "geometry.h"
#include "jsonutil.h"
#include "migrate.h"

static int failures;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
                   failures++; } } while (0)

static long long clock_seq;
static long long seq_clock(void *u) { (void)u; return ++clock_seq; }

static void rmrf(const char *path) {
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    int rc = system(cmd);
    (void)rc;
}

/* ---------------------------------------------------------------------- */
static void test_json_unknown_preserved(void) {
    const char *doc =
        "{\"schema_version\":9,\"profile\":{\"theme\":{"
        "\"accent\":\"#abc\",\"futuristic\":{\"glow\":true,"
        "\"particles\":42}}}}";
    JsonNode *n = json_parse(doc);
    CHECK(n != NULL);
    const JsonNode *future = json_path_get(n,
        "profile.theme.futuristic.glow");
    CHECK(future && future->type == JSON_BOOL && future->u.boolean);

    /* An old client edits a known field, then saves. Unknown must survive. */
    json_path_set(n, "profile.theme.accent", json_string("#000"));
    size_t len = 0;
    char *emitted = json_emit(n, &len);
    CHECK(emitted != NULL);
    JsonNode *reparsed = json_parse(emitted);
    CHECK(json_path_get(reparsed,
        "profile.theme.futuristic.particles")->u.number == 42);
    CHECK(strcmp(json_path_get(reparsed,
        "profile.theme.accent")->u.string, "#000") == 0);
    free(emitted);
    json_free(reparsed);
    json_free(n);
}

static void test_migrations(void) {
    CHECK(config_migrate_self_test() == 1);
}

static void test_geometry(void) {
    Monitor dual[] = {
        {"eDP", 0, 0, 1920, 1080, true},
        {"HDMI", 1920, 0, 1920, 1080, false},
    };
    char fp1[GEOM_FP_MAX], fp2[GEOM_FP_MAX];
    geometry_fingerprint(dual, 2, fp1, sizeof(fp1));
    CHECK(strstr(fp1, "1920x1080@0,0") != NULL);
    CHECK(strstr(fp1, "1920x1080@1920,0") != NULL);

    /* Window placed on second screen at (2100,60) size 1600x900 */
    Rect w = {2100, 60, 1600, 900};
    GeomResult r = geometry_repair(w, dual, 2);
    CHECK(geometry_is_operable(r.rect, dual, 2));
    CHECK(strcmp(r.monitor_id, "HDMI") == 0);
    CHECK(r.rect.x >= 1920);

    /* Unplug HDMI: only eDP remains. */
    Monitor single[] = {{"eDP", 0, 0, 1366, 768, true}};
    geometry_fingerprint(single, 1, fp2, sizeof(fp2));
    CHECK(strcmp(fp1, fp2) != 0);
    r = geometry_repair(w, single, 1);
    CHECK(geometry_is_operable(r.rect, single, 1));
    CHECK(r.rect.x >= 0 && r.rect.x < 1366);
    CHECK(r.rect.x + r.rect.w <= 1366);
    CHECK(r.rect.w >= GEOM_MIN_W && r.rect.h >= GEOM_MIN_H);
    CHECK(r.reason == GEOM_MOVED_TO_PRIMARY);

    /* Coordinates from the dual machine are NOT acceptable on the single:
       they are machine-specific and must be repaired, never synced. */
    Rect offscreen = {2100, 60, 1600, 900};
    CHECK(!geometry_is_operable(offscreen, single, 1));

    /* Default sentinel positions center on primary */
    Rect centered = {-1, -1, 1280, 720};
    r = geometry_repair(centered, single, 1);
    CHECK(geometry_is_operable(r.rect, single, 1));
    CHECK(r.reason == GEOM_CENTERED_DEFAULT);
}

/* ---------------------------------------------------------------------- */
static void test_power_loss_stages(void) {
    const char *dir = "/tmp/vw-c-test-power";
    const char *stages[] = {"journal", "tmp", "rename"};
    for (int stage = 0; stage < 3; ++stage) {
        rmrf(dir);
        ConfigHooks hooks = {.now_ms = seq_clock,
                             .fail_stage = stages[stage],
                             .fail_on_call = 2};
        ConfigStore *s = config_open(dir, "dev1", &hooks);
        CHECK(s != NULL);
        /* first move survives, second mutation may "crash" */
        config_set_local_int(s, "window.x", 100);
        config_set_local_string(s, "window.monitor", "HDMI");
        config_close(s);

        /* Reopen WITHOUT fault injection: recover exactly as after power cut */
        ConfigHooks ok_hooks = {.now_ms = seq_clock};
        s = config_open(dir, "dev1", &ok_hooks);
        CHECK(s != NULL);
        long long x = -999;
        config_get_int(s, "window.x", -1, &x);
        CHECK(x == 100);          /* first durable move present */
        const char *mon = NULL;
        config_get_string(s, "window.monitor", &mon);
        /* monitor may or may not be present depending on crash stage, but the
           store must be consistent and the window must be repairable */
        (void)mon;
        long long ww = 0, wh = 0, wx = 0, wy = 0;
        config_get_int(s, "window.width", 1280, &ww);
        config_get_int(s, "window.height", 720, &wh);
        config_get_int(s, "window.x", -1, &wx);
        config_get_int(s, "window.y", -1, &wy);
        Monitor local[] = {{"eDP", 0, 0, 1920, 1080, true}};
        Rect win = {(int)wx, (int)wy, (int)ww, (int)wh};
        GeomResult gr = geometry_repair(win, local, 1);
        CHECK(geometry_is_operable(gr.rect, local, 1));
        config_close(s);
    }
}

static void test_corrupt_state_quarantine(void) {
    const char *dir = "/tmp/vw-c-test-corrupt";
    rmrf(dir);
    mkdir(dir, 0700);
    mkdir("/tmp/vw-c-test-corrupt/diag", 0700);
    FILE *f = fopen("/tmp/vw-c-test-corrupt/state.json", "wb");
    fputs("{ this is not valid json ,,,", f);
    fclose(f);

    ConfigHooks hooks = {.now_ms = seq_clock};
    ConfigStore *s = config_open(dir, "dev1", &hooks);
    CHECK(s != NULL);
    const RestoreEvent *events = NULL;
    int n = config_restore_events(s, &events);
    bool quarantined = false, fallback = false;
    for (int i = 0; i < n; ++i) {
        if (events[i].kind == RESTORE_STATE_CORRUPT_QUARANTINED) {
            quarantined = true;
            struct stat st;
            CHECK(stat(events[i].quarantine_path, &st) == 0);
        }
        if (events[i].kind == RESTORE_FALLBACK_DEFAULTS) fallback = true;
    }
    CHECK(quarantined);
    CHECK(fallback);
    CHECK(config_used_fallback(s));

    /* Fallback values must not be broadcast as pending sync changes. */
    JsonNode *pending = config_pending_changes(s);
    CHECK(pending->u.object.count == 0);
    json_free(pending);

    /* A real user edit now marks dirty and is broadcast once. */
    config_set_synced_string(s, "theme.accent", "#123456");
    pending = config_pending_changes(s);
    CHECK(json_object_has(pending, "theme.accent"));
    json_free(pending);
    config_close(s);

    /* Diagnostic copy and original retained; restore.jsonl exists. */
    struct stat st;
    CHECK(stat("/tmp/vw-c-test-corrupt/restore.jsonl", &st) == 0);
    int copies = system("ls /tmp/vw-c-test-corrupt/diag/quarantine-* >/dev/null 2>&1");
    CHECK(copies == 0);
}

static void test_dirty_and_device_isolation(void) {
    const char *dir = "/tmp/vw-c-test-isolation";
    rmrf(dir);
    ConfigHooks hooks = {.now_ms = seq_clock};
    ConfigStore *s = config_open(dir, "dev1", &hooks);
    config_set_local_int(s, "window.x", 3000);   /* dual-screen coord */
    config_set_local_int(s, "window.width", 1700);
    config_set_synced_string(s, "theme.background.uri", "bg.png");
    JsonNode *pending = config_pending_changes(s);
    CHECK(json_object_has(pending, "theme.background.uri"));
    CHECK(!json_object_has(pending, "window.x"));
    CHECK(!json_object_has(pending, "window.width"));
    /* Device coord never appears in the serialised sync payload. */
    size_t len = 0;
    char *text = json_emit(pending, &len);
    CHECK(strstr(text, "3000") == NULL);
    free(text);
    json_free(pending);
    config_close(s);
}

static void test_journal_replay(void) {
    const char *dir = "/tmp/vw-c-test-journal";
    rmrf(dir);
    ConfigHooks hooks = {.now_ms = seq_clock};
    ConfigStore *s = config_open(dir, "dev1", &hooks);
    config_set_synced_string(s, "ui.language", "zh-CN");
    config_set_synced_string(s, "ui.density", "compact");
    config_close(s);

    /* Delete state.json but keep journal+backup path exercised: the store
       must still recover; simulate state corruption + valid journal. */
    remove("/tmp/vw-c-test-journal/state.json");
    remove("/tmp/vw-c-test-journal/state.backup.json");
    ConfigHooks hooks2 = {.now_ms = seq_clock};
    s = config_open(dir, "dev1", &hooks2);
    const char *lang = NULL;
    CHECK(config_get_string(s, "ui.language", &lang) && strcmp(lang, "zh-CN") == 0);
    config_close(s);
}

int main(void) {
    test_json_unknown_preserved();
    test_migrations();
    test_geometry();
    test_power_loss_stages();
    test_corrupt_state_quarantine();
    test_dirty_and_device_isolation();
    test_journal_replay();
    if (failures) {
        printf("\n%d C TEST CHECK(S) FAILED\n", failures);
        return 1;
    }
    printf("ALL C TESTS PASSED\n");
    return 0;
}
