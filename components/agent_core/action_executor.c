#include "action_executor.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "hid_device.h"
#include "jpeg_utils.h"
#include "json_safe.h"
#include "utf8_util.h"
#include "uvc_capture_card_driver.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "action_exec";

static char s_last_summary[512] = "No actions executed yet.";

/*
 * Evidence line for the most recent `type`.
 *
 * The single most useful fact when typed text comes out wrong is whether the
 * DEVICE actually sent every keystroke. If it did, the loss is downstream of
 * the device — and on a machine with a Chinese IME that is almost always the
 * input method eating the input (a space becomes a candidate-selection key, and
 * letters sit in the composition buffer until committed). Surfacing this to the
 * model turns guesswork into a diagnosis.
 */
static char s_last_type_report[288] = "none";

const char *action_executor_get_last_type_report(void)
{
    return s_last_type_report;
}

/*
 * Timing for the `run` action (Win+R → command → Enter).
 *
 * Configurable because how long the Run dialog takes to appear, and how long
 * the launched application takes to become usable, depends entirely on the
 * target machine.
 */
static uint32_t s_run_dialog_ms = 500;   /* Win+R → dialog is ready to type   */
static uint32_t s_run_settle_ms = 400;   /* typed text committed before Enter */
static uint32_t s_run_verify_ms = 700;   /* after Enter, before checking the screen */

/*
 * The field of view the model's coordinates are relative to.
 *
 * Module state rather than a parameter on every action: the agent loop is
 * single-threaded and sets it once per step, and keeping it in one place means
 * the model->screen mapping exists in exactly one function (box_to_pixel).
 */
static view_spec_t s_view = VIEW_SPEC_FULL;

void action_executor_set_run_timing(uint32_t dialog_ms, uint32_t settle_ms)
{
    if (dialog_ms) s_run_dialog_ms = dialog_ms;
    if (settle_ms) s_run_settle_ms = settle_ms;
    ESP_LOGI(TAG, "Run timing: dialog=%lums settle=%lums",
             s_run_dialog_ms, s_run_settle_ms);
}

/*
 * Screen-change probe.
 *
 * `run` is the one action where the harness knows exactly what should happen:
 * the command should launch and the screen should visibly change. Checking that
 * makes the action self-correcting, which matters a lot — in the field a single
 * ignored Enter cost a whole model round trip (5s) to notice and fix.
 *
 * The initial Enter is genuinely unreliable on Windows: the Run dialog's
 * autocomplete dropdown is open right after typing and can swallow it.
 */
static bool screen_sig_equal(const uvc_frame_sig_t *a, const uvc_frame_sig_t *b)
{
    return (a->len_bytes == b->len_bytes) && (a->hash == b->hash);
}

/* True when the screen has NOT changed since `before` was sampled. */
static bool screen_unchanged_since(const uvc_frame_sig_t *before)
{
    uvc_frame_sig_t now = {0};
    if (uvc_capture_frame_sig(&now, 1000) != ESP_OK) {
        return false;   /* cannot tell — assume it worked, do not add an Enter */
    }
    return screen_sig_equal(before, &now);
}

/* Track last action types for repetition detection */
#define TYPE_HISTORY 15
static char s_last_types[TYPE_HISTORY][32] = {{0}};
static int s_last_type_idx = 0;

const char *action_executor_get_last_summary(void)
{
    return s_last_summary;
}

/*
 * How many Chinese/English input-mode toggles have been sent this task.
 *
 * Toggling is a PARITY the harness cannot observe: `shift` and `ctrl+space`
 * flip the SAME state in Microsoft Pinyin, so a model that tries one after the
 * other toggles twice and lands exactly where it started — which is what turned
 * one dropped digit into an ever-worsening retype loop in the field. The count
 * is published in the status line so the model has an objective reason to stop:
 * "IME toggles sent: 3" means the approach is not working, whatever the screen
 * looks like.
 */
static int s_ime_toggles = 0;

int action_executor_ime_toggle_count(void)
{
    return s_ime_toggles;
}

void action_executor_reset_ime_toggles(void)
{
    s_ime_toggles = 0;
}

/* Toggle == a key press that flips the input mode rather than typing. */
static bool is_ime_toggle_key(const char *key)
{
    static const char *const toggles[] = {
        "shift", "ctrl+space", "win+space", "alt+shift",
        "ctrl+shift", "shift+space",
    };
    for (size_t i = 0; i < sizeof(toggles) / sizeof(toggles[0]); i++) {
        if (strcmp(key, toggles[i]) == 0) return true;
    }
    return false;
}

void action_executor_emergency_release(void)
{
    ESP_LOGW(TAG, "EMERGENCY: releasing all HID");
    hid_release_all();
}

/*
 * Convert a [ymin, xmin, ymax, xmax] box to a pixel centre.
 *
 * THE ONE PLACE coordinates are unmapped. The model returns boxes normalised to
 * the image it was SHOWN; when that image is a crop, view_map_to_full() is what
 * turns "middle of the crop" back into "middle of the screen". Everything else
 * in the pipeline works in full-frame coordinates, including the trajectory,
 * so history stays readable across view changes.
 *
 * The model is told to send normalised [0,1] only, but the pixel form is still
 * tolerated. Returns false when the box is missing, malformed, or non-numeric —
 * callers must NOT fall back to (0,0), because that silently yanks the cursor
 * into the screen corner.
 */
static bool box_to_pixel(const cJSON *box, uint32_t screen_w, uint32_t screen_h,
                         uint32_t *px, uint32_t *py)
{
    if (!box || !cJSON_IsArray(box) || cJSON_GetArraySize(box) < 4) return false;

    const cJSON *i0 = cJSON_GetArrayItem(box, 0);
    const cJSON *i1 = cJSON_GetArrayItem(box, 1);
    const cJSON *i2 = cJSON_GetArrayItem(box, 2);
    const cJSON *i3 = cJSON_GetArrayItem(box, 3);
    if (!cJSON_IsNumber(i0) || !cJSON_IsNumber(i1) ||
        !cJSON_IsNumber(i2) || !cJSON_IsNumber(i3)) {
        return false;
    }

    float v0 = (float)i0->valuedouble;
    float v1 = (float)i1->valuedouble;
    float v2 = (float)i2->valuedouble;
    float v3 = (float)i3->valuedouble;

    float y_center = (v0 + v2) / 2.0f;
    float x_center = (v1 + v3) / 2.0f;

    float nx, ny;

    /* Per-axis detection: the model may mix normalized and pixel coords.
     * The prompt demands normalized [0,1] only, so this should never fire; it is
     * rate limited because a disobedient model would otherwise flood the
     * console once per action. */
    bool pixel_x = (v1 > 2.0f || v3 > 2.0f);
    bool pixel_y = (v0 > 2.0f || v2 > 2.0f);

    if (pixel_x || pixel_y) {
        static int s_pixel_warn_count = 0;
        if (s_pixel_warn_count < 3 || (s_pixel_warn_count % 20) == 0) {
            ESP_LOGW(TAG, "box [%.1f,%.1f,%.1f,%.1f] is not normalized — the "
                          "model ignored the [0,1] requirement", v0, v1, v2, v3);
        }
        s_pixel_warn_count++;

        /* Pixels can only sensibly refer to the full screen; applying the view
         * mapping to them would double-map. */
        if (!view_is_full(&s_view)) {
            ESP_LOGW(TAG, "pixel coordinates while a cropped view is active — "
                          "treating them as full-screen pixels");
        }
        nx = (screen_w > 0) ? x_center / (float)screen_w : 0.0f;
        ny = (screen_h > 0) ? y_center / (float)screen_h : 0.0f;
    } else {
        view_map_to_full(&s_view, x_center, y_center, &nx, &ny);
    }

    float x = nx * (float)screen_w;
    float y = ny * (float)screen_h;

    if (x < 0.0f) x = 0.0f;
    if (y < 0.0f) y = 0.0f;
    if (screen_w > 0 && x > (float)(screen_w - 1)) x = (float)(screen_w - 1);
    if (screen_h > 0 && y > (float)(screen_h - 1)) y = (float)(screen_h - 1);

    *px = (uint32_t)x;
    *py = (uint32_t)y;
    return true;
}

void action_executor_set_view(const view_spec_t *view)
{
    if (!view) {
        s_view = (view_spec_t)VIEW_SPEC_FULL;
        return;
    }
    view_spec_t v = *view;
    if (!view_normalize(&v)) v = (view_spec_t)VIEW_SPEC_FULL;
    s_view = v;
}

static void track_action_type(const char *atype)
{
    if (!atype) return;
    memset(s_last_types[s_last_type_idx], 0, sizeof(s_last_types[0]));
    strncpy(s_last_types[s_last_type_idx], atype,
            sizeof(s_last_types[0]) - 1);
    s_last_type_idx = (s_last_type_idx + 1) % TYPE_HISTORY;
}

static int check_repetition(void)
{
    int count = 0;
    const char *last = s_last_types[(s_last_type_idx - 1 + TYPE_HISTORY) % TYPE_HISTORY];
    if (!last[0]) return 0;
    for (int i = 0; i < TYPE_HISTORY; i++) {
        if (strcmp(s_last_types[i], last) == 0) count++;
    }
    return count;
}

int action_executor_repetition_count(void)
{
    return check_repetition();
}

const char *action_executor_last_type(void)
{
    return s_last_types[(s_last_type_idx - 1 + TYPE_HISTORY) % TYPE_HISTORY];
}

/* Append one description to the batch summary, never overflowing. */
static void append_desc(char *buf, size_t len, const char *fmt, ...)
{
    size_t used = strlen(buf);
    if (used + 2 >= len) return;
    if (used > 0) {
        buf[used++] = ' ';
        buf[used] = '\0';
    }

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + used, len - used, fmt, ap);
    va_end(ap);
    if (n < 0) buf[used] = '\0';   /* encoding error: keep what we had */

    /* vsnprintf() drops any tail that does not fit, which can split a
     * multi-byte character. This summary is echoed into the prompt, so repair
     * the cut before it can reach the API. */
    utf8_sanitize(buf);
}

/* Box centre as "0.45,0.42", or "?" when the box is unusable. */
static void box_desc(const cJSON *box, uint32_t sw, uint32_t sh,
                     char *out, size_t out_len)
{
    uint32_t x = 0, y = 0;
    if (!box_to_pixel(box, sw, sh, &x, &y)) {
        snprintf(out, out_len, "?");
        return;
    }
    snprintf(out, out_len, "%.3f,%.3f", sw ? (double)x / sw : 0.0,
             sh ? (double)y / sh : 0.0);
}

/*
 * Execute one action and describe it, parameters included, into `summary`.
 *
 * The description has to carry the parameters: the model's own history is built
 * from these lines, and "click" alone tells it nothing about why a click landed
 * wrong — it needs the coordinates it sent so it can compare them with the
 * cursor position the status line reports.
 */
static void execute_single(cJSON *action, uint32_t screen_w, uint32_t screen_h,
                           char *summary, size_t summary_len)
{
    /* Every field below is optional in practice: the model drops keys, sends
     * them with the wrong type, or sends null. Read defensively. */
    const char *atype = jget_str(action, "action_type", NULL);
    if (!atype || !atype[0]) {
        ESP_LOGW(TAG, "Action without valid action_type — skipped");
        return;
    }

    double delay_after = jget_num(action, "delay_after", 0.05);
    track_action_type(atype);
    char b[48];

    if (strcmp(atype, "click") == 0) {
        const cJSON *box = cJSON_GetObjectItem(action, "box_2d");
        const char *btn = jget_str(action, "button", "left");
        uint32_t x = 0, y = 0;
        if (box_to_pixel(box, screen_w, screen_h, &x, &y)) {
            hid_mouse_move_abs(x, y, screen_w, screen_h);
            vTaskDelay(pdMS_TO_TICKS(50));  /* let the host settle before clicking */
            box_desc(box, screen_w, screen_h, b, sizeof(b));
            append_desc(summary, summary_len, "click(%s,%s)", btn, b);
            ESP_LOGI(TAG, "click btn=%s at (%lu,%lu)", btn, x, y);
        } else {
            append_desc(summary, summary_len, "click(%s,@cursor)", btn);
            ESP_LOGI(TAG, "click btn=%s at current position", btn);
        }
        hid_mouse_click(btn);

    } else if (strcmp(atype, "move") == 0) {
        const cJSON *box = cJSON_GetObjectItem(action, "box_2d");
        uint32_t x = 0, y = 0;
        if (box_to_pixel(box, screen_w, screen_h, &x, &y)) {
            hid_mouse_move_abs(x, y, screen_w, screen_h);
            box_desc(box, screen_w, screen_h, b, sizeof(b));
            append_desc(summary, summary_len, "move(%s)", b);
            ESP_LOGI(TAG, "move to (%lu,%lu)", x, y);
        } else {
            append_desc(summary, summary_len, "move(?)");
            ESP_LOGW(TAG, "move without a valid box_2d — skipped");
        }

    } else if (strcmp(atype, "drag") == 0) {
        const cJSON *from = cJSON_GetObjectItem(action, "from_box");
        const cJSON *to = cJSON_GetObjectItem(action, "to_box");
        const char *btn = jget_str(action, "button", "left");
        int hold_ms = jget_int(action, "hold", 0);
        uint32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        bool have_from = box_to_pixel(from, screen_w, screen_h, &x1, &y1);
        bool have_to = box_to_pixel(to, screen_w, screen_h, &x2, &y2);

        if (!have_from || !have_to) {
            append_desc(summary, summary_len, "drag(?)");
            ESP_LOGW(TAG, "drag needs both from_box and to_box — skipped");
        } else {
            char b1[48], b2[48];
            box_desc(from, screen_w, screen_h, b1, sizeof(b1));
            box_desc(to, screen_w, screen_h, b2, sizeof(b2));
            append_desc(summary, summary_len, "drag(%s->%s)", b1, b2);

            hid_mouse_move_abs(x1, y1, screen_w, screen_h);
            vTaskDelay(pdMS_TO_TICKS(20));
            hid_mouse_down(btn);

            /* Interpolate in 8 steps */
            for (int step = 1; step <= 8; step++) {
                uint32_t ix = x1 + ((int32_t)(x2 - x1) * step) / 8;
                uint32_t iy = y1 + ((int32_t)(y2 - y1) * step) / 8;
                hid_mouse_move_abs(ix, iy, screen_w, screen_h);
                vTaskDelay(pdMS_TO_TICKS(5));
            }

            if (hold_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(hold_ms));
            }
            hid_mouse_up(btn);
            ESP_LOGI(TAG, "drag from (%lu,%lu) to (%lu,%lu) hold=%dms",
                     x1, y1, x2, y2, hold_ms);
        }

    } else if (strcmp(atype, "scroll") == 0) {
        int dx = jget_int(action, "delta_x", 0);
        int dy = jget_int(action, "delta_y", 0);
        hid_mouse_scroll((int8_t)dx, (int8_t)dy);
        append_desc(summary, summary_len, "scroll(dx=%d,dy=%d)", dx, dy);
        ESP_LOGI(TAG, "scroll dx=%d dy=%d", dx, dy);

    } else if (strcmp(atype, "press") == 0 || strcmp(atype, "hotkey") == 0) {
        const char *key = jget_str(action, "key", NULL);
        if (key && key[0]) {
            hid_key_press(key);
            if (is_ime_toggle_key(key)) {
                s_ime_toggles++;
                /*
                 * Let the target's input method actually apply the mode change
                 * before any keystroke that follows in the SAME batch.
                 *
                 * The switch lands asynchronously on the host, so a probe typed
                 * immediately after the toggle can be sent before the switch
                 * takes effect — which makes a WORKING toggle look broken. In
                 * the field that produced three "failed" toggles in a row, the
                 * next multi-word type with no toggle at all came through with
                 * its spaces intact, i.e. the mode had been English all along.
                 * The prompt now forbids probing in the same step; this delay
                 * covers any other action that happens to follow one.
                 */
                vTaskDelay(pdMS_TO_TICKS(150));
            }
            append_desc(summary, summary_len, "press \"%.32s\"", key);
            ESP_LOGI(TAG, "press %s", key);
        } else {
            append_desc(summary, summary_len, "press(?)");
            ESP_LOGW(TAG, "press without key — skipped");
        }

    } else if (strcmp(atype, "run") == 0) {
        /* Atomic "launch something through the Run dialog".
         *
         * As one action instead of a model-orchestrated Win+R / type / Enter
         * sequence this removes several round trips, and — more importantly —
         * removes the chance to botch the recovery. In the field the agent
         * repeatedly got stuck in "dismiss the error dialog, select-all,
         * retype" loops that each cost a full model round trip.
         *
         * This does NOT avoid the IME: the Run field is a normal text control.
         * See the prompt for the input-method rules. */
        const char *cmd = jget_str(action, "command", NULL);
        if (!cmd || !cmd[0]) {
            append_desc(summary, summary_len, "run(?)");
            ESP_LOGW(TAG, "run without command — skipped");
        } else {
            bool clear = jget_bool(action, "clear_first", true);
            double wait_after = jget_num(action, "wait_after", 1.5);
            if (wait_after < 0.2) wait_after = 0.2;
            if (wait_after > 30.0) wait_after = 30.0;

            append_desc(summary, summary_len, "run \"%.40s\"", cmd);

            hid_key_press("win+r");
            vTaskDelay(pdMS_TO_TICKS(s_run_dialog_ms));
            if (clear) {
                /* The box often still holds the previous, wrong command. */
                hid_key_press("ctrl+a");
                vTaskDelay(pdMS_TO_TICKS(80));
            }
            hid_type_string(cmd);
            vTaskDelay(pdMS_TO_TICKS(s_run_settle_ms));

            /* Sample the screen with the command typed but not yet submitted. */
            uvc_frame_sig_t before = {0};
            bool have_before = (uvc_capture_frame_sig(&before, 1000) == ESP_OK);

            hid_key_press("enter");
            vTaskDelay(pdMS_TO_TICKS(s_run_verify_ms));

            if (have_before && screen_unchanged_since(&before)) {
                /* Nothing moved: the dialog is still sitting there. Either the
                 * Enter was eaten by the autocomplete dropdown or the report
                 * was lost. One bounded retry. */
                ESP_LOGW(TAG, "run: screen unchanged after Enter — retrying once");
                hid_key_press("enter");
                vTaskDelay(pdMS_TO_TICKS(s_run_verify_ms));
                if (screen_unchanged_since(&before)) {
                    ESP_LOGE(TAG, "run: \"%s\" still produced no screen change — "
                                  "the command may not have executed", cmd);
                    append_desc(summary, summary_len, "[NO SCREEN CHANGE]");
                }
            }

            vTaskDelay(pdMS_TO_TICKS((uint32_t)(wait_after * 1000.0)));
            ESP_LOGI(TAG, "run \"%s\" (clear=%d, wait=%.1fs)",
                     cmd, (int)clear, wait_after);
        }

    } else if (strcmp(atype, "type") == 0) {
        const char *text = jget_str(action, "text", NULL);
        if (text && text[0]) {
            /*
             * Two different replacement scopes, because they are not
             * interchangeable:
             *
             *   clear_first    -> ctrl+a, which selects the ENTIRE field. Right
             *                     on a fresh document, destructive on a field
             *                     that already holds work the agent typed
             *                     earlier in the same task.
             *   replace_chars  -> shift+left N times, which selects exactly the
             *                     N characters just inserted. Exact and local,
             *                     and N is knowable because the application
             *                     reports its own character count.
             *
             * The harness used to offer only the first, so recovering from a
             * failed `type` in the middle of a document meant wiping the
             * document. When both are supplied the exact one wins.
             */
            int replace_n = jget_int(action, "replace_chars", 0);
            if (replace_n < 0) replace_n = 0;
            if (replace_n > 200) replace_n = 200;   /* bounded: it is N keystrokes */

            bool clear = jget_bool(action, "clear_first", false) && replace_n == 0;
            if (clear) {
                hid_key_press("ctrl+a");
                vTaskDelay(pdMS_TO_TICKS(80));
            }

            for (int i = 0; i < replace_n; i++) {
                hid_key_press("shift+left");
                vTaskDelay(pdMS_TO_TICKS(15));
            }
            /*
             * Optional keypad path for the digits.
             *
             * A Chinese IME in composition mode consumes the main number row as
             * candidate selectors, which is how "Vicrl-7391-A" arrives as
             * "Vicrl-A". The keypad is usually passed through. Which channel
             * works is a property of the target machine, so the model chooses
             * per action — but the device refuses to use the keypad unless
             * Num Lock is confirmed ON from the host's LED report, because an
             * unlit keypad emits arrows and Home/End, which is far more
             * destructive than a dropped digit.
             */
            bool kp = jget_bool(action, "digits_numpad", false);
            if (kp && !hid_ensure_num_lock()) {
                ESP_LOGW(TAG, "digits_numpad requested but Num Lock is not "
                              "confirmed — using the main number row");
                append_desc(summary, summary_len, "[keypad unavailable]");
                kp = false;
            }
            hid_type_set_digits_on_keypad(kp);
            hid_type_result_t r = hid_type_string(text);
            hid_type_set_digits_on_keypad(false);

            /* Record the CHANNEL, not just the text.
             *
             * In the field, a log line reading `type "Vicrl-7391-A" clear_first`
             * was impossible to interpret: the digits were missing, but nothing
             * in the record said whether the keypad path had been used, so the
             * evidence could not distinguish "the keypad is also eaten" from
             * "the keypad was never tried". An experiment that does not record
             * which variant it ran is not an experiment. */
            ESP_LOGI(TAG, "type: %s (channel=%s leds=0x%02X)",
                     text, kp ? "keypad-digits" : "main-row-numrow",
                     (unsigned)hid_keyboard_led_state());

            /* Echo at most 127 bytes of what was typed, cut on a character
             * boundary: s_last_type_report goes into the status line verbatim,
             * and a half-copied Chinese character there would make the whole
             * request body invalid UTF-8. */
            char shown[128];
            utf8_copy(shown, sizeof(shown), text);
            /* Record what the device actually put on the wire, so the model can
             * tell "the device failed" apart from "the target swallowed it". */
            if (r.queued == r.len) {
                snprintf(s_last_type_report, sizeof(s_last_type_report),
                         "\"%s\" - device queued ALL %u/%u key events, so any "
                         "missing/different text on screen was caused by the "
                         "target machine",
                         shown, (unsigned)r.queued, (unsigned)r.len);
            } else {
                snprintf(s_last_type_report, sizeof(s_last_type_report),
                         "\"%s\" - device queued only %u/%u key events "
                         "(skipped %u with no ASCII key mapping)",
                         shown, (unsigned)r.queued, (unsigned)r.len,
                         (unsigned)r.skipped);
            }
            append_desc(summary, summary_len, "type \"%s\"%s%s%s",
                        shown,
                        clear ? " clear_first" : "",
                        replace_n ? " replace" : "",
                        kp ? " keypad" : "");
            if (replace_n) {
                append_desc(summary, summary_len, "(%d)", replace_n);
            }
        } else {
            append_desc(summary, summary_len, "type(?)");
            ESP_LOGW(TAG, "type without text — skipped");
        }

    } else if (strcmp(atype, "wait") == 0) {
        double wait_s = jget_num(action, "wait_seconds", 0.0);
        if (wait_s < 0.0) wait_s = 0.0;
        if (wait_s > 60.0) wait_s = 60.0;   /* guard against a bogus huge wait */
        vTaskDelay(pdMS_TO_TICKS((uint32_t)(wait_s * 1000)));
        append_desc(summary, summary_len, "wait %.1fs", wait_s);
        ESP_LOGI(TAG, "wait %.2fs", wait_s);

    } else if (strcmp(atype, "release") == 0) {
        const char *btn = jget_str(action, "button", "left");
        hid_mouse_up(btn);
        append_desc(summary, summary_len, "release(%s)", btn);
        ESP_LOGI(TAG, "release %s", btn);

    } else if (strcmp(atype, "complete") == 0) {
        append_desc(summary, summary_len, "complete");
        ESP_LOGI(TAG, "complete");

    } else if (strcmp(atype, "error") == 0) {
        const char *msg = jget_str(action, "message", "unknown");
        append_desc(summary, summary_len, "error(\"%.40s\")", msg);
        ESP_LOGW(TAG, "error: %s", msg);

    } else {
        append_desc(summary, summary_len, "unknown(%s)", atype);
        ESP_LOGW(TAG, "Unknown action_type '%s' — ignored", atype);
    }

    uint32_t delay_ms = (uint32_t)(delay_after * 1000);
    if (delay_ms > 10000) delay_ms = 10000;  /* cap a runaway delay_after */
    if (delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

esp_err_t action_executor_execute_all(cJSON *actions,
                                       uint32_t screen_w, uint32_t screen_h)
{
    if (!actions || !cJSON_IsArray(actions)) return ESP_ERR_INVALID_ARG;

    int count = cJSON_GetArraySize(actions);
    ESP_LOGI(TAG, "Executing %d actions", count);

    /* The descriptions carry parameters and become part of the model's own
     * trajectory — see execute_single(). */
    char summary_buf[sizeof(s_last_summary)] = {0};

    cJSON *action = NULL;
    cJSON_ArrayForEach(action, actions) {
        if (!cJSON_IsObject(action)) continue;
        execute_single(action, screen_w, screen_h, summary_buf, sizeof(summary_buf));
    }

    snprintf(s_last_summary, sizeof(s_last_summary), "%s",
             summary_buf[0] ? summary_buf : "(none)");
    ESP_LOGI(TAG, "Executed %d: %s", count, s_last_summary);

    /* Warn on repetition */
    int reps = check_repetition();
    if (reps >= 6) {
        ESP_LOGW(TAG, "Action repetition detected: %d similar actions", reps);
    }

    return ESP_OK;
}
