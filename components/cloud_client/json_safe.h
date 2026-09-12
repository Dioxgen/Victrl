#pragma once

/*
 * Defensive cJSON accessors.
 *
 * Everything the LLM returns is untrusted input: any field may be absent, may
 * have the wrong type, or may be null. The stock cJSON helpers return NULL in
 * all of those cases and every caller in this project used to dereference the
 * result immediately, so a single missing field in the model's JSON crashed
 * the firmware.
 *
 * These accessors never return NULL for strings and never dereference a
 * missing item — they fall back to the caller-supplied default instead.
 *
 * Lookups stay case-insensitive (cJSON_GetObjectItem) on purpose: models
 * frequently reshape keys to "Action_Type" / "Box_2D", and tolerating that has
 * been the existing behaviour.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

/* String field. Returns `fallback` when absent, null, or not a string. */
static inline const char *jget_str(const cJSON *obj, const char *key,
                                   const char *fallback)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }
    return fallback;
}

/* Numeric field. Accepts numbers and numeric strings ("1.5"). */
static inline double jget_num(const cJSON *obj, const char *key, double fallback)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (!item) return fallback;
    if (cJSON_IsNumber(item)) return item->valuedouble;
    if (cJSON_IsString(item) && item->valuestring) {
        char *end = NULL;
        double v = strtod(item->valuestring, &end);
        if (end && end != item->valuestring) return v;
    }
    return fallback;
}

static inline int jget_int(const cJSON *obj, const char *key, int fallback)
{
    double v = jget_num(obj, key, (double)fallback);
    return (int)v;
}

/* Boolean field. Tolerates true/false, 0/1 and "true"/"false". */
static inline bool jget_bool(const cJSON *obj, const char *key, bool fallback)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (!item) return fallback;
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    if (cJSON_IsNumber(item)) return item->valueint != 0;
    if (cJSON_IsString(item) && item->valuestring) {
        const char *s = item->valuestring;
        if (strcmp(s, "true") == 0 || strcmp(s, "1") == 0) return true;
        if (strcmp(s, "false") == 0 || strcmp(s, "0") == 0) return false;
    }
    return fallback;
}

/* True when the key exists as an array of at least `min_items` entries. */
static inline bool jget_array_ok(const cJSON *obj, const char *key, int min_items)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    return item && cJSON_IsArray(item) && cJSON_GetArraySize(item) >= min_items;
}
