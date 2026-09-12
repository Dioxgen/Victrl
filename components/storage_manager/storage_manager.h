#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Config ─────────────────────────────────────────────────────────── */

typedef struct {
    char api_endpoint[128];
    char api_key[128];
    char model_name[64];
    uint32_t timeout;
    uint32_t max_retries;
    uint32_t max_output_tokens;
    bool enable_thinking;
    char reasoning_effort[8];   /* "low" | "high" | "max" */
    char reasoning_effort_escalated[8];  /* used while the agent is stuck */
    uint32_t effort_escalate_after;      /* stuck steps before escalating */
    uint32_t upload_max_dim;    /* longest edge of the uploaded frame; 0 = none */
    bool save_frames;           /* write each captured frame to the SD card   */
    uint32_t max_actions;
    uint32_t history_max_len;
    uint32_t system_prompt_interval;
    /* Idle-query skipping (need_screen v2). Off by default: it relies on the
     * frame fingerprint being a trustworthy "screen unchanged" signal. */
    bool     allow_wait_skip;
    uint32_t max_blind_rounds;
    uint32_t wait_settle_ms;

    /* ── HID typing / Run-dialog timing ──
     * These are control-channel parameters, not performance knobs: a dropped
     * character costs a whole model round trip plus a recovery sequence. The
     * right values depend on how loaded the target machine is, so they are
     * configurable rather than compiled in. */
    uint32_t type_hold_ms;        /* key held down                     */
    uint32_t type_gap_ms;         /* base delay between keys           */
    uint32_t type_jitter_ms;      /* random extra per key              */
    uint32_t type_word_pause_ms;  /* extra pause after a space         */
    uint32_t run_dialog_ms;       /* Win+R until the dialog accepts input */
    uint32_t run_settle_ms;       /* after typing, before Enter        */
    uint32_t capture_width;
    uint32_t capture_height;
    uint32_t output_width;
    uint32_t output_height;
    char profile_dir[64];
    char plan_dir[64];          /* kept for config compatibility; unused now
                                 * that each session owns its own plan */
    char session_dir[64];       /* conversation windows                     */
    char default_profile[32];
    char log_dir[64];
    char sysprompt_dir[64];
    char web_dir[64];
    uint16_t http_port;
    char wifi_ssid[64];
    char wifi_password[64];
} runtime_config_t;

esp_err_t config_load(const char *path, runtime_config_t *cfg);

/* ── Profile Manager ────────────────────────────────────────────────── */

esp_err_t profile_mgr_init(const char *profile_dir, const char *default_id);
char *profile_mgr_get(const char *identifier);
char *profile_mgr_get_default(void);
esp_err_t profile_mgr_append(const char *content, const char *identifier);

/* ── Plan Manager ───────────────────────────────────────────────────── */

esp_err_t plan_mgr_init(const char *plan_dir);
esp_err_t plan_mgr_new_task(const char *task_goal, char task_id_out[32]);
/* Same, but the caller supplies the id (the session id). */
esp_err_t plan_mgr_new_task_with_id(const char *id, const char *task_goal);
/* Load <plan_dir>/plan.json into memory (used when switching sessions). */
esp_err_t plan_mgr_load(void);
esp_err_t plan_mgr_save(cJSON *plan_update);
cJSON *plan_mgr_get_current(void);

/* ── Sessions (conversation windows) ────────────────────────────────── */

#include "session_manager.h"

/* ── Task Log ───────────────────────────────────────────────────────── */

#include "task_log.h"

/* ── Short-Term Memory ──────────────────────────────────────────────── */

void stm_init(size_t max_len);
void stm_add(const char *summary);
char *stm_get_all_formatted(void);
void stm_clear(void);

#ifdef __cplusplus
}
#endif
