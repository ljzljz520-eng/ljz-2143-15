#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "jsonutil.h"

#define CONFIG_KNOWN_SCHEMA 3

typedef enum {
    RESTORE_BOOT_FRESH = 0,
    RESTORE_TMP_DISCARDED,
    RESTORE_STATE_CORRUPT_QUARANTINED,
    RESTORE_RECOVERED_FROM_BACKUP,
    RESTORE_FALLBACK_DEFAULTS,
    RESTORE_JOURNAL_REPLAYED,
    RESTORE_JOURNAL_TRUNCATED,
    RESTORE_SCHEMA_MIGRATED,
    RESTORE_NEWER_SCHEMA_PRESERVED,
} RestoreKind;

typedef struct {
    RestoreKind kind;
    long long ts_ms;
    char detail[256];
    char quarantine_path[512];
} RestoreEvent;

typedef struct ConfigStore ConfigStore;

typedef long long (*clock_fn)(void *user);

typedef struct {
    clock_fn now_ms;
    void *clock_user;
    /* Fault injection for "power cut halfway through a write".
       fail_stage: "journal" | "tmp" | "rename"; fail_on_call is 1-based and
       counts commit_mutation calls on THIS opened store instance. The store
       simulates a sudden process death (no checkpoint) at that point. */
    const char *fail_stage;
    int fail_on_call;
} ConfigHooks;

/* Open (and if necessary create/recover) the durable state under dir.
 * Returns NULL only on unrecoverable allocation failure. */
ConfigStore *config_open(const char *dir, const char *device_id,
                         const ConfigHooks *hooks);
void config_close(ConfigStore *store);

/* Read a leaf by dotted path (borrowed node; NULL when absent). */
const JsonNode *config_get(ConfigStore *store, const char *path);
bool config_get_int(ConfigStore *store, const char *path, long long fallback,
                    long long *out);
bool config_get_string(ConfigStore *store, const char *path,
                       const char **out);
bool config_get_bool(ConfigStore *store, const char *path, bool fallback);

/* Mutations. Device paths are local-only and never marked for sync. */
bool config_set_local(ConfigStore *store, const char *path, JsonNode *value);
bool config_set_local_int(ConfigStore *store, const char *path, long long v);
bool config_set_local_string(ConfigStore *store, const char *path,
                             const char *v);
bool config_set_synced(ConfigStore *store, const char *path, JsonNode *value);
bool config_set_synced_int(ConfigStore *store, const char *path, long long v);
bool config_set_synced_string(ConfigStore *store, const char *path,
                              const char *v);

/* Current restore record (append-only restore.jsonl also written on disk). */
int config_restore_events(ConfigStore *store, const RestoreEvent **out);

/* Access the full document for the agent/CLI (borrowed). */
const JsonNode *config_root(ConfigStore *store);

/* Replace the profile/session sections with a server-provided document.
 * Unknown members in `doc` are preserved because the merge operates on the
 * existing tree; device-scoped members of `doc` are ignored defensively. */
bool config_apply_segment(ConfigStore *store, const char *segment,
                          const JsonNode *doc, long long base_version);

long long config_base_version(ConfigStore *store, const char *segment);

/* Serialise the set of pending synced changes for the agent:
 * output object maps path -> {value, stamp, author}. Device paths excluded. */
JsonNode *config_pending_changes(ConfigStore *store);

/* True after a corrupt state/backup caused fallback defaults; those leaves
 * are tagged source=fallback and must not be broadcast to the group. */
bool config_used_fallback(ConfigStore *store);

#endif /* CONFIG_H */
