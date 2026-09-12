#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "jpeg_utils.h"    /* view_spec_t */
#include "task_log.h"      /* step_timing_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AGENT_STATE_IDLE,
    AGENT_STATE_RUNNING,
    AGENT_STATE_PAUSED,
    AGENT_STATE_STEP,
    AGENT_STATE_STOPPING,
    AGENT_STATE_EMERGENCY,
} agent_state_t;

/* EventGroup bits */
#define AGENT_EVT_RUN       BIT0
#define AGENT_EVT_STEP      BIT1
#define AGENT_EVT_EMERGENCY BIT2
#define AGENT_EVT_STOP      BIT3

typedef struct {
    agent_state_t state;
    SemaphoreHandle_t state_mutex;
    EventGroupHandle_t control_events;
    TaskHandle_t main_loop_task;

    uint32_t max_actions;
    uint32_t capture_width;
    uint32_t capture_height;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t system_prompt_interval;
    bool dry_run;

    uint32_t action_count;
    uint32_t fail_count;
    uint32_t steps_since_full_prompt;
    bool need_screen;

    /* ── Idle-query skipping (need_screen v2) ──
     *
     * Reusing a screenshot to decide the next action is an *approximation*: the
     * screen may have changed and the model would be reasoning about a stale
     * image. So the harness never does that. Instead `need_screen: false` on a
     * wait-only step means "nothing is happening — do not ask me again until the
     * screen changes", which is honoured only while the frame fingerprint proves
     * the screen really is unchanged, and is capped.
     *
     * Separately, when the new frame is byte-identical to the one already
     * encoded, the prepared payload is reused. That is lossless (same
     * fingerprint => same JPEG => same base64), so it needs no policy switch,
     * and it removes the compression + base64 cost from any step where the
     * screen held still. */
    bool     allow_wait_skip;      /* master switch for skipping queries  */
    uint32_t max_blind_rounds;     /* cap on consecutive skipped queries  */
    uint32_t wait_settle_ms;       /* recheck interval while waiting      */
    uint32_t blind_rounds;         /* consecutive skips so far            */
    bool     wait_mode;            /* model asked to be left alone        */

    /* ── Field of view ──
     * How the next screenshot is presented to the model. Full frame by default;
     * the model can ask for a crop (a zoom) and/or an integer downscale via
     * `next_view` in its response, which applies to the NEXT screenshot.
     *
     * Coordinates the model returns are normalised WITHIN this view, so
     * action_executor has to undo it before touching the mouse. */
    view_spec_t view;

    /* ── Adaptive reasoning effort ──
     * The API call is 60-85% of a step's wall clock and reasoning tokens are
     * 30-50% of the output, so thinking hard on every routine step is the
     * largest avoidable cost. The agent escalates only while it is stuck. */
    char     effort_normal[8];         /* routine budget (config)          */
    char     effort_escalated[8];      /* budget while stuck (config)      */
    uint32_t effort_escalate_after;    /* stuck steps before escalating    */
    uint32_t stuck_streak;             /* consecutive "stuck" steps        */
    char     last_effort[8];           /* what the last query actually used */

    /* Last completed step's latency breakdown, for the WebUI. Mirrors what is
     * written to the serial log so the same numbers can be watched live from
     * the browser. last_timing_step == 0 means "no step completed yet". */
    step_timing_t last_timing;
    uint32_t      last_timing_step;

    /* Same size as SESSION_GOAL_LEN: the goal is echoed in the prompt AND kept
     * by the session, so the two must not truncate at different lengths. */
    char task_goal[1024];
    char active_profile[32];
} agent_ctx_t;

esp_err_t agent_init(agent_ctx_t *ctx, uint32_t max_actions,
                     uint32_t capture_w, uint32_t capture_h,
                     uint32_t output_w, uint32_t output_h,
                     uint32_t prompt_interval, bool dry_run);

/*
 * Configure whether the harness may skip an API call while the model is only
 * waiting and the screen is provably unchanged.
 * Off by default: it depends on the frame fingerprint being a reliable
 * "screen unchanged" signal, which has to be confirmed on real hardware first.
 */
void agent_set_wait_skip_policy(agent_ctx_t *ctx, bool allow,
                                uint32_t max_blind_rounds,
                                uint32_t wait_settle_ms);

/*
 * Configure adaptive reasoning effort.
 *
 * The agent escalates from `normal` to `escalated` once it has been "stuck" for
 * `escalate_after` consecutive steps, where stuck means the previous action
 * produced no visible screen change OR the agent is repeating the same action
 * type. It drops back to `normal` as soon as a step makes visible progress.
 */
void agent_set_effort_policy(agent_ctx_t *ctx, const char *normal,
                             const char *escalated, uint32_t escalate_after);

/*
 * Seed the profile that `profile_updates` are appended to.
 *
 * agent_init() zeroes the context, so without this the active profile is the
 * empty string — and appending to it created a file literally named ".md"
 * instead of the configured default profile.
 */
void agent_set_active_profile(agent_ctx_t *ctx, const char *profile_id);

esp_err_t agent_start(agent_ctx_t *ctx, const char *task_goal);

/*
 * Continue an existing session (conversation window): loads its goal, plan,
 * trajectory and step counter, then runs. Only valid from IDLE.
 */
esp_err_t agent_start_session(agent_ctx_t *ctx, const char *session_id);
esp_err_t agent_pause(agent_ctx_t *ctx);
esp_err_t agent_resume(agent_ctx_t *ctx);
esp_err_t agent_step_once(agent_ctx_t *ctx);
esp_err_t agent_stop(agent_ctx_t *ctx);
esp_err_t agent_emergency(agent_ctx_t *ctx);
agent_state_t agent_get_state(agent_ctx_t *ctx);
agent_ctx_t *agent_get_global(void);

void agent_main_loop_task(void *pvParameters);

#ifdef __cplusplus
}
#endif
