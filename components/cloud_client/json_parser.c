#include "json_parser.h"

#include <string.h>
#include "json_safe.h"
#include "esp_log.h"

static const char *TAG = "json_parser";

static char *strip_markdown_fences(char *text)
{
    if (!text) return NULL;

    char *start = text;
    while (*start == ' ' || *start == '\n' || *start == '\r') {
        start++;
    }

    if (strncmp(start, "```", 3) == 0) {
        start += 3;
        char *newline = strchr(start, '\n');
        if (newline) start = newline + 1;
    }

    size_t len = strlen(start);
    while (len > 0 && (start[len - 1] == '\n' || start[len - 1] == '\r' || start[len - 1] == ' ')) {
        len--;
    }

    if (len >= 3 && strncmp(start + len - 3, "```", 3) == 0) {
        len -= 3;
    }

    start[len] = '\0';
    return start;
}

cJSON *json_parser_parse(const char *raw_text)
{
    if (!raw_text || !*raw_text) {
        ESP_LOGW(TAG, "Empty input text");
        return NULL;
    }

    size_t len = strlen(raw_text) + 1;
    char *work = (char *)malloc(len);
    if (!work) return NULL;
    strcpy(work, raw_text);

    char *clean = strip_markdown_fences(work);
    cJSON *data = cJSON_Parse(clean);

    if (!data) {
        const char *err = cJSON_GetErrorPtr();
        ESP_LOGW(TAG, "Initial parse failed at: %.40s", err ? err : "?");

        char *start = strchr(clean, '{');
        char *end = clean ? strrchr(clean, '}') : NULL;
        if (start && end && end > start) {
            *(end + 1) = '\0';
            data = cJSON_Parse(start);
        }
    }

    free(work);

    if (!data || !cJSON_IsObject(data)) {
        ESP_LOGW(TAG, "Failed to parse JSON object from response");
        if (data) cJSON_Delete(data);
        return NULL;
    }

    if (cJSON_HasObjectItem(data, "action_type") &&
        !cJSON_HasObjectItem(data, "actions")) {

        const char *error_type = jget_str(data, "action_type", NULL);
        if (error_type && strcmp(error_type, "error") == 0) {
            if (!cJSON_HasObjectItem(data, "plan_update")) {
                ESP_LOGW(TAG, "Error response missing plan_update");
                cJSON_Delete(data);
                return NULL;
            }
            cJSON_AddArrayToObject(data, "actions");
            cJSON_AddTrueToObject(data, "done");
            /* Must still fill in need_screen / sleep_before_next /
             * request_profile / profile_updates: the caller dereferences
             * them unconditionally. Returning early here without defaults
             * used to crash the agent loop. */
            json_parser_set_response_defaults(data);
            return data;
        }

        const char *atype = jget_str(data, "action_type", NULL);
        if (!atype || !atype[0]) {
            ESP_LOGW(TAG, "Response has empty action_type");
            cJSON_Delete(data);
            return NULL;
        }

        cJSON *actions = cJSON_AddArrayToObject(data, "actions");
        cJSON *action = cJSON_CreateObject();
        cJSON_AddStringToObject(action, "action_type", atype);

        /* Boxes are copied verbatim — the executor validates them. */
        static const char *const boxes[] = {"box_2d", "from_box", "to_box"};
        for (size_t b = 0; b < sizeof(boxes) / sizeof(boxes[0]); b++) {
            cJSON *v = cJSON_GetObjectItem(data, boxes[b]);
            if (v) cJSON_AddItemToObject(action, boxes[b], cJSON_Duplicate(v, 1));
        }

        /* Scalars go through the typed accessors: cJSON_HasObjectItem() only
         * says the key exists, not that it is a string, so reading
         * ->valuestring off an array/number here handed a garbage pointer to
         * strdup(). */
        const char *s;
        if ((s = jget_str(data, "button", NULL)))
            cJSON_AddStringToObject(action, "button", s);
        if ((s = jget_str(data, "key", NULL)))
            cJSON_AddStringToObject(action, "key", s);
        if ((s = jget_str(data, "text", NULL)))
            cJSON_AddStringToObject(action, "text", s);
        if (cJSON_GetObjectItem(data, "delta_x"))
            cJSON_AddNumberToObject(action, "delta_x", jget_num(data, "delta_x", 0));
        if (cJSON_GetObjectItem(data, "delta_y"))
            cJSON_AddNumberToObject(action, "delta_y", jget_num(data, "delta_y", 0));
        if (cJSON_GetObjectItem(data, "wait_seconds"))
            cJSON_AddNumberToObject(action, "wait_seconds",
                                    jget_num(data, "wait_seconds", 0.0));
        if (cJSON_GetObjectItem(data, "hold"))
            cJSON_AddNumberToObject(action, "hold", jget_num(data, "hold", 0));

        json_parser_set_action_defaults(action);
        cJSON_AddItemToArray(actions, action);

        cJSON_DeleteItemFromObject(data, "action_type");
        cJSON_DeleteItemFromObject(data, "box_2d");
        cJSON_DeleteItemFromObject(data, "from_box");
        cJSON_DeleteItemFromObject(data, "to_box");
        cJSON_DeleteItemFromObject(data, "button");
        cJSON_DeleteItemFromObject(data, "key");
        cJSON_DeleteItemFromObject(data, "text");
        cJSON_DeleteItemFromObject(data, "delta_x");
        cJSON_DeleteItemFromObject(data, "delta_y");
        cJSON_DeleteItemFromObject(data, "wait_seconds");
        cJSON_DeleteItemFromObject(data, "hold");
    }

    cJSON *actions = cJSON_GetObjectItem(data, "actions");
    if (!actions || !cJSON_IsArray(actions)) {
        ESP_LOGW(TAG, "No valid actions array in response");
        cJSON_Delete(data);
        return NULL;
    }

    /* Validate in place. Malformed entries must be REMOVED, not merely
     * skipped: the action executor and the need_screen scan both iterate this
     * array afterwards and dereference action_type unconditionally, so leaving
     * a bad entry in place crashes the agent loop. */
    for (int i = 0; i < cJSON_GetArraySize(actions); ) {
        cJSON *action = cJSON_GetArrayItem(actions, i);
        cJSON *atype_item = action ? cJSON_GetObjectItem(action, "action_type") : NULL;

        if (!cJSON_IsObject(action) || !cJSON_IsString(atype_item) ||
            !atype_item->valuestring || !atype_item->valuestring[0]) {
            ESP_LOGW(TAG, "Dropping malformed action at index %d", i);
            cJSON_DeleteItemFromArray(actions, i);
            continue; /* array shifted left — re-examine this index */
        }

        /* Normalise so the executor's strcmp() matches whatever capitalisation
         * the model chose ("Click" / "CLICK" / "click"). */
        for (char *p = atype_item->valuestring; *p; p++) {
            if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
        }

        json_parser_set_action_defaults(action);
        i++;
    }

    if (!cJSON_HasObjectItem(data, "plan_update")) {
        ESP_LOGW(TAG, "Response missing required field: plan_update");
        cJSON_Delete(data);
        return NULL;
    }

    json_parser_set_response_defaults(data);
    return data;
}

void json_parser_set_action_defaults(cJSON *action)
{
    if (!cJSON_HasObjectItem(action, "delay_after"))
        cJSON_AddNumberToObject(action, "delay_after", 0.05);
    if (!cJSON_HasObjectItem(action, "button"))
        cJSON_AddStringToObject(action, "button", "left");
    if (!cJSON_HasObjectItem(action, "hold"))
        cJSON_AddNumberToObject(action, "hold", 0);
    if (!cJSON_HasObjectItem(action, "wait_seconds"))
        cJSON_AddNumberToObject(action, "wait_seconds", 0.0);
}

void json_parser_set_response_defaults(cJSON *response)
{
    if (!cJSON_HasObjectItem(response, "need_screen"))
        cJSON_AddTrueToObject(response, "need_screen");
    if (!cJSON_HasObjectItem(response, "sleep_before_next"))
        cJSON_AddNumberToObject(response, "sleep_before_next", 0.0);
    if (!cJSON_HasObjectItem(response, "done"))
        cJSON_AddFalseToObject(response, "done");
    if (!cJSON_HasObjectItem(response, "request_profile"))
        cJSON_AddStringToObject(response, "request_profile", "");
    if (!cJSON_HasObjectItem(response, "profile_updates"))
        cJSON_AddArrayToObject(response, "profile_updates");
}
