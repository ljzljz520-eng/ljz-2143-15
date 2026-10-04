#include "migrate.h"

#include <stdio.h>
#include <string.h>

/*
 * v1 -> v2: device.window.w/h become width/height; profile.bg + profile.dark
 *           move under theme.background.uri / theme.
 * v2 -> v3: theme.accent, theme.background.mode, session.ui.* are added.
 *
 * Every step is an ordered pure transform with a verifier and an inverse on
 * the canonical sample. Unknown members survive because transforms operate on
 * specific keys and never rebuild the whole tree from a schema template.
 */

static JsonNode *v1_to_v2(JsonNode *r) {
    JsonNode *dev = json_object_get(r, "device");
    JsonNode *win = dev ? json_object_get(dev, "window") : NULL;
    if (win) {
        JsonNode *w = json_object_get(win, "w");
        JsonNode *h = json_object_get(win, "h");
        if (w && !json_object_has(win, "width"))
            json_object_set(win, "width", json_clone(w));
        if (h && !json_object_has(win, "height"))
            json_object_set(win, "height", json_clone(h));
        json_object_delete(win, "w");
        json_object_delete(win, "h");
    }
    JsonNode *prof = json_object_get(r, "profile");
    if (prof) {
        JsonNode *theme = json_object_get(prof, "theme");
        if (!theme) { theme = json_object_new(); json_object_set(prof, "theme", theme); }
        JsonNode *bg = json_object_get(theme, "background");
        if (!bg) { bg = json_object_new(); json_object_set(theme, "background", bg); }
        JsonNode *old_bg = json_object_get(prof, "bg");
        if (old_bg && !json_object_has(bg, "uri"))
            json_object_set(bg, "uri", json_clone(old_bg));
        if (!json_object_has(bg, "mode"))
            json_object_set(bg, "mode", json_string("fill"));
        JsonNode *old_dark = json_object_get(prof, "dark");
        if (old_dark && !json_object_has(theme, "dark"))
            json_object_set(theme, "dark", json_clone(old_dark));
        json_object_delete(prof, "bg");
        json_object_delete(prof, "dark");
    }
    json_object_set(r, "schema_version", json_number(2));
    return r;
}

static JsonNode *v2_to_v3(JsonNode *r) {
    JsonNode *prof = json_object_get(r, "profile");
    if (prof) {
        JsonNode *theme = json_object_get(prof, "theme");
        if (!theme) { theme = json_object_new(); json_object_set(prof, "theme", theme); }
        if (!json_object_has(theme, "accent"))
            json_object_set(theme, "accent", json_string("#2b6cb0"));
        JsonNode *bg = json_object_get(theme, "background");
        if (!bg) { bg = json_object_new(); json_object_set(theme, "background", bg); }
        if (!json_object_has(bg, "mode"))
            json_object_set(bg, "mode", json_string("fill"));
    }
    JsonNode *sess = json_object_get(r, "session");
    if (!sess) { sess = json_object_new(); json_object_set(r, "session", sess); }
    JsonNode *ui = json_object_get(sess, "ui");
    if (!ui) { ui = json_object_new(); json_object_set(sess, "ui", ui); }
    if (!json_object_has(ui, "density"))
        json_object_set(ui, "density", json_string("normal"));
    if (!json_object_has(ui, "language"))
        json_object_set(ui, "language", json_string("en"));
    json_object_set(r, "schema_version", json_number(3));
    return r;
}

/* Inverses operate on the canonical sample (used by the verifiable test). */
static JsonNode *v2_back(JsonNode *r) {
    JsonNode *win = json_path_get(r, "device.window");
    if (win) {
        JsonNode *width = json_object_get(win, "width");
        JsonNode *height = json_object_get(win, "height");
        if (width) json_object_set(win, "w", json_clone(width));
        if (height) json_object_set(win, "h", json_clone(height));
        json_object_delete(win, "width");
        json_object_delete(win, "height");
    }
    JsonNode *prof = json_object_get(r, "profile");
    if (prof) {
        JsonNode *theme = json_object_get(prof, "theme");
        if (theme) {
            JsonNode *bg = json_object_get(theme, "background");
            if (bg) {
                JsonNode *uri = json_object_get(bg, "uri");
                if (uri) json_object_set(prof, "bg", json_clone(uri));
            }
            JsonNode *dark = json_object_get(theme, "dark");
            if (dark) json_object_set(prof, "dark", json_clone(dark));
            json_object_delete(prof, "theme");
        }
    }
    json_object_set(r, "schema_version", json_number(1));
    return r;
}

static JsonNode *v3_back(JsonNode *r) {
    /* Inverse of v2->v3: only remove fields that v2->v3 introduced.
     * background.mode already existed in v2 (added by v1->v2), so it stays. */
    JsonNode *theme = json_path_get(r, "profile.theme");
    if (theme) json_object_delete(theme, "accent");
    JsonNode *sess = json_object_get(r, "session");
    if (sess) json_object_delete(sess, "ui");
    json_object_set(r, "schema_version", json_number(2));
    return r;
}

static int check_v2(const JsonNode *r) {
    const JsonNode *win = json_path_get(r, "device.window");
    if (!win || json_object_has(win, "w") || json_object_has(win, "h")) return 0;
    if (!json_object_has(win, "width") || !json_object_has(win, "height")) return 0;
    const JsonNode *uri = json_path_get(r, "profile.theme.background.uri");
    if (!uri) return 0;
    if (json_path_get(r, "profile.bg")) return 0;
    return 1;
}

static int check_v3(const JsonNode *r) {
    if (!json_path_get(r, "profile.theme.accent")) return 0;
    const JsonNode *d = json_path_get(r, "session.ui.density");
    if (!d || d->type != JSON_STRING) return 0;
    return 1;
}

JsonNode *config_migrate(const JsonNode *root, int target_schema) {
    if (!root || root->type != JSON_OBJECT) return NULL;
    const JsonNode *sv = json_object_get(root, "schema_version");
    int version = sv && sv->type == JSON_NUMBER ? (int)sv->u.number : 1;
    JsonNode *out = json_clone(root);
    if (!out) return NULL;
    if (version > target_schema) {
        /* Newer than this client: do not downgrade and do not rewrite. */
        return out;
    }
    if (version < 2 && target_schema >= 2) out = v1_to_v2(out);
    if (json_object_get(out, "schema_version")->u.number < 3
        && target_schema >= 3) out = v2_to_v3(out);
    return out;
}

static const char *kCanonicalV1 =
    "{\"schema_version\":1,"
    "\"device\":{\"window\":{\"x\":-1,\"y\":-1,\"w\":1280,\"h\":720,"
    "\"maximized\":false}},"
    "\"profile\":{\"bg\":\"assets/background.png\",\"dark\":false},"
    "\"session\":{}}";

int config_migrate_self_test(void) {
    JsonNode *v1 = json_parse(kCanonicalV1);
    if (!v1) return 0;

    JsonNode *v2 = config_migrate(v1, 2);
    if (!v2 || !check_v2(v2)) { json_free(v1); json_free(v2); return 0; }
    /* idempotent step */
    JsonNode *v2_again = v1_to_v2(json_clone(v2));
    if (!json_equal(v2, v2_again)) {
        json_free(v1); json_free(v2); json_free(v2_again); return 0;
    }
    json_free(v2_again);
    /* inverse round trip */
    JsonNode *back1 = v2_back(json_clone(v2));
    if (!json_equal(back1, v1)) {
        json_free(v1); json_free(v2); json_free(back1); return 0;
    }
    json_free(back1);

    JsonNode *v3 = config_migrate(v2, 3);
    if (!v3 || !check_v3(v3)) { json_free(v1); json_free(v2); json_free(v3); return 0; }
    JsonNode *v3_again = v2_to_v3(json_clone(v3));
    if (!json_equal(v3, v3_again)) {
        json_free(v1); json_free(v2); json_free(v3); json_free(v3_again); return 0;
    }
    json_free(v3_again);
    JsonNode *back2 = v3_back(json_clone(v3));
    if (!json_equal(back2, v2)) {
        json_free(v1); json_free(v2); json_free(v3); json_free(back2); return 0;
    }

    /* newer document is preserved untouched */
    const char *future =
        "{\"schema_version\":9,\"profile\":{\"theme\":{\"futuristic\":true}}}";
    JsonNode *f = json_parse(future);
    JsonNode *fkept = config_migrate(f, 3);
    if (!fkept || !json_equal(fkept, f)) {
        json_free(f); json_free(fkept);
        json_free(v1); json_free(v2); json_free(v3); json_free(back2);
        return 0;
    }

    json_free(f); json_free(fkept);
    json_free(v1); json_free(v2); json_free(v3); json_free(back2);
    return 1;
}
