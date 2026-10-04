#ifndef CONFIG_REGISTRY_H
#define CONFIG_REGISTRY_H

#include <stdbool.h>

/* Mirror of sync/synckit/schema.py. Device-scoped paths MUST NOT be sent on
 * the wire; the C client asserts this before invoking the agent. */

typedef enum {
    SCOPE_UNKNOWN = 0,
    SCOPE_DEVICE,
    SCOPE_PROFILE,
    SCOPE_SESSION,
} FieldScope;

typedef struct {
    const char *path;
    FieldScope scope;
    bool synced;
} FieldSpec;

static const FieldSpec kFieldRegistry[] = {
    {"window.x",                SCOPE_DEVICE,  false},
    {"window.y",                SCOPE_DEVICE,  false},
    {"window.width",            SCOPE_DEVICE,  false},
    {"window.height",           SCOPE_DEVICE,  false},
    {"window.maximized",        SCOPE_DEVICE,  false},
    {"window.monitor",          SCOPE_DEVICE,  false},
    {"displays.fingerprint",    SCOPE_DEVICE,  false},

    {"theme.background.uri",    SCOPE_PROFILE, true},
    {"theme.background.mode",   SCOPE_PROFILE, true},
    {"theme.accent",            SCOPE_PROFILE, true},
    {"theme.dark",              SCOPE_PROFILE, true},

    {"ui.density",              SCOPE_SESSION, true},
    {"ui.language",             SCOPE_SESSION, true},
};

static const int kFieldRegistryCount =
    (int)(sizeof(kFieldRegistry) / sizeof(kFieldRegistry[0]));

static inline const FieldSpec *field_lookup(const char *path) {
    for (int i = 0; i < kFieldRegistryCount; ++i) {
        const FieldSpec *s = &kFieldRegistry[i];
        const char *a = s->path, *b = path;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') return s;
    }
    return NULL;
}

static inline FieldScope field_scope(const char *path) {
    const FieldSpec *s = field_lookup(path);
    return s ? s->scope : SCOPE_UNKNOWN;
}

static inline bool field_is_synced(const char *path) {
    const FieldSpec *s = field_lookup(path);
    return s && s->synced;
}

/* Unknown paths (newer schema) are preserved opaquely, never rejected. */
static inline bool field_is_unknown(const char *path) {
    return field_lookup(path) == NULL;
}

#endif /* CONFIG_REGISTRY_H */
