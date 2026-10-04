#include "config.h"

#include <stdio.h>
#include <string.h>

const CfgDefault CFG_DEFAULTS[] = {
    {"machine.window.x", "100", 2},
    {"machine.window.y", "100", 2},
    {"machine.window.width", "1024", 2},
    {"machine.window.height", "768", 2},
    {"machine.window.monitor", "\"primary\"", 2},
    {"prefs.theme", "\"light\"", 2},
    {"prefs.background", "\"assets/background.png\"", 2},
    {"prefs.opacity", "1.0", 3},
    {"prefs.font_size", "13", 2},
};
const int CFG_DEFAULTS_LEN = (int)(sizeof(CFG_DEFAULTS) / sizeof(CFG_DEFAULTS[0]));

int cfg_owner_is_device(const char *field) {
    return strncmp(field, "machine.", 8) == 0;
}

JVal *cfg_default_doc_for(int schema) {
    if (schema < 2) schema = 2;
    if (schema > CFG_SCHEMA_VERSION) schema = CFG_SCHEMA_VERSION;
    JVal *doc = j_new(J_OBJ);
    for (int i = 0; i < CFG_DEFAULTS_LEN; i++) {
        if (CFG_DEFAULTS[i].since > schema) continue;
        const char *err = NULL;
        JVal *v = j_parse(CFG_DEFAULTS[i].json_value, &err);
        if (v) j_set_path(doc, CFG_DEFAULTS[i].field, v);
    }
    j_obj_set(doc, "schema_version", j_num(schema));
    return doc;
}

JVal *cfg_default_doc(void) {
    return cfg_default_doc_for(CFG_SCHEMA_VERSION);
}

/* ---------------- 校验 ---------------- */

static int has_num(const JVal *doc, const char *path) {
    JVal *v = j_get_path((JVal *)doc, path);
    return v && v->type == J_NUM;
}

static int has_str(const JVal *doc, const char *path) {
    JVal *v = j_get_path((JVal *)doc, path);
    return v && v->type == J_STR;
}

int cfg_validate_doc(const JVal *doc, int version) {
    if (!doc || doc->type != J_OBJ) return 0;
    switch (version) {
    case 1: {
        JVal *win = j_obj_get((JVal *)doc, "win");
        if (!win || win->type != J_OBJ) return 0;
        return has_num(doc, "win.x") && has_num(doc, "win.y") &&
               has_num(doc, "win.w") && has_num(doc, "win.h") &&
               has_str(doc, "theme");
    }
    case 2:
        return has_num(doc, "machine.window.x") && has_num(doc, "machine.window.y") &&
               has_num(doc, "machine.window.width") && has_num(doc, "machine.window.height") &&
               has_str(doc, "prefs.theme") && has_str(doc, "prefs.background");
    case 3:
        return cfg_validate_doc(doc, 2) && has_num(doc, "prefs.opacity");
    default:
        return 0;
    }
}

/* ---------------- 迁移 ---------------- */

static void move_key(JVal *doc, const char *from_path, const char *to_path) {
    JVal *v = j_get_path(doc, from_path);
    if (v) j_set_path(doc, to_path, j_copy(v));
}

static int up_1_2(JVal *doc) {
    /* v1 扁平 {theme,bg,win:{x,y,w,h}} -> v2 分层 machine/prefs */
    JVal *win = j_obj_get(doc, "win");
    if (win && win->type == J_OBJ) {
        move_key(doc, "win.x", "machine.window.x");
        move_key(doc, "win.y", "machine.window.y");
        move_key(doc, "win.w", "machine.window.width");
        move_key(doc, "win.h", "machine.window.height");
        j_obj_del(doc, "win");
    }
    JVal *bg = j_obj_get(doc, "bg");
    if (bg) {
        j_set_path(doc, "prefs.background", j_copy(bg));
        j_obj_del(doc, "bg");
    }
    JVal *theme = j_obj_get(doc, "theme");
    if (theme) {
        j_set_path(doc, "prefs.theme", j_copy(theme));
        j_obj_del(doc, "theme");
    }
    j_obj_set(doc, "schema_version", j_num(2));
    return 0;
}

static int up_2_3(JVal *doc) {
    /* v3 引入 prefs.opacity；已有值（未知字段由新版写入）不覆盖 */
    if (!j_get_path(doc, "prefs.opacity"))
        j_set_path(doc, "prefs.opacity", j_num(1.0));
    j_obj_set(doc, "schema_version", j_num(3));
    return 0;
}

int cfg_migrate_doc(JVal *doc, int target, char *err, size_t errlen) {
    if (!doc || doc->type != J_OBJ) {
        snprintf(err, errlen, "doc is not an object");
        return -1;
    }
    JVal *sv = j_obj_get(doc, "schema_version");
    int version = (sv && sv->type == J_NUM) ? (int)sv->num : 1;
    if (version > target) {
        /* 新版文档、旧客户端：原样保留（含未知字段），不降级不抹除 */
        return 0;
    }
    while (version < target) {
        int rc = 0;
        if (version == 1) rc = up_1_2(doc);
        else if (version == 2) rc = up_2_3(doc);
        else {
            snprintf(err, errlen, "no migration from schema %d", version);
            return -1;
        }
        version++;
        if (rc != 0 || !cfg_validate_doc(doc, version)) {
            snprintf(err, errlen, "validation failed after migrating to schema %d", version);
            return -1;
        }
    }
    if (!cfg_validate_doc(doc, version)) {
        snprintf(err, errlen, "validation failed at schema %d", version);
        return -1;
    }
    return 0;
}

/* ---------------- 窗口可见性钳制 ---------------- */

#define MIN_WIN_W 320
#define MIN_WIN_H 200
#define TITLEBAR_MARGIN 20  /* 标题栏至少要有这么多像素露在某块屏幕内 */
#define EDGE_OVERLAP 60     /* 水平方向至少重叠像素数 */

typedef struct { int x, y, w, h; } Rect;

static int rect_contains_point(const Rect *r, int px, int py) {
    return px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h;
}

int cfg_clamp_window(JVal *doc, const JVal *monitors, char *note, size_t notelen) {
    if (!doc) return 0;
    JVal *win = j_get_path(doc, "machine.window");
    if (!win || win->type != J_OBJ) return 0;

    Rect screens[16];
    int nscreens = 0;
    if (monitors && monitors->type == J_ARR) {
        for (int i = 0; i < monitors->len && nscreens < 16; i++) {
            JVal *m = monitors->items[i];
            screens[nscreens].x = (int)j_as_num(j_obj_get(m, "x"), 0);
            screens[nscreens].y = (int)j_as_num(j_obj_get(m, "y"), 0);
            screens[nscreens].w = (int)j_as_num(j_obj_get(m, "w"), 1920);
            screens[nscreens].h = (int)j_as_num(j_obj_get(m, "h"), 1080);
            nscreens++;
        }
    }
    if (nscreens == 0) {
        screens[0] = (Rect){0, 0, 1920, 1080};
        nscreens = 1;
    }

    int x = (int)j_as_num(j_obj_get(win, "x"), 100);
    int y = (int)j_as_num(j_obj_get(win, "y"), 100);
    int w = (int)j_as_num(j_obj_get(win, "width"), 1024);
    int h = (int)j_as_num(j_obj_get(win, "height"), 768);
    int ox = x, oy = y, ow = w, oh = h;

    /* 尺寸钳制：不小于最小可操作尺寸，不超过主屏 */
    if (w < MIN_WIN_W) w = MIN_WIN_W;
    if (h < MIN_WIN_H) h = MIN_WIN_H;
    if (w > screens[0].w) w = screens[0].w;
    if (h > screens[0].h) h = screens[0].h;

    /* 可见性：中心点落在某块屏幕内，且标题栏（顶部条带）可见 */
    int cx = x + w / 2, cy = y + h / 2;
    int visible = 0;
    for (int i = 0; i < nscreens; i++) {
        int center_in = rect_contains_point(&screens[i], cx, cy);
        int title_in = rect_contains_point(&screens[i], x + w / 2, y + TITLEBAR_MARGIN / 2);
        int overlap_x = (x + w > screens[i].x + EDGE_OVERLAP) &&
                        (x < screens[i].x + screens[i].w - EDGE_OVERLAP);
        if ((center_in || title_in) && overlap_x) {
            visible = 1;
            break;
        }
    }
    if (!visible) {
        /* 屏幕拔出 / 坐标来自别的机器：搬回主屏安全位置 */
        x = screens[0].x + 50;
        y = screens[0].y + 50;
    }

    if (x == ox && y == oy && w == ow && h == oh) return 0;

    j_obj_set(win, "x", j_num(x));
    j_obj_set(win, "y", j_num(y));
    j_obj_set(win, "width", j_num(w));
    j_obj_set(win, "height", j_num(h));
    if (note && notelen > 0) {
        snprintf(note, notelen,
                 "window clamped: (%d,%d %dx%d) -> (%d,%d %dx%d), screens=%d",
                 ox, oy, ow, oh, x, y, w, h, nscreens);
    }
    return 1;
}
