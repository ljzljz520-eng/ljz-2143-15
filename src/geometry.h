#ifndef GEOMETRY_H
#define GEOMETRY_H

#include <stdbool.h>

#define GEOM_MIN_W 320
#define GEOM_MIN_H 240
#define GEOM_FP_MAX 1024

typedef struct {
    const char *id;
    int x, y, w, h;
    bool primary;
} Monitor;

typedef struct {
    int x, y, w, h;
} Rect;

typedef enum {
    GEOM_UNCHANGED,
    GEOM_CENTERED_DEFAULT,
    GEOM_CLAMPED,
    GEOM_MOVED_TO_PRIMARY,
    GEOM_NO_MONITOR,
} GeomReason;

typedef struct {
    Rect rect;
    GeomReason reason;
    char monitor_id[64];
} GeomResult;

/* Stable display-topology fingerprint. The string length never exceeds
 * GEOM_FP_MAX-1. Device-specific: it never leaves the machine. */
void geometry_fingerprint(const Monitor *monitors, int count,
                          char *out, int out_size);

/* Repair a window against the *local* monitor topology so that it is always
 * visible and operable (>=320x240, fully inside some monitor, title bar
 * reachable). This is what prevents a dual-monitor machine's coordinates
 * being forced onto a single-monitor machine. */
GeomResult geometry_repair(Rect win, const Monitor *monitors, int count);

bool geometry_is_operable(Rect win, const Monitor *monitors, int count);

#endif /* GEOMETRY_H */
