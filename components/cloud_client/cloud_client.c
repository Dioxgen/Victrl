#include "cloud_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "base64.h"
#include "jpeg_utils.h"
#include "json_parser.h"
#include "json_safe.h"
#include "task_log.h"
#include "utf8_util.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cloud_client";

/* Forward declaration: client_ensure() installs this handler. */
static esp_err_t http_event_handler(esp_http_client_event_t *evt);

/* Compress the frame when it is bigger than this, in bytes. */
#define COMPRESS_THRESHOLD_BYTES  80000
/* JPEG quality for the re-encoded upload. The model reads small UI text, so
 * this trades bytes against legibility rather than chasing the smallest body. */
#define UPLOAD_JPEG_QUALITY       50

static cloud_client_config_t s_cfg;

/*
 * Response accumulator + connection timestamps.
 *
 * Kept as a struct rather than a bare char** so the buffer can grow
 * geometrically (the old code called strlen() on the whole body for every
 * chunk and realloc'd exactly, making a large response quadratic) and so the
 * event handler can record when the connection was established.
 */
typedef struct {
    char   *buf;
    size_t  len;
    size_t  cap;
    int64_t t_start_us;
    int64_t t_connected_us;    /* HTTP_EVENT_ON_CONNECTED: after TCP + TLS     */
    int64_t t_first_header_us; /* first response byte from the server          */
    int64_t t_finish_us;
} http_accum_t;

static http_accum_t s_accum;

/* Persistent client: reusing one handle is what lets HTTP/TLS keep-alive
 * actually work across steps. */
static esp_http_client_handle_t s_client = NULL;

/*
 * Cached base64 of the last prepared frame.
 *
 * Compressing and base64-encoding a 1080p frame costs a few hundred
 * milliseconds. When the agent has verified that the screen did not change, the
 * next request can carry the identical image, so that work is skipped entirely
 * rather than repeated. The raw JPEG is deliberately not cached — only the
 * encoded payload, which is the expensive part.
 */
static char  *s_cached_b64 = NULL;
static size_t s_cached_b64_len = 0;

/* The view the cached payload was produced with. A cached crop is only valid
 * for the frame AND the view it was made from. */
static view_spec_t s_cached_view = VIEW_SPEC_FULL;

/* The exact bytes handed to the encoder, kept so the WebUI can show what the
 * model saw. ~150KB, replaced every step. */
static uint8_t *s_last_jpeg = NULL;
static size_t   s_last_jpeg_len = 0;

static void cache_store_last_jpeg(const uint8_t *data, size_t len)
{
    if (!data || !len) return;
    uint8_t *copy = (uint8_t *)realloc(s_last_jpeg, len);
    if (!copy) return;                 /* keep the old one; the preview is optional */
    memcpy(copy, data, len);
    s_last_jpeg = copy;
    s_last_jpeg_len = len;
}

const uint8_t *cloud_client_get_last_jpeg(size_t *out_len)
{
    if (out_len) *out_len = s_last_jpeg_len;
    return s_last_jpeg;
}

bool cloud_client_can_reuse_image(const view_spec_t *view)
{
    view_spec_t want = VIEW_SPEC_FULL;
    if (view) want = *view;
    if (!view_normalize(&want)) want = (view_spec_t)VIEW_SPEC_FULL;

    return (s_cached_b64 != NULL) && (s_cached_b64_len > 0) &&
           view_equal(&s_cached_view, &want);
}

esp_err_t cloud_client_init(const cloud_client_config_t *cfg)
{
    if (!cfg || !cfg->api_endpoint[0] || !cfg->api_key[0]) {
        ESP_LOGE(TAG, "Invalid config: endpoint and api_key required");
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(&s_cfg, cfg, sizeof(s_cfg));

    /* Normalise both reasoning efforts; anything unexpected falls back to a
     * safe value rather than being sent to the API as-is. */
    if (strcmp(s_cfg.reasoning_effort, "low") != 0 &&
        strcmp(s_cfg.reasoning_effort, "high") != 0 &&
        strcmp(s_cfg.reasoning_effort, "max") != 0) {
        strcpy(s_cfg.reasoning_effort, "low");
    }
    if (strcmp(s_cfg.reasoning_effort_escalated, "low") != 0 &&
        strcmp(s_cfg.reasoning_effort_escalated, "high") != 0 &&
        strcmp(s_cfg.reasoning_effort_escalated, "max") != 0) {
        strcpy(s_cfg.reasoning_effort_escalated, "high");
    }

    /* retry_delays[] below has 4 entries — keep the index in range. */
    if (s_cfg.max_retries > 3) s_cfg.max_retries = 3;

    ESP_LOGI(TAG, "Initialized: endpoint=%s model=%s timeout=%lu "
                  "effort=%s/%s max_dim=%lu save_frames=%d",
             s_cfg.api_endpoint, s_cfg.model_name, s_cfg.timeout_sec,
             s_cfg.enable_thinking ? s_cfg.reasoning_effort : "low",
             s_cfg.enable_thinking ? s_cfg.reasoning_effort_escalated : "low",
             s_cfg.upload_max_dim, (int)s_cfg.save_frames);
    return ESP_OK;
}

/* ── HTTP plumbing ─────────────────────────────────────────────────── */

static void client_teardown(void)
{
    if (s_client) {
        esp_http_client_close(s_client);
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
}

static esp_err_t client_ensure(void)
{
    if (s_client) return ESP_OK;

    esp_http_client_config_t http_cfg = {
        .url = s_cfg.api_endpoint,
        .method = HTTP_METHOD_POST,
        .timeout_ms = (int)(s_cfg.timeout_sec * 1000),
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* Keep the TCP+TLS session alive between steps. Establishing a fresh
         * connection every step means a full handshake including certificate
         * chain verification, which is the single largest removable item on
         * the per-step network path. */
        .keep_alive_enable = true,
        .buffer_size = 4096,
        .buffer_size_tx = 16384,
        .event_handler = http_event_handler,
        .user_data = &s_accum,
    };

    s_client = esp_http_client_init(&http_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_accum_t *a = (http_accum_t *)evt->user_data;
    if (!a) return ESP_OK;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_CONNECTED:
        /* Fires once per TCP+TLS establishment. On a reused keep-alive
         * connection it does NOT fire — so connect_ms == 0 is the signal that
         * the previous session was still good. */
        if (a->t_connected_us == 0) a->t_connected_us = esp_timer_get_time();
        break;

    case HTTP_EVENT_ON_HEADER:
        if (a->t_first_header_us == 0) a->t_first_header_us = esp_timer_get_time();
        break;

    case HTTP_EVENT_ON_DATA: {
        if (evt->data_len <= 0 || !evt->data) break;
        size_t need = a->len + (size_t)evt->data_len + 1;
        if (need > a->cap) {
            size_t newcap = a->cap ? a->cap : 4096;
            while (newcap < need) newcap *= 2;
            char *nb = (char *)realloc(a->buf, newcap);
            if (!nb) return ESP_ERR_NO_MEM;
            a->buf = nb;
            a->cap = newcap;
        }
        memcpy(a->buf + a->len, evt->data, evt->data_len);
        a->len += (size_t)evt->data_len;
        a->buf[a->len] = '\0';
        break;
    }

    case HTTP_EVENT_ON_FINISH:
        if (a->t_finish_us == 0) a->t_finish_us = esp_timer_get_time();
        break;

    default:
        break;
    }
    return ESP_OK;
}

static void accum_reset(void)
{
    free(s_accum.buf);
    memset(&s_accum, 0, sizeof(s_accum));
}

/* ── Request body ──────────────────────────────────────────────────── */

/*
 * Build the volatile part of the user message.
 *
 * Everything that changes between steps lives here: the device profile, the
 * previous action's outcome, the plan, the history and the goal. None of it is
 * allowed into `instructions`, because the API's context cache is prefix
 * based — a system prompt that changes every step can never be a cache hit.
 */
static char *build_user_text(const char *user_text, const char *plan_json,
                             const char *history_text, const char *profile_text,
                             const char *last_summary, const char *status_text)
{
    const char *goal    = (user_text && user_text[0]) ? user_text : "(unspecified)";
    const char *plan    = plan_json ? plan_json : "No plan yet.";
    const char *history = history_text ? history_text : "No history yet.";
    const char *profile = (profile_text && profile_text[0]) ? profile_text
                                                            : "No device profile loaded.";
    const char *summary = (last_summary && last_summary[0]) ? last_summary
        : "(This is the first step - there is no prior action to evaluate.)";
    const char *status  = (status_text && status_text[0]) ? status_text
                                                          : "(unavailable)";

    static const char *const fmt =
        "**Goal:** %s\n\n"
        "**Status:** %s\n\n"
        "**Device profile:**\n%s\n\n"
        "**Your last action:** %s\n\n"
        "**Current plan:**\n%s\n\n"
        "**Recent steps (oldest first):**\n%s\n\n"
        "TASK: Analyze the screen, evaluate whether your last action worked, "
        "and output the next action as JSON.";

    int n = snprintf(NULL, 0, fmt, goal, status, profile, summary, plan, history);
    if (n < 0) return NULL;

    char *out = (char *)malloc((size_t)n + 1);
    if (!out) return NULL;

    snprintf(out, (size_t)n + 1, fmt,
             goal, status, profile, summary, plan, history);
    return out;
}

/* ── Query ─────────────────────────────────────────────────────────── */

esp_err_t cloud_client_query(
    const uint8_t *jpeg_data,
    size_t jpeg_len,
    const char *system_prompt,
    const char *user_text,
    const char *plan_json,
    const char *history_text,
    const char *profile_text,
    const char *last_summary,
    const char *status_text,
    const char *effort,
    const view_spec_t *view,
    bool reuse_image,
    cJSON **out_response,
    uint32_t step_number,
    cloud_client_timing_t *out_timing)
{
    if (!out_response) return ESP_ERR_INVALID_ARG;
    *out_response = NULL;

    cloud_client_timing_t local_timing = {0};
    cloud_client_timing_t *timing = out_timing ? out_timing : &local_timing;
    memset(timing, 0, sizeof(*timing));

    int64_t t_prep_start = esp_timer_get_time();

    /* `instructions` is passed through verbatim: it must stay byte-identical
     * across steps for the prefix cache to hit. */
    const char *instructions = system_prompt ? system_prompt : "";

    /* ── Step A: prepare the image, or reuse the prepared one ── */
    bool have_image = false;
    esp_err_t ret;

    view_spec_t want_view = VIEW_SPEC_FULL;
    if (view) want_view = *view;
    if (!view_normalize(&want_view)) want_view = (view_spec_t)VIEW_SPEC_FULL;

    if (reuse_image && cloud_client_can_reuse_image(&want_view)) {
        /* The caller verified the screen is unchanged AND the view matches, so
         * the cached payload is still an exact description of what is on
         * screen. */
        have_image = true;
        ESP_LOGD(TAG, "Reusing cached image payload (%zu base64 bytes)",
                 s_cached_b64_len);
    } else if (jpeg_data && jpeg_len > 0) {
        const uint8_t *upload_data = jpeg_data;
        size_t upload_len = jpeg_len;
        uint8_t *transformed = NULL;
        size_t transformed_len = 0;

        int64_t t0 = esp_timer_get_time();
        if (!view_is_full(&want_view)) {
            /* A crop or a scale was requested, so the frame must be decoded.
             * This is the price of the zoom: the hardware decoder can neither
             * crop nor scale. */
            ret = jpeg_crop_scale(jpeg_data, jpeg_len, &want_view,
                                  UPLOAD_JPEG_QUALITY,
                                  &transformed, &transformed_len);
            if (ret == ESP_OK && transformed && transformed_len > 0) {
                upload_data = transformed;
                upload_len = transformed_len;
            } else {
                ESP_LOGW(TAG, "crop/scale failed (%d) — sending the full frame", ret);
                transformed = NULL;
            }
        } else if (s_cfg.upload_max_dim > 0 &&
                   jpeg_len > COMPRESS_THRESHOLD_BYTES) {
            /* upload_max_dim == 0 means "do not touch the frame": the MS2109
             * already delivers a complete JPEG and the model resizes it
             * server-side anyway, so decoding it is pure cost. */
            ret = jpeg_compress(jpeg_data, jpeg_len, &transformed, &transformed_len,
                                UPLOAD_JPEG_QUALITY, (int)s_cfg.upload_max_dim);
            if (ret == ESP_OK && transformed && transformed_len > 0) {
                upload_data = transformed;
                upload_len = transformed_len;
            } else {
                ESP_LOGW(TAG, "JPEG compress failed (%d), using original", ret);
                transformed = NULL;
            }
        }
        timing->compress_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

        /* Sub-phase split of the transform. `compress_ms > 0` is the test for
         * "a transform actually ran this step": the stage record belongs to the
         * last jpeg_utils call, so on a pass-through step it would otherwise be
         * the previous step's numbers. */
        if (timing->compress_ms > 0) {
            const jpeg_stage_timing_t *st = jpeg_utils_last_stages();
            timing->jpeg_setup_ms    = st->setup_ms;
            timing->jpeg_decode_ms   = st->decode_ms;
            timing->jpeg_resample_ms = st->resample_ms;
            timing->jpeg_encode_ms   = st->encode_ms;
        }

        /* Keep the exact bytes for /api/snapshot, and optionally persist them. */
        cache_store_last_jpeg(upload_data, upload_len);
        if (s_cfg.save_frames) {
            /* Blocking SD write — off by default; it sits on the critical path
             * right before the HTTP request. */
            task_log_save_frame(step_number, upload_data, upload_len);
        }

        t0 = esp_timer_get_time();
        uint8_t *b64_raw = NULL;
        size_t b64_raw_len = 0;
        ret = base64_encode(upload_data, upload_len, &b64_raw, &b64_raw_len);
        timing->base64_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        free(transformed);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Base64 encode failed");
            return ret;
        }

        /* Swap into the cache; the old payload is released. */
        free(s_cached_b64);
        s_cached_b64 = (char *)b64_raw;
        s_cached_b64_len = b64_raw_len;
        s_cached_view = want_view;
        have_image = true;
    }

    /* ── Step B: build the JSON body ── */
    int64_t t_json = esp_timer_get_time();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", s_cfg.model_name);
    cJSON_AddStringToObject(root, "instructions", instructions);

    cJSON *input_arr = cJSON_AddArrayToObject(root, "input");
    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON *content_arr = cJSON_AddArrayToObject(user_msg, "content");

    char *body_text = build_user_text(user_text, plan_json, history_text,
                                      profile_text, last_summary, status_text);
    cJSON *text_block = cJSON_CreateObject();
    cJSON_AddStringToObject(text_block, "type", "input_text");
    cJSON_AddStringToObject(text_block, "text", body_text ? body_text : "TASK: continue.");
    cJSON_AddItemToArray(content_arr, text_block);
    free(body_text);

    if (have_image && s_cached_b64) {
        cJSON *img_block = cJSON_CreateObject();
        cJSON_AddStringToObject(img_block, "type", "input_image");
        size_t url_len = 32 + s_cached_b64_len;
        char *img_url = (char *)malloc(url_len);
        if (img_url) {
            snprintf(img_url, url_len, "data:image/jpeg;base64,%s", s_cached_b64);
            cJSON_AddStringToObject(img_block, "image_url", img_url);
            free(img_url);
        }
        cJSON_AddItemToArray(content_arr, img_block);
    }

    cJSON_AddItemToArray(input_arr, user_msg);
    cJSON_AddNumberToObject(root, "temperature", 0.1);
    cJSON_AddNumberToObject(root, "max_output_tokens", s_cfg.max_output_tokens);

    /* Thinking / reasoning budget.
     *
     * DeepSeek's Responses API controls this with `reasoning: {effort: ...}`
     * where effort is low | high | max. The `thinking: {type: "disabled"}`
     * field used by Volcengine Ark does not exist in the OpenAI Responses
     * schema and is silently ignored by DeepSeek, so it is not sent.
     * There is no true "off" — disabling thinking maps to the cheapest
     * effort instead. */
    cJSON *reasoning = cJSON_CreateObject();
    const char *use_effort = s_cfg.enable_thinking
        ? ((effort && effort[0]) ? effort : s_cfg.reasoning_effort)
        : "low";
    cJSON_AddStringToObject(reasoning, "effort", use_effort);
    cJSON_AddItemToObject(root, "reasoning", reasoning);

    char *json_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    /* The base64 payload stays in the cache for a possible reuse next step. */

    timing->serialize_ms = (uint32_t)((esp_timer_get_time() - t_json) / 1000);

    if (!json_body) {
        ESP_LOGE(TAG, "Failed to serialize JSON body");
        return ESP_ERR_NO_MEM;
    }

    /*
     * Guarantee the body is valid UTF-8 before it goes out.
     *
     * A JSON body must be valid UTF-8, and the API answers 400 (fast, with no
     * useful message) when it is not — which is how a single Chinese character
     * cut in half by a byte-wise copy took down every step of a task while
     * looking like a generic network failure. The truncation sites are fixed,
     * but this is the one place every request passes through, so the invariant
     * is enforced here rather than trusted.
     */
    size_t repaired = utf8_sanitize(json_body);
    if (repaired) {
        ESP_LOGW(TAG, "Request body had %zu invalid UTF-8 byte(s) — replaced. "
                      "Some text was truncated mid-character upstream.",
                 repaired);
    }

    const size_t json_len = strlen(json_body);
    timing->prep_ms = (uint32_t)((esp_timer_get_time() - t_prep_start) / 1000);
    ESP_LOGI(TAG, "Request body: %zu bytes", json_len);

    /* ── Step C: send with retry ── */
    uint32_t retry_delays[] = {0, 2000, 4000, 8000};
    cJSON *parsed = NULL;
    esp_err_t last_err = ESP_FAIL;

    for (uint32_t attempt = 0; attempt <= s_cfg.max_retries; attempt++) {
        if (attempt > 0) {
            ESP_LOGI(TAG, "Retry %lu/%lu after %lums",
                     attempt, s_cfg.max_retries, retry_delays[attempt]);
            vTaskDelay(pdMS_TO_TICKS(retry_delays[attempt]));
        }

        if (client_ensure() != ESP_OK) {
            last_err = ESP_FAIL;
            continue;
        }

        char auth_header[256];
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_cfg.api_key);
        esp_http_client_set_header(s_client, "Content-Type", "application/json");
        esp_http_client_set_header(s_client, "Authorization", auth_header);
        esp_http_client_set_post_field(s_client, json_body, (int)json_len);

        accum_reset();
        s_accum.t_start_us = esp_timer_get_time();
        esp_err_t err = esp_http_client_perform(s_client);
        if (s_accum.t_finish_us == 0) s_accum.t_finish_us = esp_timer_get_time();

        int status = esp_http_client_get_status_code(s_client);

        uint32_t http_ms = (uint32_t)((s_accum.t_finish_us - s_accum.t_start_us) / 1000);
        uint32_t connect_ms = s_accum.t_connected_us
            ? (uint32_t)((s_accum.t_connected_us - s_accum.t_start_us) / 1000) : 0;
        timing->http_ms = http_ms;
        if (attempt == 0) timing->connect_ms = connect_ms;  /* first try only */

        bool body_complete = esp_http_client_is_complete_data_received(s_client);

        /* Time to first response byte: separates TLS+send from server think time. */
        uint32_t ttfb_ms = s_accum.t_first_header_us
            ? (uint32_t)((s_accum.t_first_header_us - s_accum.t_start_us) / 1000) : 0;

        if (err == ESP_OK && status == 200 && s_accum.buf && body_complete) {
            ESP_LOGI(TAG, "HTTP %d in %lums (connect %lums%s, ttfb %lums), %zu response bytes",
                     status, http_ms, connect_ms,
                     connect_ms ? "" : " reused", ttfb_ms, s_accum.len);

            int64_t t_parse = esp_timer_get_time();

            /* Extract output_text from API response.
             * Every field here is remote input: a missing "type" or a
             * non-string "text" used to dereference NULL and panic. */
            cJSON *api_resp = cJSON_Parse(s_accum.buf);
            if (api_resp) {
                cJSON *output = cJSON_GetObjectItem(api_resp, "output");
                cJSON *item = NULL;
                char *action_text = NULL;
                char *reasoning_text = NULL;
                cJSON_ArrayForEach(item, output) {
                    const char *type = jget_str(item, "type", NULL);
                    if (!type) continue;

                    if (strcmp(type, "message") == 0) {
                        cJSON *content = cJSON_GetObjectItem(item, "content");
                        cJSON *block = NULL;
                        cJSON_ArrayForEach(block, content) {
                            if (strcmp(jget_str(block, "type", ""), "output_text") == 0) {
                                const char *t = jget_str(block, "text", NULL);
                                if (t) action_text = (char *)t;
                            }
                        }
                    } else if (strcmp(type, "reasoning") == 0) {
                        /* Ark returns a summary array; DeepSeek documents
                         * `summary` as unsupported but may still populate a
                         * plain-text `content`. Try both, tolerate neither. */
                        cJSON *summary = cJSON_GetObjectItem(item, "summary");
                        if (summary && cJSON_IsArray(summary)) {
                            cJSON *s = cJSON_GetArrayItem(summary, 0);
                            const char *t = jget_str(s, "text", NULL);
                            if (t) reasoning_text = (char *)t;
                        }
                        if (!reasoning_text) {
                            cJSON *content = cJSON_GetObjectItem(item, "content");
                            cJSON *part = NULL;
                            cJSON_ArrayForEach(part, content) {
                                const char *t = jget_str(part, "text", NULL);
                                if (t) { reasoning_text = (char *)t; break; }
                            }
                        }
                    }
                }

                if (action_text && action_text[0]) {
                    parsed = json_parser_parse(action_text);
                    if (parsed && reasoning_text) {
                        /* Distinct name: `reasoning` already exists above as the
                         * request's reasoning object. */
                        cJSON *reasoning_node = cJSON_CreateString(reasoning_text);
                        if (reasoning_node)
                            cJSON_AddItemToObject(parsed, "_reasoning", reasoning_node);
                    }
                }
                cJSON_Delete(api_resp);
            }

            timing->parse_ms = (uint32_t)((esp_timer_get_time() - t_parse) / 1000);

            if (parsed) {
                /* Metadata for the task log. */
                cJSON *raw = cJSON_CreateString(s_accum.buf);
                if (raw) cJSON_AddItemToObject(parsed, "_raw_response", raw);
                cJSON *rs = cJSON_CreateNumber((double)json_len);
                if (rs) cJSON_AddItemToObject(parsed, "_request_size", rs);
                cJSON *rs2 = cJSON_CreateNumber((double)s_accum.len);
                if (rs2) cJSON_AddItemToObject(parsed, "_response_size", rs2);
                last_err = ESP_OK;
                break;
            }
            ESP_LOGW(TAG, "Parse failed on attempt %lu", attempt + 1);
        } else {
            ESP_LOGW(TAG, "HTTP error on attempt %lu: err=%d status=%d complete=%d",
                     attempt + 1, err, status, (int)body_complete);
            if (s_accum.buf && s_accum.buf[0]) {
                ESP_LOGI(TAG, "Response: %.200s", s_accum.buf);
            }
            /* Drop the connection: a stale keep-alive socket would otherwise
             * be retried as-is and fail the same way. */
            client_teardown();

            /*
             * A 4xx is the request's fault, not the network's, so replaying the
             * identical body only sends it four times and burns 2000+4000+8000ms
             * of backoff. In the field a single malformed byte turned one step
             * into a 19.5s failure. 408 and 429 are the two 4xx codes a retry
             * genuinely helps with.
             */
            if (status >= 400 && status < 500 && status != 408 && status != 429) {
                ESP_LOGE(TAG, "HTTP %d is not retryable — abandoning this step", status);
                last_err = err;
                break;
            }
        }
        last_err = err;
    }

    accum_reset();
    free(json_body);

    if (parsed) {
        *out_response = parsed;
        return ESP_OK;
    }

    ESP_LOGE(TAG, "All %lu attempts failed", s_cfg.max_retries + 1);
    return last_err;
}
