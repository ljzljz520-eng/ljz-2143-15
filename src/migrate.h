#ifndef MIGRATE_H
#define MIGRATE_H

#include "jsonutil.h"

/* Ordered, verifiable migrations. Returns a migrated (possibly newly
 * allocated) document, or NULL on a malformed document. Documents newer than
 * target_schema are returned untouched (never downgraded). */
JsonNode *config_migrate(const JsonNode *root, int target_schema);

/* Run all up/down/idempotency checks on the built-in canonical v1 sample. */
int config_migrate_self_test(void);

#endif /* MIGRATE_H */
