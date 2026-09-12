#pragma once

#include <stdbool.h>
#include <stddef.h>   /* size_t — do not rely on esp_err.h to pull it in */
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Per-step latency breakdown, in milliseconds.
 *
 * Instrumentation exists so that optimisation is driven by measurement rather
 * than guesswork: each field maps to one phase of the sense-think-act loop, so
 * a single log line shows where a step's wall-clock time actually went.
 *
 * All values are 0 when the phase did not run in that step.
 */
typedef struct {
    uint32_t capture_ms;    /* wait for a UVC frame + copy out of the driver   */
    uint32_t prep_ms;       /* whole cloud_client prep: prompt + JPEG + base64 + JSON */
    uint32_t compress_ms;   /*   subset of prep_ms: JPEG decode + re-encode    */
    uint32_t jpeg_setup_ms; /*     subset of the two below: codec/buffer setup */
    uint32_t jpeg_decode_ms;/*     subset: hardware decode at full resolution  */
    uint32_t jpeg_resample_ms; /*  subset: software crop + downscale            */
    uint32_t jpeg_encode_ms;/*     subset: hardware re-encode                  */
    uint32_t base64_ms;     /*   subset of prep_ms: base64 encode              */
    uint32_t serialize_ms;  /*   subset of prep_ms: cJSON_PrintUnformatted     */
    uint32_t connect_ms;    /* TCP + TLS handshake (inside perform)            */
    uint32_t http_ms;       /* full esp_http_client_perform()                  */
    uint32_t parse_ms;      /* response JSON parse + action normalisation      */
    uint32_t log_ms;        /* task log write to SD                            */
    uint32_t exec_ms;       /* HID action execution                            */
    uint32_t sleep_ms;      /* sleep_before_next                               */
    uint32_t total_ms;      /* capture start -> end of sleep                   */
} step_timing_t;

esp_err_t task_log_init(const char *log_dir, const char *task_id);
esp_err_t task_log_write_step(uint32_t step, const char *observation,
                               const char *self_eval, const char *action_summary,
                               const char *reasoning, const char *plan_json,
                               const char *raw_response, bool used_full_prompt,
                               uint32_t request_bytes, uint32_t response_bytes,
                               uint32_t api_time_ms, uint32_t exec_time_ms);

/* Append the phase breakdown for one step. Also emitted via ESP_LOGI. */
esp_err_t task_log_write_timing(uint32_t step, const step_timing_t *t);

/*
 * Append a free-form line to the task log.
 *
 * Used to record the end state (cursor position, view, why the run stopped),
 * which is not part of any step and used to be lost: a task that ended on
 * `done: true` in the same batch as a state-changing action left no record of
 * where the pointer actually ended up, so the result could not be verified
 * from the log alone.
 */
esp_err_t task_log_write_note(const char *text);

esp_err_t task_log_save_frame(uint32_t step, const uint8_t *data, size_t len);
esp_err_t task_log_close(void);

#ifdef __cplusplus
}
#endif
