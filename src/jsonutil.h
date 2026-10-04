#ifndef JSONUTIL_H
#define JSONUTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Minimal recursive-descent JSON parser/emitter.
 *
 * Critical for forward compatibility: every object member is preserved,
 * including members unknown to the running schema version. An old client
 * parsing a newer document can re-emit it verbatim (minus formatting) and
 * never drops fields it does not understand.
 */

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} JsonType;

typedef struct JsonNode JsonNode;

struct JsonNode {
    JsonType type;
    union {
        bool boolean;
        double number;
        char *string;          /* null-terminated, owned                 */
        struct {
            JsonNode **items;
            size_t count;
        } array;
        struct {
            char **keys;       /* member order is preserved               */
            JsonNode **values;
            size_t count;
        } object;
    } u;
};

typedef struct {
    const char *cur;
    const char *end;
    char error[160];
} JsonParser;

JsonNode *json_parse(const char *text);
JsonNode *json_parse_len(const char *text, size_t len);
char *json_emit(const JsonNode *node, size_t *out_len);
char *json_emit_pretty(const JsonNode *node, size_t *out_len);
void json_free(JsonNode *node);

JsonNode *json_null(void);
JsonNode *json_bool(bool value);
JsonNode *json_number(double value);
JsonNode *json_string(const char *value);
JsonNode *json_array_new(void);
JsonNode *json_object_new(void);

bool json_array_append(JsonNode *arr, JsonNode *item);
bool json_object_set(JsonNode *obj, const char *key, JsonNode *value);
JsonNode *json_object_get(const JsonNode *obj, const char *key);
bool json_object_has(const JsonNode *obj, const char *key);
bool json_object_delete(JsonNode *obj, const char *key);

/* Dotted-path accessors ("a.b.c"); missing nodes return NULL / false. */
JsonNode *json_path_get(const JsonNode *root, const char *dotted);
bool json_path_set(JsonNode *root, const char *dotted, JsonNode *value);

/* Deep copy (used when migrating so the old document stays available). */
JsonNode *json_clone(const JsonNode *node);

/* Strict comparison (type+value, object order independent). */
bool json_equal(const JsonNode *a, const JsonNode *b);

#endif /* JSONUTIL_H */
