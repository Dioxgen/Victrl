#include "storage_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"

static const char *TAG = "plan";

static char s_plan_dir[64] = "/sdcard/plans";
static cJSON *s_current_plan = NULL;
static char s_task_id[32] = {0};

esp_err_t plan_mgr_init(const char *plan_dir)
{
    if (plan_dir) strncpy(s_plan_dir, plan_dir, sizeof(s_plan_dir) - 1);
    ESP_LOGI(TAG, "Init: dir=%s", s_plan_dir);
    return ESP_OK;
}

/* Build a fresh plan object with a single starter milestone. */
static void plan_reset(const char *id, const char *task_goal)
{
    if (s_current_plan) cJSON_Delete(s_current_plan);

    snprintf(s_task_id, sizeof(s_task_id), "%s", id ? id : "");

    s_current_plan = cJSON_CreateObject();
    cJSON_AddStringToObject(s_current_plan, "task_id", s_task_id);
    cJSON_AddStringToObject(s_current_plan, "task_goal", task_goal);
    cJSON_AddStringToObject(s_current_plan, "created", s_task_id);

    cJSON *milestones = cJSON_AddArrayToObject(s_current_plan, "milestones");
    cJSON *m1 = cJSON_CreateObject();
    cJSON_AddNumberToObject(m1, "id", 1);
    cJSON_AddStringToObject(m1, "description", "Analyze the task and plan approach");
    cJSON_AddStringToObject(m1, "status", "pending");
    cJSON_AddItemToArray(milestones, m1);
}

esp_err_t plan_mgr_new_task(const char *task_goal, char task_id_out[32])
{
    /* Generate task ID from timestamp.
     * strftime, not snprintf("%04d%02d..."): the tm fields are plain ints whose
     * range the compiler cannot bound, so the manual form is rejected under
     * -Werror=format-truncation. */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char id[32] = {0};
    if (strftime(id, sizeof(id), "%Y%m%d_%H%M%S", tm_info) == 0) {
        snprintf(id, sizeof(id), "%s", "session");
    }

    plan_reset(id, task_goal);
    if (task_id_out) strcpy(task_id_out, s_task_id);
    ESP_LOGI(TAG, "New task: %s -> %s", s_task_id, task_goal);
    return ESP_OK;
}

esp_err_t plan_mgr_new_task_with_id(const char *id, const char *task_goal)
{
    if (!id || !id[0]) return ESP_ERR_INVALID_ARG;
    plan_reset(id, task_goal);
    ESP_LOGI(TAG, "New task: %s -> %s", s_task_id, task_goal);
    return ESP_OK;
}

esp_err_t plan_mgr_load(void)
{
    if (s_current_plan) cJSON_Delete(s_current_plan);
    s_current_plan = NULL;
    s_task_id[0] = '\0';

    char path[160];
    snprintf(path, sizeof(path), "%s/plan.json", s_plan_dir);

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGI(TAG, "No plan yet at %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return ESP_ERR_NOT_FOUND; }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    s_current_plan = cJSON_Parse(buf);
    free(buf);
    if (!s_current_plan) {
        ESP_LOGW(TAG, "Plan file is not valid JSON: %s", path);
        return ESP_FAIL;
    }

    const cJSON *tid = cJSON_GetObjectItem(s_current_plan, "task_id");
    if (cJSON_IsString(tid) && tid->valuestring) {
        snprintf(s_task_id, sizeof(s_task_id), "%s", tid->valuestring);
    }
    ESP_LOGI(TAG, "Plan loaded: %s", path);
    return ESP_OK;
}

/*
 * Remove every copy of `key` from the current plan.
 *
 * cJSON's Add*ToObject APPENDS — it does not replace an existing key — so adding
 * a key that is already present silently creates a DUPLICATE. This plan is
 * serialised and re-sent to the model on every step, so `current_milestone` was
 * accumulating one extra copy per step (three copies by step 4, twenty by step
 * 20) purely as growing junk in every request.
 *
 * DeleteItemFromObject removes only the FIRST match, hence the loop.
 */
static void plan_drop_key(const char *key)
{
    if (!s_current_plan || !key) return;
    while (cJSON_GetObjectItem(s_current_plan, key)) {
        cJSON_DeleteItemFromObject(s_current_plan, key);
    }
}

esp_err_t plan_mgr_save(cJSON *plan_update)
{
    if (!plan_update) return ESP_ERR_INVALID_ARG;

    if (s_current_plan) {
        /*
         * Keep ONE copy of the milestone list.
         *
         * This used to store the whole update object under "plan_update" AND
         * merge its milestones to the top level, so plan.json held the same
         * array twice — and plan.json is re-sent to the API on every step, so
         * the duplicate cost tokens on every request. The top level is the
         * canonical copy (the WebUI and the LCD read `milestones` there); the
         * nested object only needs what has no top-level home, i.e. `summary`
         * (the WebUI renders it) and `current_milestone` (the LCD reads it).
         */
        plan_drop_key("plan_update");

        cJSON *stored = cJSON_CreateObject();
        if (stored) {
            const cJSON *summary = cJSON_GetObjectItem(plan_update, "summary");
            if (cJSON_IsString(summary) && summary->valuestring) {
                cJSON_AddStringToObject(stored, "summary", summary->valuestring);
            }

            const cJSON *new_ms = cJSON_GetObjectItem(plan_update, "milestones");
            const cJSON *cur = cJSON_GetObjectItem(plan_update, "current_milestone");

            /* current_milestone is tracked independently of the array, because
             * the model may omit `milestones` on a step where the plan itself
             * did not change — and the pointer should still advance. */
            if (cJSON_IsNumber(cur)) {
                cJSON_AddNumberToObject(stored, "current_milestone", cur->valueint);
                plan_drop_key("current_milestone");
                cJSON_AddNumberToObject(s_current_plan, "current_milestone",
                                        cur->valueint);
            }

            /* A missing `milestones` means "the plan is unchanged", not "delete
             * the plan": the model only needs to re-send the array when a
             * milestone starts, finishes or is reworded. That array is ~20% of
             * a step's output tokens, so omitting it when it is unchanged is a
             * direct latency saving. */
            if (new_ms && cJSON_IsArray(new_ms)) {
                plan_drop_key("milestones");
                cJSON_AddItemToObject(s_current_plan, "milestones",
                                      cJSON_Duplicate(new_ms, 1));
            }

            /* Do not litter the plan with an empty object when the model sent
             * neither field. */
            if (cJSON_GetArraySize(stored) > 0) {
                cJSON_AddItemToObject(s_current_plan, "plan_update", stored);
            } else {
                cJSON_Delete(stored);
            }
        }
    }

    if (!s_task_id[0]) return ESP_OK;

    /* The plan lives inside the session directory now, so the filename is
     * fixed: the directory already identifies the session. */
    char path[160];
    snprintf(path, sizeof(path), "%s/plan.json", s_plan_dir);

    /* Unformatted: this text is echoed to the model on every step, and
     * indentation is pure token waste. */
    char *json_str = cJSON_PrintUnformatted(s_current_plan);
    if (!json_str) return ESP_ERR_NO_MEM;

    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(json_str, 1, strlen(json_str), f);
        fclose(f);
        ESP_LOGI(TAG, "Plan saved: %s", path);
    } else {
        ESP_LOGW(TAG, "Failed to save plan to %s", path);
    }
    free(json_str);
    return ESP_OK;
}

cJSON *plan_mgr_get_current(void)
{
    if (s_current_plan) {
        return cJSON_Duplicate(s_current_plan, 1);
    }
    return NULL;
}
