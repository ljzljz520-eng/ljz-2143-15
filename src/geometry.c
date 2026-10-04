#define _POSIX_C_SOURCE 200809L
#include "geometry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int rect_intersection_area(Rect w, const Monitor *m) {
    int x1 = w.x > m->x ? w.x : m->x;
    int y1 = w.y > m->y ? w.y : m->y;
    int x2 = (w.x + w.w < m->x + m->w) ? (w.x + w.w) : (m->x + m->w);
    int y2 = (w.y + w.h < m->y + m->h) ? (w.y + w.h) : (m->y + m->h);
    int iw = x2 - x1;
    int ih = y2 - y1;
    return (iw > 0 && ih > 0) ? iw * ih : 0;
}

static const Monitor *primary_monitor(const Monitor *monitors, int count) {
    for (int i = 0; i < count; ++i) {
        if (monitors[i].primary) return &monitors[i];
    }
    return &monitors[0];
}

static Rect centered_on(const Monitor *m, Rect win) {
    if (win.w > m->w) win.w = m->w;
    if (win.h > m->h) win.h = m->h;
    if (win.w < GEOM_MIN_W) win.w = GEOM_MIN_W;
    if (win.h < GEOM_MIN_H) win.h = GEOM_MIN_H;
    win.x = m->x + (m->w - win.w) / 2;
    win.y = m->y + (m->h - win.h) / 2;
    return win;
}

static Rect clamp_to(const Monitor *m, Rect win) {
    if (win.w > m->w) win.w = m->w;
    if (win.h > m->h) win.h = m->h;
    if (win.w < GEOM_MIN_W) win.w = GEOM_MIN_W;
    if (win.h < GEOM_MIN_H) win.h = GEOM_MIN_H;
    if (win.x < m->x) win.x = m->x;
    if (win.y < m->y) win.y = m->y;
    if (win.x + win.w > m->x + m->w) win.x = m->x + m->w - win.w;
    if (win.y + win.h > m->y + m->h) win.y = m->y + m->h - win.h;
    return win;
}

static int monitor_cmp(const void *a, const void *b) {
    const char *sa = (const char *)a;
    const char *sb = (const char *)b;
    return strcmp(sa, sb);
}

void geometry_fingerprint(const Monitor *monitors, int count,
                          char *out, int out_size) {
    if (out_size <= 0) return;
    out[0] = '\0';
    char parts[64][48];
    int n = 0;
    for (int i = 0; i < count && n < 64; ++i) {
        snprintf(parts[n], sizeof(parts[n]), "%dx%d@%d,%d",
                 monitors[i].w, monitors[i].h, monitors[i].x, monitors[i].y);
        n++;
    }
    qsort(parts, (size_t)n, sizeof(parts[0]), monitor_cmp);
    int used = 0;
    for (int i = 0; i < n; ++i) {
        int written = snprintf(out + used,
                               (size_t)(out_size - used),
                               "%s%s", i ? "|" : "", parts[i]);
        if (written < 0 || written >= out_size - used) break;
        used += written;
    }
}

GeomResult geometry_repair(Rect win, const Monitor *monitors, int count) {
    GeomResult res;
    memset(&res, 0, sizeof(res));
    res.rect = win;

    if (count <= 0 || monitors == NULL) {
        res.reason = GEOM_NO_MONITOR;
        if (res.rect.w < GEOM_MIN_W) res.rect.w = GEOM_MIN_W;
        if (res.rect.h < GEOM_MIN_H) res.rect.h = GEOM_MIN_H;
        res.rect.x = 0;
        res.rect.y = 0;
        return res;
    }

    const Monitor *prim = primary_monitor(monitors, count);

    if (win.x < 0 || win.y < 0) {
        res.rect = centered_on(prim, win);
        res.reason = GEOM_CENTERED_DEFAULT;
        snprintf(res.monitor_id, sizeof(res.monitor_id), "%s", prim->id);
        return res;
    }

    const Monitor *best = NULL;
    int best_area = 0;
    for (int i = 0; i < count; ++i) {
        int area = rect_intersection_area(win, &monitors[i]);
        if (area > best_area) {
            best_area = area;
            best = &monitors[i];
        }
    }

    if (best != NULL && best_area > 0) {
        Rect clamped = clamp_to(best, win);
        bool changed = clamped.x != win.x || clamped.y != win.y
                       || clamped.w != win.w || clamped.h != win.h;
        res.rect = clamped;
        res.reason = changed ? GEOM_CLAMPED : GEOM_UNCHANGED;
        snprintf(res.monitor_id, sizeof(res.monitor_id), "%s", best->id);
        return res;
    }

    res.rect = centered_on(prim, win);
    res.reason = GEOM_MOVED_TO_PRIMARY;
    snprintf(res.monitor_id, sizeof(res.monitor_id), "%s", prim->id);
    return res;
}

bool geometry_is_operable(Rect win, const Monitor *monitors, int count) {
    if (win.w < GEOM_MIN_W || win.h < GEOM_MIN_H) return false;
    for (int i = 0; i < count; ++i) {
        const Monitor *m = &monitors[i];
        if (win.x >= m->x && win.y >= m->y
            && win.x + win.w <= m->x + m->w
            && win.y + win.h <= m->y + m->h) {
            return true;
        }
    }
    return false;
}
