#include "storage_manager.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "config";

esp_err_t config_load(const char *path, runtime_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    /* Set defaults.
     * `api_endpoint` is used verbatim as the request URL, so the full path
     * including the /responses suffix must be here — without it a missing
     * config.json posts to the API root and every call 404s. */
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->api_endpoint, "https://api.deepseek.com/responses");
    strcpy(cfg->model_name, "deepseek-flash");
    cfg->timeout = 120;
    cfg->max_retries = 3;
    cfg->max_output_tokens = 16384;
    cfg->enable_thinking = true;
    /* Routine steps get the cheap budget; the agent escalates only while it is
     * objectively stuck. The API call dominates a step's wall clock and
     * reasoning tokens are a third to a half of the output. */
    strcpy(cfg->reasoning_effort, "low");
    strcpy(cfg->reasoning_effort_escalated, "high");
    cfg->effort_escalate_after = 1;
    /* The model downscales anything above ~1300x1300 on its side and bills a
     * flat ceiling of ~1024 tokens per image, so uploading more pixels buys
     * nothing but bandwidth. */
    cfg->upload_max_dim = 1360;
    cfg->save_frames = false;
    cfg->max_actions = 200;
    cfg->history_max_len = 10;
    cfg->system_prompt_interval = 0;   /* 0 = never rotate: keeps the prefix stable */
    cfg->allow_wait_skip = false;      /* enable after validating the fingerprint */
    cfg->max_blind_rounds = 3;
    cfg->wait_settle_ms = 1000;
    /* Human-paced typing: accuracy over speed. A dropped character costs a
     * full round trip, so these are much more conservative than USB allows. */
    cfg->type_hold_ms = 15;
    cfg->type_gap_ms = 35;
    cfg->type_jitter_ms = 25;
    cfg->type_word_pause_ms = 120;
    cfg->run_dialog_ms = 500;
    cfg->run_settle_ms = 150;
    cfg->capture_width = 1920;
    cfg->capture_height = 1080;
    cfg->output_width = 1920;
    cfg->output_height = 1080;
    strcpy(cfg->profile_dir, "/sdcard/profiles");
    strcpy(cfg->plan_dir, "/sdcard/plans");
    strcpy(cfg->session_dir, "/sdcard/sessions");
    strcpy(cfg->default_profile, "win11_laptop");
    strcpy(cfg->log_dir, "/sdcard/log");
    strcpy(cfg->sysprompt_dir, "/sdcard/sysprompt");
    strcpy(cfg->web_dir, "/sdcard/web");
    cfg->http_port = 80;

    if (!path) {
        ESP_LOGW(TAG, "No config path, using defaults");
        return ESP_OK;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "Config file %s not found, using defaults", path);
        return ESP_OK;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 8192) {
        ESP_LOGW(TAG, "Config file invalid size %ld, using defaults", size);
        fclose(f);
        return ESP_OK;
    }

    char *buf = (char *)malloc(size + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        ESP_LOGW(TAG, "Config JSON parse failed, using defaults");
        return ESP_OK;
    }

    /* Parse sections.
     *
     * Every string copy is guarded by a cJSON_IsString() check: a user who
     * writes a number or an object where a string is expected used to pass
     * NULL into strncpy() and take the device down at boot. */
    cJSON *api = cJSON_GetObjectItem(root, "api");
    if (api) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(api, "endpoint")) && cJSON_IsString(v))
            strncpy(cfg->api_endpoint, v->valuestring, sizeof(cfg->api_endpoint) - 1);
        if ((v = cJSON_GetObjectItem(api, "key")) && cJSON_IsString(v))
            strncpy(cfg->api_key, v->valuestring, sizeof(cfg->api_key) - 1);
        if ((v = cJSON_GetObjectItem(api, "model_name")) && cJSON_IsString(v))
            strncpy(cfg->model_name, v->valuestring, sizeof(cfg->model_name) - 1);
        if ((v = cJSON_GetObjectItem(api, "reasoning_effort")) && cJSON_IsString(v))
            strncpy(cfg->reasoning_effort, v->valuestring, sizeof(cfg->reasoning_effort) - 1);
        if ((v = cJSON_GetObjectItem(api, "reasoning_effort_escalated")) && cJSON_IsString(v))
            strncpy(cfg->reasoning_effort_escalated, v->valuestring,
                    sizeof(cfg->reasoning_effort_escalated) - 1);
        if ((v = cJSON_GetObjectItem(api, "effort_escalate_after")) && cJSON_IsNumber(v))
            cfg->effort_escalate_after = v->valueint;
        if ((v = cJSON_GetObjectItem(api, "timeout")) && cJSON_IsNumber(v))
            cfg->timeout = v->valueint;
        if ((v = cJSON_GetObjectItem(api, "max_retries")) && cJSON_IsNumber(v))
            cfg->max_retries = v->valueint;
        if ((v = cJSON_GetObjectItem(api, "max_output_tokens")) && cJSON_IsNumber(v))
            cfg->max_output_tokens = v->valueint;
        if ((v = cJSON_GetObjectItem(api, "upload_max_dim")) && cJSON_IsNumber(v))
            cfg->upload_max_dim = v->valueint;
        if ((v = cJSON_GetObjectItem(api, "save_frames")))
            cfg->save_frames = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(api, "enable_thinking")))
            cfg->enable_thinking = cJSON_IsTrue(v);
    }

    /* Sanity-clamp the retry count: the backoff table only has 4 entries. */
    if (cfg->max_retries > 3) cfg->max_retries = 3;

    cJSON *agent = cJSON_GetObjectItem(root, "agent");
    if (agent) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(agent, "max_actions")) && cJSON_IsNumber(v))
            cfg->max_actions = v->valueint;
        if ((v = cJSON_GetObjectItem(agent, "history_max_len")) && cJSON_IsNumber(v))
            cfg->history_max_len = v->valueint;
        if ((v = cJSON_GetObjectItem(agent, "system_prompt_interval")) && cJSON_IsNumber(v))
            cfg->system_prompt_interval = v->valueint;
        if ((v = cJSON_GetObjectItem(agent, "allow_wait_skip")))
            cfg->allow_wait_skip = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(agent, "max_blind_rounds")) && cJSON_IsNumber(v))
            cfg->max_blind_rounds = v->valueint;
        if ((v = cJSON_GetObjectItem(agent, "wait_settle_ms")) && cJSON_IsNumber(v))
            cfg->wait_settle_ms = v->valueint;
    }

    cJSON *hid = cJSON_GetObjectItem(root, "hid");
    if (hid) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(hid, "type_hold_ms")) && cJSON_IsNumber(v))
            cfg->type_hold_ms = v->valueint;
        if ((v = cJSON_GetObjectItem(hid, "type_gap_ms")) && cJSON_IsNumber(v))
            cfg->type_gap_ms = v->valueint;
        if ((v = cJSON_GetObjectItem(hid, "type_jitter_ms")) && cJSON_IsNumber(v))
            cfg->type_jitter_ms = v->valueint;
        if ((v = cJSON_GetObjectItem(hid, "type_word_pause_ms")) && cJSON_IsNumber(v))
            cfg->type_word_pause_ms = v->valueint;
        if ((v = cJSON_GetObjectItem(hid, "run_dialog_ms")) && cJSON_IsNumber(v))
            cfg->run_dialog_ms = v->valueint;
        if ((v = cJSON_GetObjectItem(hid, "run_settle_ms")) && cJSON_IsNumber(v))
            cfg->run_settle_ms = v->valueint;
    }

    cJSON *uvc = cJSON_GetObjectItem(root, "uvc");
    if (uvc) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(uvc, "capture_width")) && cJSON_IsNumber(v))
            cfg->capture_width = v->valueint;
        if ((v = cJSON_GetObjectItem(uvc, "capture_height")) && cJSON_IsNumber(v))
            cfg->capture_height = v->valueint;
        if ((v = cJSON_GetObjectItem(uvc, "output_width")) && cJSON_IsNumber(v))
            cfg->output_width = v->valueint;
        if ((v = cJSON_GetObjectItem(uvc, "output_height")) && cJSON_IsNumber(v))
            cfg->output_height = v->valueint;
    }

    cJSON *paths = cJSON_GetObjectItem(root, "paths");
    if (paths) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(paths, "profile_dir")) && cJSON_IsString(v))
            strncpy(cfg->profile_dir, v->valuestring, sizeof(cfg->profile_dir) - 1);
        if ((v = cJSON_GetObjectItem(paths, "plan_dir")) && cJSON_IsString(v))
            strncpy(cfg->plan_dir, v->valuestring, sizeof(cfg->plan_dir) - 1);
        if ((v = cJSON_GetObjectItem(paths, "session_dir")) && cJSON_IsString(v))
            strncpy(cfg->session_dir, v->valuestring, sizeof(cfg->session_dir) - 1);
        if ((v = cJSON_GetObjectItem(paths, "default_profile")) && cJSON_IsString(v))
            strncpy(cfg->default_profile, v->valuestring, sizeof(cfg->default_profile) - 1);
        if ((v = cJSON_GetObjectItem(paths, "log_dir")) && cJSON_IsString(v))
            strncpy(cfg->log_dir, v->valuestring, sizeof(cfg->log_dir) - 1);
        if ((v = cJSON_GetObjectItem(paths, "sysprompt_dir")) && cJSON_IsString(v))
            strncpy(cfg->sysprompt_dir, v->valuestring, sizeof(cfg->sysprompt_dir) - 1);
        if ((v = cJSON_GetObjectItem(paths, "web_dir")) && cJSON_IsString(v))
            strncpy(cfg->web_dir, v->valuestring, sizeof(cfg->web_dir) - 1);
    }

    cJSON *http = cJSON_GetObjectItem(root, "http");    if (http) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(http, "port")) && cJSON_IsNumber(v))
            cfg->http_port = (uint16_t)v->valueint;
    }

    cJSON *wifi = cJSON_GetObjectItem(root, "wifi");
    if (wifi) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(wifi, "ssid")) && cJSON_IsString(v))
            strncpy(cfg->wifi_ssid, v->valuestring, sizeof(cfg->wifi_ssid) - 1);
        if ((v = cJSON_GetObjectItem(wifi, "password")) && cJSON_IsString(v))
            strncpy(cfg->wifi_password, v->valuestring, sizeof(cfg->wifi_password) - 1);
    }

    /* Clamp the remaining numeric knobs to sane ranges. */
    if (cfg->timeout < 10) cfg->timeout = 10;
    if (cfg->max_actions == 0) cfg->max_actions = 1;
    if (cfg->history_max_len == 0) cfg->history_max_len = 1;
    if (cfg->system_prompt_interval == 0) cfg->system_prompt_interval = 1;
    if (cfg->capture_width == 0) cfg->capture_width = 1920;
    if (cfg->capture_height == 0) cfg->capture_height = 1080;
    if (cfg->output_width == 0) cfg->output_width = 1920;
    if (cfg->output_height == 0) cfg->output_height = 1080;
    if (cfg->http_port == 0) cfg->http_port = 80;

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Config loaded from %s", path);
    return ESP_OK;
}
