#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Sessions: one "conversation window" per task.
 *
 * Layout on the SD card:
 *
 *   /sdcard/sessions/<id>/meta.json        title, goal, created, steps
 *   /sdcard/sessions/<id>/plan.json        plan + milestones for this task
 *   /sdcard/sessions/<id>/trajectory.txt   per-turn record (the model's context)
 *
 * A session owns the task goal, the plan and the trajectory. The device profile
 * is deliberately NOT part of a session: it describes the MACHINE, not the task,
 * and is shared by every session.
 *
 * Why there is no per-turn screenshot history: for a GUI agent the previous
 * frame is information-free (the screen has already changed), while re-sending
 * it costs ~160KB of upload per past turn — a 20-step task would push 3.2MB on
 * every request. The trajectory is text, so it stays cheap, and the only image
 * in a request is the current screen.
 */

#define SESSION_ID_LEN     24
#define SESSION_TITLE_LEN  64
/* Hard cap on the task statement, in BYTES.
 *
 * The goal is user text and is routinely Chinese (~3 bytes per character), and
 * a real task statement lists several steps: a 192-byte cap cut a normal
 * sentence mid-character, and the 256-byte cap that replaced it still silently
 * dropped whole steps — after which the agent confidently completed a shorter
 * task than the user asked for. 1024 bytes is ~340 Chinese characters.
 *
 * Callers must REJECT input that does not fit rather than truncate it; a
 * silently shortened goal is the most damaging failure this project has had. */
#define SESSION_GOAL_LEN   1024

/* Trajectory cap. Older turns are trimmed from the front once exceeded; the
 * text is cheap (it caches), so this is a generous bound rather than a budget. */
#define SESSION_TRAJ_MAX   24576
#define SESSION_TRAJ_KEEP  16384

typedef struct {
    char     id[SESSION_ID_LEN];
    char     title[SESSION_TITLE_LEN];
    char     goal[SESSION_GOAL_LEN];
    char     created[24];
    uint32_t steps;
    uint32_t traj_bytes;
} session_info_t;

esp_err_t session_mgr_init(const char *base_dir);

/* Enumerate sessions, newest first. Returns how many were written (<= max). */
int session_mgr_list(session_info_t *out, int max);

/* Create a new session for `goal` and make it current. Returns its id. */
esp_err_t session_mgr_create(const char *goal, char id_out[SESSION_ID_LEN]);

/* Make an existing session current (loads its meta and plan). */
esp_err_t session_mgr_load(const char *id);

const char *session_mgr_current_id(void);
const char *session_mgr_current_goal(void);
uint32_t session_mgr_current_steps(void);

/* Persist the step counter into the current session's meta. */
esp_err_t session_mgr_set_steps(uint32_t steps);

/* Append one turn record (multi-line, no trailing newline needed). */
esp_err_t session_mgr_append_turn(const char *record);

/* Whole trajectory of the current session (malloc'd; caller frees). */
char *session_mgr_get_trajectory(void);

/* Overwrite the current session's trajectory (used by the WebUI editor). */
esp_err_t session_mgr_write_trajectory(const char *text);

/* Absolute path of the current session's directory ("" when none). */
const char *session_mgr_current_dir(void);

esp_err_t session_mgr_delete(const char *id);
esp_err_t session_mgr_rename(const char *id, const char *title);

#ifdef __cplusplus
}
#endif
