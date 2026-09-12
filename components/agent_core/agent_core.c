#include "agent_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "action_executor.h"
#include "cloud_client.h"
#include "hid_device.h"
#include "json_parser.h"
#include "json_safe.h"
#include "storage_manager.h"
#include "utf8_util.h"
#include "system_prompts.h"
#include "uvc_capture_card_driver.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const char *TAG = "agent_core";
static agent_ctx_t *g_agent = NULL;

/* Defined below; declared here so the abort paths can record the end state. */
static char *build_status_text(agent_ctx_t *ctx, const char *screen_changed);
static void  log_final_state(const char *why);

/* The task goal is truncated when it enters the session AND when it enters the
 * context; if those two sizes ever drift, one of them silently cuts the goal. */
_Static_assert(sizeof(((agent_ctx_t *)0)->task_goal) == SESSION_GOAL_LEN,
               "agent_ctx_t.task_goal must match SESSION_GOAL_LEN");

/* ── State machine ──────────────────────────────────────────────────── */

esp_err_t agent_init(agent_ctx_t *ctx, uint32_t max_actions,
                     uint32_t capture_w, uint32_t capture_h,
                     uint32_t output_w, uint32_t output_h,
                     uint32_t prompt_interval, bool dry_run)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;

    memset(ctx, 0, sizeof(*ctx));
    ctx->state = AGENT_STATE_IDLE;
    ctx->max_actions = max_actions;
    ctx->capture_width = capture_w;
    ctx->capture_height = capture_h;
    ctx->output_width = output_w;
    ctx->output_height = output_h;
    ctx->system_prompt_interval = prompt_interval;
    ctx->dry_run = dry_run;
    ctx->need_screen = true;

    /* memset() zeroed this, and an all-zero view is NOT "full frame" — it would
     * crop the model down to a degenerate corner. */
    ctx->view = (view_spec_t)VIEW_SPEC_FULL;

    ctx->state_mutex = xSemaphoreCreateMutex();
    if (!ctx->state_mutex) {
        ESP_LOGE(TAG, "Failed to create state mutex");
        return ESP_ERR_NO_MEM;
    }

    ctx->control_events = xEventGroupCreate();
    if (!ctx->control_events) {
        ESP_LOGE(TAG, "Failed to create event group");
        vSemaphoreDelete(ctx->state_mutex);
        return ESP_ERR_NO_MEM;
    }

    g_agent = ctx;
    ESP_LOGI(TAG, "Agent initialized: capture=%lux%lu output=%lux%lu max_actions=%lu",
             capture_w, capture_h, output_w, output_h, max_actions);
    return ESP_OK;
}

agent_ctx_t *agent_get_global(void)
{
    return g_agent;
}

agent_state_t agent_get_state(agent_ctx_t *ctx)
{
    agent_state_t state;
    xSemaphoreTake(ctx->state_mutex, portMAX_DELAY);
    state = ctx->state;
    xSemaphoreGive(ctx->state_mutex);
    return state;
}

static void agent_set_state(agent_ctx_t *ctx, agent_state_t new_state)
{
    xSemaphoreTake(ctx->state_mutex, portMAX_DELAY);
    ctx->state = new_state;
    xSemaphoreGive(ctx->state_mutex);
}

/*
 * Abort the current task and go back to IDLE *without* killing the main loop.
 *
 * Every abort path used to `break` out of the loop's `while (1)`, which
 * permanently deleted the agent task: the state could still be flipped to
 * RUNNING by the WebUI or the start button, but nothing was left alive to
 * service it, so the device required a power cycle. Always unwind to IDLE.
 */
static void agent_abort_to_idle(agent_ctx_t *ctx, const char *reason)
{
    ESP_LOGE(TAG, "%s - aborting, HID released, back to IDLE", reason);
    action_executor_emergency_release();
    log_final_state(reason);
    task_log_close();
    xEventGroupClearBits(ctx->control_events,
        AGENT_EVT_RUN | AGENT_EVT_STEP | AGENT_EVT_STOP | AGENT_EVT_EMERGENCY);
    agent_set_state(ctx, AGENT_STATE_IDLE);
}

static bool agent_should_continue(agent_ctx_t *ctx)
{
    xSemaphoreTake(ctx->state_mutex, portMAX_DELAY);
    bool cont = (ctx->state == AGENT_STATE_RUNNING || ctx->state == AGENT_STATE_STEP);
    xSemaphoreGive(ctx->state_mutex);
    return cont;
}

esp_err_t agent_start(agent_ctx_t *ctx, const char *task_goal)
{
    if (!ctx || !task_goal || !task_goal[0]) return ESP_ERR_INVALID_ARG;
    if (agent_get_state(ctx) != AGENT_STATE_IDLE) {
        ESP_LOGW(TAG, "Cannot start: not in IDLE state");
        return ESP_ERR_INVALID_STATE;
    }

    /* Starting a task opens a NEW conversation window. Every session keeps its
     * own goal, plan and trajectory on the SD card. */
    char sid[SESSION_ID_LEN] = {0};
    if (session_mgr_create(task_goal, sid) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot create a session");
        return ESP_FAIL;
    }

    xSemaphoreTake(ctx->state_mutex, portMAX_DELAY);
    utf8_copy(ctx->task_goal, sizeof(ctx->task_goal), task_goal);
    ctx->state = AGENT_STATE_RUNNING;
    ctx->action_count = 0;
    ctx->fail_count = 0;
    ctx->steps_since_full_prompt = 0;
    ctx->need_screen = true;
    ctx->view = (view_spec_t)VIEW_SPEC_FULL;   /* new task starts wide */
    xSemaphoreGive(ctx->state_mutex);

    /* The IME-toggle counter is per task: its whole point is to tell the model
     * "you have already tried this N times on THIS task". */
    action_executor_reset_ime_toggles();

    task_log_init("/sdcard/log", sid);

    xEventGroupSetBits(ctx->control_events, AGENT_EVT_RUN);
    ESP_LOGI(TAG, "Agent started in session %s: %s", sid, task_goal);
    return ESP_OK;
}

/*
 * Continue an existing session instead of opening a new one.
 *
 * Loads that session's goal, plan and trajectory, and restores its step
 * counter, so the agent picks up exactly where the window left off.
 */
esp_err_t agent_start_session(agent_ctx_t *ctx, const char *session_id)
{
    if (!ctx || !session_id || !session_id[0]) return ESP_ERR_INVALID_ARG;
    if (agent_get_state(ctx) != AGENT_STATE_IDLE) {
        ESP_LOGW(TAG, "Cannot resume session: agent is not IDLE");
        return ESP_ERR_INVALID_STATE;
    }

    if (session_mgr_load(session_id) != ESP_OK) {
        ESP_LOGW(TAG, "No such session: %s", session_id);
        return ESP_ERR_NOT_FOUND;
    }

    const char *goal = session_mgr_current_goal();

    xSemaphoreTake(ctx->state_mutex, portMAX_DELAY);
    utf8_copy(ctx->task_goal, sizeof(ctx->task_goal), goal);
    ctx->state = AGENT_STATE_RUNNING;
    ctx->action_count = session_mgr_current_steps();
    ctx->fail_count = 0;
    ctx->steps_since_full_prompt = 0;
    ctx->need_screen = true;
    ctx->view = (view_spec_t)VIEW_SPEC_FULL;   /* resuming starts wide too */
    xSemaphoreGive(ctx->state_mutex);

    /* Appending keeps the earlier steps of this session in the log. */
    task_log_init("/sdcard/log", session_id);

    xEventGroupSetBits(ctx->control_events, AGENT_EVT_RUN);
    ESP_LOGI(TAG, "Resumed session %s at step %lu: %s",
             session_id, (unsigned long)ctx->action_count, goal);
    return ESP_OK;
}

esp_err_t agent_pause(agent_ctx_t *ctx)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(ctx->state_mutex, portMAX_DELAY);
    if (ctx->state != AGENT_STATE_RUNNING) {
        xSemaphoreGive(ctx->state_mutex);
        ESP_LOGW(TAG, "Cannot pause: not running");
        return ESP_ERR_INVALID_STATE;
    }
    ctx->state = AGENT_STATE_PAUSED;
    xSemaphoreGive(ctx->state_mutex);
    xEventGroupClearBits(ctx->control_events, AGENT_EVT_RUN);
    ESP_LOGI(TAG, "Agent paused");
    return ESP_OK;
}

esp_err_t agent_resume(agent_ctx_t *ctx)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(ctx->state_mutex, portMAX_DELAY);
    if (ctx->state != AGENT_STATE_PAUSED) {
        xSemaphoreGive(ctx->state_mutex);
        ESP_LOGW(TAG, "Cannot resume: not paused");
        return ESP_ERR_INVALID_STATE;
    }
    ctx->state = AGENT_STATE_RUNNING;
    xSemaphoreGive(ctx->state_mutex);
    xEventGroupSetBits(ctx->control_events, AGENT_EVT_RUN);
    ESP_LOGI(TAG, "Agent resumed");
    return ESP_OK;
}

esp_err_t agent_step_once(agent_ctx_t *ctx)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(ctx->state_mutex, portMAX_DELAY);
    if (ctx->state != AGENT_STATE_PAUSED) {
        xSemaphoreGive(ctx->state_mutex);
        ESP_LOGW(TAG, "Cannot step: not paused");
        return ESP_ERR_INVALID_STATE;
    }
    ctx->state = AGENT_STATE_STEP;
    xSemaphoreGive(ctx->state_mutex);
    xEventGroupSetBits(ctx->control_events, AGENT_EVT_STEP);
    ESP_LOGI(TAG, "Agent step once");
    return ESP_OK;
}

esp_err_t agent_stop(agent_ctx_t *ctx)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;
    agent_set_state(ctx, AGENT_STATE_STOPPING);
    xEventGroupSetBits(ctx->control_events, AGENT_EVT_STOP);
    ESP_LOGI(TAG, "Agent stop requested");
    return ESP_OK;
}

esp_err_t agent_emergency(agent_ctx_t *ctx)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;
    agent_set_state(ctx, AGENT_STATE_EMERGENCY);
    xEventGroupSetBits(ctx->control_events, AGENT_EVT_EMERGENCY);
    action_executor_emergency_release();
    ESP_LOGE(TAG, "EMERGENCY STOP");
    return ESP_OK;
}

/* ── Build context strings ──────────────────────────────────────────── */

static char *build_history_text(void)
{
    /* The session trajectory, not the short-term ring: it carries action
     * PARAMETERS and per-turn outcomes, which is what the model needs in order
     * to judge its own last step and to compare the coordinates it sent against
     * the cursor position the status line reports. */
    return session_mgr_get_trajectory();
}

static char *build_plan_text(void)
{
    cJSON *plan = plan_mgr_get_current();   /* returns a duplicate — must free */
    if (!plan) return strdup("No plan yet.");
    /* Unformatted: this goes into the prompt on every step, and the indentation
     * costs tokens while carrying no information. */
    char *s = cJSON_PrintUnformatted(plan);
    cJSON_Delete(plan);
    return s ? s : strdup("{}");
}

static char *build_profile_text(void)
{
    char *profile = profile_mgr_get_default();
    if (!profile) return strdup("No device profile loaded.");
    return profile;
}

/*
 * Status bar.
 *
 * Deterministically maintained meta-information about the run, computed by code
 * rather than inferred by the model — the model reliably believes whatever a
 * status line says and will not recompute it.
 *
 * The two entries that matter most:
 *
 *  - Cursor position. The device is the only party that knows it exactly. The
 *    pointer is frequently invisible or ambiguous in a screenshot, so after a
 *    bare `move` the model otherwise cannot tell where it left the cursor.
 *  - `screen_changed`. Objective feedback on whether the previous action had any
 *    visible effect at all. The prompt asks the model to self-evaluate, but a
 *    pixel-level answer beats a guess — and "no change" is the clearest possible
 *    signal that the last action missed.
 */
static char *build_status_text(agent_ctx_t *ctx, const char *screen_changed)
{
    uint16_t abs_x = 0, abs_y = 0;
    uint8_t buttons = 0;
    uint32_t sw = 0, sh = 0;
    hid_mouse_get_state(&abs_x, &abs_y, &buttons, &sw, &sh);

    /* HID absolute units (0..32767) back to pixels and to [0,1]. */
    uint32_t px = sw ? (uint32_t)(((uint32_t)abs_x * sw) / HID_ABS_MAX) : 0;
    uint32_t py = sh ? (uint32_t)(((uint32_t)abs_y * sh) / HID_ABS_MAX) : 0;
    double nx = (double)abs_x / (double)HID_ABS_MAX;
    double ny = (double)abs_y / (double)HID_ABS_MAX;

    char btn[64];
    if (buttons == 0) {
        snprintf(btn, sizeof(btn), "none");
    } else {
        snprintf(btn, sizeof(btn), "%s%s%s",
                 (buttons & 0x01) ? "LEFT " : "",
                 (buttons & 0x02) ? "RIGHT " : "",
                 (buttons & 0x04) ? "MIDDLE" : "");
    }

    char view_buf[144];
    const char *view_txt;
    if (view_is_full(&ctx->view)) {
        view_txt = "FULL";
    } else {
        snprintf(view_buf, sizeof(view_buf),
                 "ROI y%.3f-%.3f x%.3f-%.3f scale=%u — box_2d is normalised "
                 "WITHIN this view",
                 ctx->view.y0, ctx->view.y1, ctx->view.x0, ctx->view.x1,
                 ctx->view.scale);
        view_txt = view_buf;
    }

    /* Lock-key state, from the host's HID LED output report. This is the only
     * host-side state a HID device is given, and both bits matter when text
     * comes out wrong: Caps Lock turns every letter into shifted output, and
     * Num Lock decides whether the keypad path can be used at all. */
    uint8_t leds = hid_keyboard_led_state();
    bool leds_known = hid_keyboard_led_seen();
    char lock_buf[48];
    snprintf(lock_buf, sizeof(lock_buf), "num=%s caps=%s",
             !leds_known ? "?" : ((leds & 0x01) ? "on" : "off"),
             !leds_known ? "?" : ((leds & 0x02) ? "on" : "off"));

    char buf[896];
    snprintf(buf, sizeof(buf),
             "view: %s | step %lu/%lu | cursor=(%.3f, %.3f) px=(%lu, %lu) | "
             "held buttons: %s | "
             "screen since last screenshot: %s | consecutive actions without a new "
             "screenshot: %lu/%lu | recent actions: %s x%d of last 15 | "
             "IME toggles sent: %d | locks: %s | last type: %s",
             view_txt,
             ctx->action_count + 1, ctx->max_actions,
             nx, ny, px, py, btn,
             screen_changed ? screen_changed : "unknown",
             ctx->blind_rounds, ctx->max_blind_rounds,
             action_executor_last_type()[0] ? action_executor_last_type() : "(none)",
             action_executor_repetition_count(),
             action_executor_ime_toggle_count(),
             lock_buf,
             action_executor_get_last_type_report());

    /* The status line is the longest Chinese-bearing string in the request, and
     * it carries a device-supplied report verbatim. Repair any tail that the
     * 896-byte bound happened to cut through a character. */
    utf8_sanitize(buf);
    return strdup(buf);
}

/*
 * Record the end state of a run.
 *
 * This is the only place it exists. A task can stop on a batch whose actions
 * changed the pointer (the model may set done:true in the same response as a
 * move), and that batch is never screenshotted — so without this the log says
 * the task finished but not where things ended up, and the outcome cannot be
 * verified from the log alone.
 */
static void log_final_state(const char *why)
{
    if (!g_agent) return;
    char *st = build_status_text(g_agent, why ? why : "n/a");
    if (!st) return;
    task_log_write_note("--- End state ---");
    task_log_write_note(st);
    free(st);
}

/* Read array element `idx` as a number, or `fallback` when absent/not numeric. */
static float arr_num(const cJSON *arr, int idx, float fallback)
{
    const cJSON *it = cJSON_GetArrayItem(arr, idx);
    return (it && cJSON_IsNumber(it)) ? (float)it->valuedouble : fallback;
}

/*
 * Adopt the field of view the model asked for in `next_view`.
 *
 * It applies to the NEXT screenshot, never to the one just analysed, and it
 * PERSISTS until the model changes it (see the note in the body). A ROI whose
 * extent is below VIEW_MIN_EXTENT is ignored rather than repaired into a
 * degenerate corner — the model almost certainly meant "nothing special".
 */
static void agent_adopt_next_view(agent_ctx_t *ctx, const cJSON *response)
{
    /*
     * `next_view` is STICKY: the region stays in force until the model changes
     * it. Sending no `next_view` keeps the current one; `{}` returns to FULL.
     *
     * This was one-shot, which silently reverted the view on the first step
     * where the model did not repeat the request — a state change the model
     * never asked for. In testing it spotted the revert and spent a long
     * reasoning block trying to explain it. Rule 13 documents `{}` as the way
     * back to the full frame, which is only meaningful if the region persists.
     *
     * Persistence is also the cheaper behaviour. Measured on this hardware, the
     * ROI step costs ~200ms of decode+resample but saves more than that in
     * base64 alone (25ms vs 150ms) because the payload shrinks ~4.7x — and it
     * cuts the upload from ~406KB to ~87KB. Sticky costs nothing extra per step;
     * one-shot meant the model had to re-send the ROI every step to keep it.
     */
    cJSON *nj = cJSON_GetObjectItem(response, "next_view");
    if (!nj || !cJSON_IsObject(nj)) return;   /* absent: keep the current view */

    view_spec_t nv = (view_spec_t)VIEW_SPEC_FULL;   /* `{}` means "go wide" */

    cJSON *roi = cJSON_GetObjectItem(nj, "roi");
    if (roi && cJSON_IsArray(roi) && cJSON_GetArraySize(roi) >= 4) {
        float y0 = arr_num(roi, 0, 0.0f);
        float x0 = arr_num(roi, 1, 0.0f);
        float y1 = arr_num(roi, 2, 1.0f);
        float x1 = arr_num(roi, 3, 1.0f);
        float eh = (y1 > y0) ? (y1 - y0) : (y0 - y1);
        float ew = (x1 > x0) ? (x1 - x0) : (x0 - x1);
        if (eh >= VIEW_MIN_EXTENT && ew >= VIEW_MIN_EXTENT) {
            nv.y0 = y0; nv.x0 = x0; nv.y1 = y1; nv.x1 = x1;
        } else {
            ESP_LOGW(TAG, "next_view roi too small (%.3f x %.3f) — ignored",
                     ew, eh);
        }
    }
    int sc = jget_int(nj, "scale", 1);
    nv.scale = (uint8_t)((sc == 2 || sc == 4) ? sc : 1);

    if (!view_normalize(&nv)) nv = (view_spec_t)VIEW_SPEC_FULL;

    if (!view_equal(&nv, &ctx->view)) {
        ESP_LOGI(TAG, "Next view: ROI y%.3f-%.3f x%.3f-%.3f scale=%u",
                 nv.y0, nv.y1, nv.x0, nv.x1, nv.scale);
    }
    ctx->view = nv;
}

/* Record the breakdown so the WebUI can read it, then log it. */
static void agent_publish_timing(agent_ctx_t *ctx, uint32_t step,
                                 const step_timing_t *t)
{
    ctx->last_timing = *t;
    ctx->last_timing_step = step;
    task_log_write_timing(step, t);
}

/*
 * Append one field to a trajectory record, keeping the buffer NUL-terminated.
 *
 * `n` carries the running length (as snprintf reports it, which may exceed the
 * capacity). Bounds are re-checked on every call so no remaining-capacity
 * subtraction can wrap, and the terminator is written on every path.
 */
static void rec_append(char *rec, size_t cap, int *n, const char *fmt,
                       const char *text)
{
    if (*n < 0) *n = 0;
    if ((size_t)*n >= cap) { rec[cap - 1] = '\0'; return; }

    int w = snprintf(rec + *n, cap - (size_t)*n, fmt, text);
    if (w > 0) *n += w;
    rec[cap - 1] = '\0';
}

/*
 * Append one turn to the current session's trajectory.
 *
 * This IS the model's memory. It is the only place where what the agent DID
 * (with parameters), what it SAW and what HAPPENED are kept together — the raw
 * API responses go to the SD task log and are deliberately never sent back.
 *
 * Coordinates in the action description are FULL-FRAME even when the model was
 * looking at a crop, so history stays readable across view changes; `look=`
 * records which region that step was actually shown.
 */
static void agent_record_turn(agent_ctx_t *ctx, const view_spec_t *view,
                              const char *actions, const char *obs,
                              const char *self_eval, const char *plan_summary)
{
    char ts[16] = "--:--:--";
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);

    char look[80];
    if (view && !view_is_full(view)) {
        snprintf(look, sizeof(look), "ROI(y%.2f-%.2f x%.2f-%.2f s%u)",
                 view->y0, view->y1, view->x0, view->x1, view->scale);
    } else {
        snprintf(look, sizeof(look), "full");
    }

    /* rec is larger than the worst case (~1027 bytes) so nothing is cut. The
     * four text fields are pre-truncated by utf8_copy(), which never splits a
     * character. */
    char rec[1152];
    char f[288];
    int n = snprintf(rec, sizeof(rec), "#%lu [%s] look=%s",
                     (unsigned long)ctx->action_count, ts, look);
    if (n < 0) n = 0;

    utf8_copy(f, sizeof(f), actions ? actions : "(none)");
    rec_append(rec, sizeof(rec), &n, " | action: %s", f);

    utf8_copy(f, 241, obs ? obs : "(none)");
    rec_append(rec, sizeof(rec), &n, "\n   saw: %s", f);

    utf8_copy(f, 201, self_eval ? self_eval : "(none)");
    rec_append(rec, sizeof(rec), &n, "\n   result: %s", f);

    utf8_copy(f, 141, plan_summary ? plan_summary : "-");
    rec_append(rec, sizeof(rec), &n, "\n   plan: %s", f);

    utf8_sanitize(rec);    /* defence in depth against any future raw "%.Ns" */

    session_mgr_append_turn(rec);
    session_mgr_set_steps(ctx->action_count);
}

void agent_set_wait_skip_policy(agent_ctx_t *ctx, bool allow,
                                uint32_t max_blind_rounds,
                                uint32_t wait_settle_ms)
{
    if (!ctx) return;
    ctx->allow_wait_skip = allow;
    ctx->max_blind_rounds = max_blind_rounds;
    ctx->wait_settle_ms = wait_settle_ms ? wait_settle_ms : 1000;
    ctx->blind_rounds = 0;
    ctx->wait_mode = false;
    ESP_LOGI(TAG, "Wait-skip policy: allow=%d max_rounds=%lu settle=%lums",
             (int)allow, max_blind_rounds, ctx->wait_settle_ms);
}

void agent_set_effort_policy(agent_ctx_t *ctx, const char *normal,
                             const char *escalated, uint32_t escalate_after)
{
    if (!ctx) return;
    if (normal && normal[0]) {
        strncpy(ctx->effort_normal, normal, sizeof(ctx->effort_normal) - 1);
    }
    if (escalated && escalated[0]) {
        strncpy(ctx->effort_escalated, escalated,
                sizeof(ctx->effort_escalated) - 1);
    }
    ctx->effort_escalate_after = escalate_after ? escalate_after : 1;
    ctx->stuck_streak = 0;
    ESP_LOGI(TAG, "Effort policy: normal=%s escalated=%s after=%lu stuck step(s)",
             ctx->effort_normal, ctx->effort_escalated, ctx->effort_escalate_after);
}

void agent_set_active_profile(agent_ctx_t *ctx, const char *profile_id)
{
    if (!ctx) return;
    if (profile_id && profile_id[0]) {
        strncpy(ctx->active_profile, profile_id, sizeof(ctx->active_profile) - 1);
        ctx->active_profile[sizeof(ctx->active_profile) - 1] = '\0';
    }
    ESP_LOGI(TAG, "Active profile: %s",
             ctx->active_profile[0] ? ctx->active_profile : "(none - check config)");
}

/*
 * Which reasoning budget to use for the next query.
 *
 * "Stuck" is measured objectively from things the harness already knows: the
 * last action had no visible effect, or the agent keeps issuing the same action
 * type. Both mean more thinking is worth paying for. Any visible progress
 * drops the agent straight back to the cheap budget.
 */
static const char *agent_pick_effort(agent_ctx_t *ctx, bool stuck, bool *escalated)
{
    if (stuck) {
        if (ctx->stuck_streak < 0xFFFFFFFFu) ctx->stuck_streak++;
    } else {
        ctx->stuck_streak = 0;
    }

    bool use_escalated = (ctx->stuck_streak >= ctx->effort_escalate_after);
    if (escalated) *escalated = use_escalated;

    const char *e = use_escalated ? ctx->effort_escalated : ctx->effort_normal;
    snprintf(ctx->last_effort, sizeof(ctx->last_effort), "%s",
             (e && e[0]) ? e : "low");
    return ctx->last_effort;
}

/* ── Main loop ──────────────────────────────────────────────────────── */

void agent_main_loop_task(void *pvParameters)
{
    agent_ctx_t *ctx = (agent_ctx_t *)pvParameters;
    ESP_LOGI(TAG, "Main loop task started");

    /* Fingerprint of the frame the model last looked at. Stored across
     * iterations so each step can tell whether the screen moved since the
     * previous query — which is both the image-cache key and the objective
     * answer to "did my last action have any effect?". */
    uvc_frame_sig_t prev_sig = {0};
    bool have_prev_sig = false;

    while (1) {
        /* Block only if not actively running (IDLE/PAUSED) */
        EventBits_t bits;
        if (!agent_should_continue(ctx)) {
            bits = xEventGroupWaitBits(
                ctx->control_events,
                AGENT_EVT_RUN | AGENT_EVT_STEP | AGENT_EVT_EMERGENCY | AGENT_EVT_STOP,
                pdTRUE, pdFALSE, portMAX_DELAY);
        } else {
            bits = xEventGroupGetBits(ctx->control_events);
            xEventGroupClearBits(ctx->control_events,
                AGENT_EVT_EMERGENCY | AGENT_EVT_STOP);
        }

        if (bits & AGENT_EVT_EMERGENCY) {
            agent_abort_to_idle(ctx, "Emergency stop");
            continue;
        }

        if (bits & AGENT_EVT_STOP) {
            ESP_LOGI(TAG, "Stop event — returning to IDLE");
            task_log_close();
            xEventGroupClearBits(ctx->control_events,
                AGENT_EVT_RUN | AGENT_EVT_STEP);
            agent_set_state(ctx, AGENT_STATE_IDLE);
            continue;
        }

        /* State check */
        if (!agent_should_continue(ctx)) {
            /* Single step done, return to PAUSED */
            xSemaphoreTake(ctx->state_mutex, portMAX_DELAY);
            if (ctx->state == AGENT_STATE_STEP) {
                ctx->state = AGENT_STATE_PAUSED;
            }
            xSemaphoreGive(ctx->state_mutex);
            continue;
        }

        /* Check max actions */
        if (ctx->action_count >= ctx->max_actions) {
            ESP_LOGW(TAG, "Max actions reached (%lu)", ctx->max_actions);
            task_log_close();
            xEventGroupClearBits(ctx->control_events,
                AGENT_EVT_RUN | AGENT_EVT_STEP);
            agent_set_state(ctx, AGENT_STATE_IDLE);
            continue;
        }

        /* ── Step 1: Capture screen ──
         * A step always runs. `need_screen` no longer gates the whole body; it
         * only says whether the model wants to be left alone while nothing is
         * happening (handled by the wait-mode check below). */
        step_timing_t timing = {0};
        int64_t t_step_start = esp_timer_get_time();
        bool captured = false;
        uint8_t *frame_data = NULL;
        size_t frame_len = 0;
        bool reuse_image = false;
        bool screen_unchanged = false;
        const char *screen_changed_txt = "unknown (first screenshot)";

        {
            int64_t t_cap = esp_timer_get_time();
            uvc_frame_sig_t sig = {0};
            esp_err_t err = uvc_capture_one_frame_sig(&frame_data, &frame_len,
                                                      &sig, 5000);
            timing.capture_ms = (uint32_t)((esp_timer_get_time() - t_cap) / 1000);
            if (err == ESP_OK && frame_data && frame_len > 0) {
                captured = true;
                ESP_LOGD(TAG, "Captured frame: %zu bytes", frame_len);

                /* Compare with the frame the model last looked at. A matching
                 * fingerprint means identical JPEG bytes, so the encoded
                 * payload can be reused verbatim — and it is also the objective
                 * answer to "did my last action do anything?". */
                if (have_prev_sig) {
                    bool same = (sig.len_bytes == prev_sig.len_bytes) &&
                                (sig.hash == prev_sig.hash);
                    screen_unchanged = same;
                    /* Only reuse when the payload was made from THIS view: a crop
                     * of the previous frame is not a crop of this one. */
                    reuse_image = same && cloud_client_can_reuse_image(&ctx->view);
                    if (same) {
                        screen_changed_txt = "NO - identical to the screenshot you last saw";
                        if (reuse_image) {
                            ESP_LOGI(TAG, "Screen unchanged since last query (%lu bytes); "
                                          "reusing encoded payload", sig.len_bytes);
                        }
                    } else {
                        screen_changed_txt = "yes - the screen differs from the screenshot you last saw";
                    }
                }
                prev_sig = sig;
                have_prev_sig = true;

                /* The model asked only to wait and the screen has still not
                 * moved: there is nothing new to decide, so skip the API call
                 * and wait again. Bounded by max_blind_rounds. */
                if (ctx->allow_wait_skip && ctx->wait_mode && reuse_image &&
                    ctx->blind_rounds < ctx->max_blind_rounds) {
                    ctx->blind_rounds++;
                    ESP_LOGI(TAG, "Wait mode: screen still unchanged, skipping API call "
                                  "(%lu/%lu)", ctx->blind_rounds, ctx->max_blind_rounds);
                    free(frame_data);
                    frame_data = NULL;
                    vTaskDelay(pdMS_TO_TICKS(ctx->wait_settle_ms));
                    continue;
                }
                ctx->blind_rounds = 0;
                ctx->wait_mode = false;
            } else {
                ESP_LOGW(TAG, "Frame capture failed: %d", err);
                ctx->fail_count++;
                if (ctx->fail_count >= 3) {
                    agent_abort_to_idle(ctx, "Too many capture failures");
                    continue;
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            if (captured) {
                /* ── Step 2: Select system prompt ──
                 * An interval of 0 means "never rotate": always send the full
                 * prompt. That keeps `instructions` byte-identical on every
                 * step, which is what the API's prefix cache needs. Rotating in
                 * the short prompt saves a few hundred tokens — a fraction of
                 * a cent — but invalidates the entire cached prefix. */
                ctx->steps_since_full_prompt++;
                bool use_full = (ctx->system_prompt_interval == 0) ||
                                (ctx->steps_since_full_prompt >= ctx->system_prompt_interval) ||
                                (ctx->action_count == 0);
                const char *prompt = use_full ?
                    system_prompts_get_full() : system_prompts_get_short();

                if (use_full) {
                    ctx->steps_since_full_prompt = 0;
                }

                /* ── Step 3: Build context ── */
                char *profile_text = build_profile_text();
                char *plan_text = build_plan_text();
                char *history_text = build_history_text();
                char *status_text = build_status_text(ctx, screen_changed_txt);
                const char *last_summary = action_executor_get_last_summary();

                /* ── Step 4: Query LLM ──
                 * Escalate the reasoning budget only while the agent is stuck.
                 * Both "stuck" signals are objective and already computed:
                 * the previous action produced no visible screen change, or the
                 * agent keeps issuing the same action type. */
                bool stuck = screen_unchanged ||
                             (action_executor_repetition_count() >= 4);
                bool escalated = false;
                const char *effort = agent_pick_effort(ctx, stuck, &escalated);

                cloud_client_timing_t cc_timing = {0};
                int64_t t_api_start = esp_timer_get_time();
                cJSON *response = NULL;
                esp_err_t qerr = cloud_client_query(
                    frame_data, frame_len,
                    prompt,
                    ctx->task_goal,
                    plan_text,
                    history_text,
                    profile_text,
                    last_summary,
                    status_text,
                    effort,
                    &ctx->view,
                    reuse_image,
                    &response,
                    ctx->action_count + 1,
                    &cc_timing);
                int64_t t_api_end = esp_timer_get_time();
                uint32_t api_time_ms = (uint32_t)((t_api_end - t_api_start) / 1000);
                free(status_text);

                /* Log step BEFORE freeing context strings */
                const char *obs = NULL, *self_eval = NULL, *reasoning = NULL;
                const char *act_summary = action_executor_get_last_summary();
                const char *raw_resp = NULL;
                uint32_t req_bytes = 0, resp_bytes = 0;
                if (response) {
                    obs        = jget_str(response, "observation", NULL);
                    self_eval  = jget_str(response, "self_evaluation", NULL);
                    reasoning  = jget_str(response, "_reasoning", NULL);
                    raw_resp   = jget_str(response, "_raw_response", NULL);
                    req_bytes  = (uint32_t)jget_num(response, "_request_size", 0);
                    resp_bytes = (uint32_t)jget_num(response, "_response_size", 0);
                }

                int64_t t_log = esp_timer_get_time();
                task_log_write_step(ctx->action_count + 1, obs, self_eval,
                                    act_summary, reasoning, plan_text,
                                    raw_resp ? raw_resp : "no response",
                                    use_full,
                                    req_bytes, resp_bytes, api_time_ms, 0);
                timing.log_ms = (uint32_t)((esp_timer_get_time() - t_log) / 1000);

                /* Carry the network-side breakdown into the step timing. */
                timing.prep_ms      = cc_timing.prep_ms;
                timing.compress_ms  = cc_timing.compress_ms;
                timing.jpeg_setup_ms    = cc_timing.jpeg_setup_ms;
                timing.jpeg_decode_ms   = cc_timing.jpeg_decode_ms;
                timing.jpeg_resample_ms = cc_timing.jpeg_resample_ms;
                timing.jpeg_encode_ms   = cc_timing.jpeg_encode_ms;
                timing.base64_ms    = cc_timing.base64_ms;
                timing.serialize_ms = cc_timing.serialize_ms;
                timing.connect_ms   = cc_timing.connect_ms;
                timing.http_ms      = cc_timing.http_ms;
                timing.parse_ms     = cc_timing.parse_ms;

                /* Free context strings */
                free(profile_text);
                free(plan_text);
                free(history_text);

                /* Free frame data (from uvc_capture_one_frame) */
                free(frame_data);

                if (qerr != ESP_OK || !response) {
                    ESP_LOGW(TAG, "LLM query failed: %d", qerr);
                    /* Still emit the breakdown: on a failure the timing is what
                     * tells you whether it was a timeout, a dead socket or a
                     * parse error. */
                    timing.total_ms = (uint32_t)((esp_timer_get_time() - t_step_start) / 1000);
                    agent_publish_timing(ctx, ctx->action_count + 1, &timing);
                    ctx->fail_count++;
                    if (ctx->fail_count >= 3) {
                        agent_abort_to_idle(ctx, "Too many LLM failures");
                        continue;
                    }
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    continue;
                }

                /* Reset fail count on success */
                ctx->fail_count = 0;

                /* ── Step 5: Parse response ── */
                cJSON *actions = cJSON_GetObjectItem(response, "actions");
                bool done = jget_bool(response, "done", false);
                /* Defaults are installed by json_parser_set_response_defaults(),
                 * but read defensively anyway: a missing key here used to be a
                 * hard NULL dereference. */
                double sleep_before = jget_num(response, "sleep_before_next", 0.0);

                /* ── Step 6: Update need_screen ── */
                bool need_screen = jget_bool(response, "need_screen", true);

                /* ── Step 7: Execute actions ──
                 * The model's boxes are normalised to the image it was shown, so
                 * tell the executor which view that was before anything moves. */
                action_executor_set_view(&ctx->view);
                if (actions && cJSON_IsArray(actions) && cJSON_GetArraySize(actions) > 0) {
                    int64_t t_exec = esp_timer_get_time();
                    action_executor_execute_all(actions, ctx->output_width, ctx->output_height);
                    timing.exec_ms = (uint32_t)((esp_timer_get_time() - t_exec) / 1000);

                    /* Was this batch pure waiting? Only then may the model be
                     * left alone until the screen changes. Note the harness no
                     * longer needs to force need_screen after state-changing
                     * actions: any non-wait action makes all_wait false, so the
                     * old override was redundant — and it actively contradicted
                     * the prompt's "chain actions aggressively" guidance. */
                    cJSON *action = NULL;
                    bool all_wait = true;
                    int n_actions = 0;
                    cJSON_ArrayForEach(action, actions) {
                        const char *atype = jget_str(action, "action_type", NULL);
                        if (!atype) continue;
                        n_actions++;
                        if (strcmp(atype, "wait") != 0) all_wait = false;
                    }
                    ctx->wait_mode = all_wait && (n_actions > 0) && !need_screen;
                    if (ctx->wait_mode) {
                        ESP_LOGI(TAG, "Wait-only step with need_screen=false — "
                                      "will skip queries until the screen changes");
                    }
                }

                ctx->need_screen = need_screen;
                ctx->action_count++;

                /* ── Step 8: Update memories ── */
                const char *plan_summary = NULL;
                cJSON *plan_update = cJSON_GetObjectItem(response, "plan_update");
                if (plan_update) {
                    plan_mgr_save(plan_update);
                    plan_summary = jget_str(plan_update, "summary", NULL);
                }

                /* Handle profile request */
                const char *req_profile = jget_str(response, "request_profile", NULL);
                if (req_profile && req_profile[0]) {
                    xSemaphoreTake(ctx->state_mutex, portMAX_DELAY);
                    /* utf8_copy, not strncpy: the model picks this id and it may
                     * be Chinese, and it is used as a filename. */
                    utf8_copy(ctx->active_profile, sizeof(ctx->active_profile),
                              req_profile);
                    xSemaphoreGive(ctx->state_mutex);
                    ESP_LOGI(TAG, "Profile switch requested: %s", req_profile);
                }

                /* Handle profile updates */
                cJSON *pup = cJSON_GetObjectItem(response, "profile_updates");
                if (pup && cJSON_IsArray(pup)) {
                    cJSON *update = NULL;
                    cJSON_ArrayForEach(update, pup) {
                        const char *content = jget_str(update, "content", NULL);
                        if (content && content[0]) {
                            profile_mgr_append(content, ctx->active_profile);
                        }
                    }
                }

                /* Add to short-term memory */
                if (plan_update) {
                    const char *summary_item = jget_str(plan_update, "summary", NULL);
                    if (summary_item && summary_item[0]) {
                        stm_add(summary_item);
                    }
                }

                /* Persist this turn into the session trajectory, using the view
                 * the model was actually shown for it. */
                agent_record_turn(ctx, &ctx->view, act_summary, obs, self_eval,
                                  plan_summary);

                /* Now adopt whatever view it asked for next. */
                agent_adopt_next_view(ctx, response);

                ESP_LOGI(TAG, "Step %lu: %s | done=%d | next_screen=%d | sleep=%.2f "
                              "| effort=%s%s | stuck=%lu",
                         ctx->action_count,
                         obs ? obs : "(no observation)",
                         (int)done, (int)need_screen, sleep_before,
                         effort, escalated ? " (escalated)" : "",
                         ctx->stuck_streak);

                /* ── Step 9: Check done ── */
                if (done) {
                    ESP_LOGI(TAG, "Task complete after %lu steps", ctx->action_count);
                    timing.total_ms = (uint32_t)((esp_timer_get_time() - t_step_start) / 1000);
                    timing.sleep_ms = 0;
                    agent_publish_timing(ctx, ctx->action_count, &timing);
                    log_final_state("done");
                    task_log_close();
                    cJSON_Delete(response);
                    agent_set_state(ctx, AGENT_STATE_IDLE);
                    continue;
                }

                cJSON_Delete(response);

                /* ── Step 10: Sleep before next ──
                 * The floor is unconditional now: a fresh frame is always
                 * captured next, so the UI must always be given time to
                 * repaint before it is read. */
                double delay_s = sleep_before;
                if (delay_s < 1.0) {
                    delay_s = 1.0;
                }
                if (delay_s > 300.0) delay_s = 300.0;  /* cap a bogus huge sleep */
                int64_t t_sleep = esp_timer_get_time();
                if (delay_s > 0) {
                    vTaskDelay(pdMS_TO_TICKS((uint32_t)(delay_s * 1000)));
                }
                timing.sleep_ms = (uint32_t)((esp_timer_get_time() - t_sleep) / 1000);

                timing.total_ms = (uint32_t)((esp_timer_get_time() - t_step_start) / 1000);
                agent_publish_timing(ctx, ctx->action_count, &timing);
            }
        }
    }

    ESP_LOGI(TAG, "Main loop task exiting");
    vTaskDelete(NULL);
}
