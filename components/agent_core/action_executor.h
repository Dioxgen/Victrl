#pragma once

#include "cJSON.h"
#include "esp_err.h"
#include "jpeg_utils.h"   /* view_spec_t */

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t action_executor_execute_all(cJSON *actions,
                                       uint32_t screen_w, uint32_t screen_h);
const char *action_executor_get_last_summary(void);
void action_executor_emergency_release(void);

/*
 * Repetition detection, for the status bar.
 *
 * Counts how many of the last TYPE_HISTORY executed actions share the most
 * recent action's type, and names that type. Surfacing this to the model is
 * what makes it notice it is stuck: "click x6" is a far stronger hint to change
 * approach than the same information buried in a log line the model never sees.
 */
int action_executor_repetition_count(void);
const char *action_executor_last_type(void);

/*
 * Timing for the `run` action (Win+R → command → Enter), in milliseconds.
 * `dialog_ms` is how long the Run dialog needs before it accepts input;
 * `settle_ms` is the pause after typing and before pressing Enter.
 * Both depend on the target machine, so they come from config.
 */
void action_executor_set_run_timing(uint32_t dialog_ms, uint32_t settle_ms);

/*
 * Declare the field of view the model's current coordinates are relative to.
 *
 * The model returns boxes normalised to the image it was shown; when that image
 * is a crop, every coordinate must be mapped back to the full frame before it
 * can drive the mouse. Setting the view here (rather than threading it through
 * every action) keeps the mapping in exactly one place: box_to_pixel().
 *
 * Pass NULL (or a full-frame view) to reset.
 */
void action_executor_set_view(const view_spec_t *view);

/*
 * One-line report on the most recent `type`: the exact string sent and how many
 * key events the device actually queued.
 *
 * This is the decisive evidence for the input-method problem. When it says the
 * device queued every keystroke but the screen shows something else, the text
 * was lost on the target side and the IME is the prime suspect — no point
 * retrying the same way.
 *
 * Returns "none" before the first type action.
 */
const char *action_executor_get_last_type_report(void);

/*
 * Number of Chinese/English input-mode toggles sent so far this task.
 *
 * Toggling is a parity nobody on this side can observe, and in Microsoft Pinyin
 * `shift` and `ctrl+space` flip the SAME state — so trying one after the other
 * cancels out. Publishing the count gives the model an objective stop signal:
 * once it is 2 or more, more toggling cannot converge and the approach has to
 * change.
 */
int action_executor_ime_toggle_count(void);

/* Zero the counter. Called when a task starts. */
void action_executor_reset_ime_toggles(void);

#ifdef __cplusplus
}
#endif
