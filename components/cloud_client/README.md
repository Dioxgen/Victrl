# cloud_client — LLM API client

> **English** | [中文](README_CN.md)

## Overview

Calls the Responses API over HTTPS, sending a screenshot plus context strings and returning the model's action JSON; the client handle is kept alive so the keep-alive connection is reused, and per-phase timings are reported.

## Sub-modules

| File | Purpose |
|------|---------|
| `cloud_client.c` | Request body assembly, image crop/scale and reuse, HTTPS send/receive, retry policy and per-phase timing |
| `json_parser.c` | Parses the model's action JSON: strips markdown fences, accepts the old flat schema, drops malformed action entries, fills in defaults |
| `json_safe.h` | Defensive cJSON accessors (`jget_str` / `jget_num` / `jget_int` / `jget_bool` / `jget_array_ok`); on a missing key, a null or a wrong type they return the caller-supplied fallback and never return a NULL string |
| `base64.c` | Base64-encodes JPEG via mbedtls; the output is `malloc`'d and the caller frees it |
| `system_prompts.c` | Loads the `full.txt` / `short.txt` system prompts from the SD card; performs no placeholder substitution |

## Public API

| Interface | Description |
|-----------|-------------|
| `esp_err_t cloud_client_init(const cloud_client_config_t *cfg)` | Copies and validates the config: `api_endpoint` and `api_key` must be non-empty or it returns `ESP_ERR_INVALID_ARG`; `max_retries` is clamped to 3; an invalid `reasoning_effort` / `reasoning_effort_escalated` falls back to `low` / `high` |
| `esp_err_t cloud_client_query(...)` | Main query entry point with 15 parameters: image data and length, `system_prompt`, `user_text`, `plan_json`, `history_text`, `profile_text`, `last_summary`, `status_text`, `effort`, `view`, `reuse_image`, `out_response`, `step_number`, `out_timing` |
| `bool cloud_client_can_reuse_image(const view_spec_t *view)` | Whether the last encoded payload can be reused; true only when a cache exists, its length is greater than 0, and the view matches the one the cache was built with |
| `const uint8_t *cloud_client_get_last_jpeg(size_t *out_len)` | The JPEG bytes actually sent to the model last (the WebUI's `/api/snapshot` uses it to show what the model saw); NULL until the first successful send |
| `cloud_client_timing_t` | Per-phase timings of one call in milliseconds: `prep_ms`, `compress_ms`, `jpeg_setup_ms`, `jpeg_decode_ms`, `jpeg_resample_ms`, `jpeg_encode_ms`, `base64_ms`, `serialize_ms`, `connect_ms`, `http_ms`, `parse_ms`; phases that did not run are 0 |

## Call flow

1. Image preparation: when `reuse_image` is set and `cloud_client_can_reuse_image()` is true, the cached Base64 is used directly; otherwise a non-full view goes through `jpeg_crop_scale()`, a full view with `upload_max_dim > 0` and a frame larger than `COMPRESS_THRESHOLD_BYTES` (80000 bytes) goes through `jpeg_compress()`, and everything else is uploaded untouched; the re-encode quality is the fixed `UPLOAD_JPEG_QUALITY` (50) and is not configurable.
2. `base64_encode()` encodes it, the result is cached, and `save_frames` decides whether `task_log_save_frame()` writes it to the SD card.
3. The request JSON is assembled from `model`, `instructions` (the `system_prompt` verbatim, to preserve the prefix cache), `input[0].content[]` (one `input_text` block plus one `input_image` block whose `image_url` is `data:image/jpeg;base64,...`), a fixed `temperature` of 0.1, `max_output_tokens` and `reasoning.effort`; the volatile content (goal, status, device profile, last action, plan, history) is composed into the user message by `build_user_text()`.
4. UTF-8 safety net: `utf8_sanitize()` scans the whole request body, replaces invalid bytes with `?` and logs a warning.
5. Send: POST to `api_endpoint` with `Content-Type: application/json` and `Authorization: Bearer <api_key>`.
6. Response parsing: when the status is HTTP 200 and the body was received completely, it walks `output[]`, takes the `text` of `content[].type == output_text` inside the `type == message` item as the action text, takes `summary[0].text` of the `type == reasoning` item (falling back to its `content[].text`) as the thinking text and stores it in `_reasoning`; the action text goes to `json_parser_parse()`, and on success `_raw_response`, `_request_size` and `_response_size` are attached.

## Notes

- Connection reuse: the HTTP client handle is persistent with `keep_alive_enable` set to true; `client_teardown()` only runs on a transport error, a non-200 status or an incomplete body (the connection survives a 200 that fails to parse), so `connect_ms == 0` means this step reused an existing connection.
- Retry: the total number of attempts is `max_retries + 1` (`max_retries` is clamped to at most 3) and the backoff table is `{0, 2000, 4000, 8000}` ms — no wait on the first attempt, then 2/4/8 seconds; among 4xx codes only 408 and 429 are retried, every other 4xx is treated as non-retryable and abandons the step immediately.
- The return value alone is not a success signal: when the HTTP status is not 200 or the body fails to parse, the function may still return `ESP_OK`, so the caller must also check whether `*out_response` is NULL (which is exactly what `agent_core` does).
- Parse tolerance: `json_parser_parse()` first strips markdown fences and, if the first parse fails, retries on the slice from the first `{` to the last `}`; it returns NULL — making the step fail and be retried — when the result is not a JSON object, `actions` is not an array, or `plan_update` is missing; entries in `actions` whose `action_type` is missing, empty or not a string are deleted in place, and a valid entry's `action_type` is lowercased.
- Defaults: `json_parser_set_action_defaults()` adds `delay_after = 0.05`, `button = left`, `hold = 0`, `wait_seconds = 0.0`; `json_parser_set_response_defaults()` adds `need_screen = true`, `sleep_before_next = 0.0`, `done = false`, an empty `request_profile` string and an empty `profile_updates` array.
- Prompts: `system_prompts_init()` must read `full.txt` under `sysprompt_dir`, otherwise it returns `ESP_FAIL`; a missing `short.txt` falls back to `full.txt`. The prompts contain no placeholders and need no formatting.
- Large request body: because it carries a Base64 image, the HTTP client sets `buffer_size_tx = 16384` and `buffer_size = 4096`, and the build tree's `sdkconfig` also sets `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN` to 16384 (that file is not version controlled).
- All config keys live in the `api` section of `config.json`, with defaults from `storage_manager/config_loader.c`: `endpoint` (default `https://api.deepseek.com/responses`), `key`, `model_name`, `timeout`, `max_retries` (default 3), `max_output_tokens` (default 16384), `enable_thinking`, `reasoning_effort`, `reasoning_effort_escalated`, `upload_max_dim` (`config_loader` default 1360, the example config sets 0), `save_frames`.
- Thinking has no real "off": `enable_thinking = false` only collapses both effort levels to `low`, and the request carries `reasoning.effort` (`low` / `high` / `max`).
- `cloud_client_query()` only requires `out_response` to be non-NULL; every other pointer parameter may be NULL: an empty image skips the image block, the text parameters fall back to the defaults in `build_user_text()`, an empty `effort` uses the configured value, a NULL `view` means the full frame, and a NULL `out_timing` discards the timings.
