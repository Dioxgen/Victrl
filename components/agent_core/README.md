# agent_core — Agent Core

> **English** | [中文](README_CN.md)

## Overview

Agent state machine + main loop + action executor: the scheduling core of the device. It captures the screen, assembles the context, calls the LLM, executes HID actions, and writes the results back to the session trajectory and the task log.

## Files

| File | Responsibility |
|------|------|
| `agent_core.c` / `agent_core.h` | State machine, main loop, context and status-line assembly, sessions/trajectory/log, adaptive reasoning effort |
| `action_executor.c` / `action_executor.h` | Action execution (mouse/keyboard), coordinate un-mapping, repetition detection, IME channel and failure evidence |

## State machine

States are `IDLE` / `RUNNING` / `PAUSED` / `STEP` / `STOPPING` / `EMERGENCY`. The event bits `AGENT_EVT_RUN` / `AGENT_EVT_STEP` / `AGENT_EVT_EMERGENCY` / `AGENT_EVT_STOP` travel through a FreeRTOS EventGroup, and `state_mutex` guards state reads and writes.

- `agent_start` / `agent_start_session` are only accepted from `IDLE`; `agent_pause` only from `RUNNING`; `agent_resume` / `agent_step_once` only from `PAUSED`.
- The main loop treats `STEP` exactly like `RUNNING` as "keep going" (`agent_should_continue()`); `agent_step_once()` is only accepted from `PAUSED`.
- `agent_stop` or reaching `max_actions` closes the log and returns to `IDLE`; `agent_emergency` releases all HID immediately and returns to `IDLE`.
- Every failure path (capture failure, LLM failure) returns to `IDLE` instead of exiting the main-loop task — the old implementation deleted that task permanently, after which only a power cycle could recover it.

## Main loop

1. While not `RUNNING` / `STEP`, block on the event bits.
2. Every step captures a frame with `uvc_capture_one_frame_sig()` (5000ms timeout); 3 consecutive failures return to `IDLE`.
3. The frame fingerprint (length + hash) is compared with the previous frame to get an objective "did the screen change" answer; when the fingerprint and the view both match, the already-encoded payload is reused, skipping compression and base64.
4. System prompt selection: with `system_prompt_interval == 0` (the default) the full prompt is always sent to preserve the API prefix cache, otherwise it rotates by step count.
5. The user text is assembled from: goal, status line, device profile, previous action summary, current plan, session trajectory.
6. The reasoning budget is chosen, then `cloud_client_query()` sends the screenshot plus context.
7. The task log is written (with the per-step latency breakdown) and the timing is published to the WebUI.
8. `actions` / `done` / `need_screen` / `sleep_before_next` are parsed and the actions executed; when the whole batch is `wait` and `need_screen:false`, the agent enters wait mode.
9. `plan_update` is saved, `profile_updates` are appended, `request_profile` is handled, the turn is appended to the session trajectory, and `next_view` is adopted.
10. On `done:true` the end state is recorded, the log closed and `IDLE` restored; otherwise the loop sleeps `sleep_before_next` (floor 1s, cap 300s) and continues.

## Sessions and trajectory

- `agent_start` creates a new session (`session_mgr_create`); `agent_start_session` continues an existing one, restoring its goal, plan, trajectory and step count.
- The session directory is `/sdcard/sessions/<id>/` holding `meta.json`, `plan.json` and `trajectory.txt`; the device profile describes the machine, not the task, and is shared across sessions.
- The trajectory is the model's memory: one `#step [time] look=<full|ROI(...)>` record per turn, followed by `action:` / `saw:` / `result:` / `plan:`; coordinates are always full-frame normalized and `look=` states which region that step actually saw.
- Task logs live in `/sdcard/log/<session_id>/`; raw model responses go to the log only and are never fed back into the context.

## Public API

| Interface | Description |
|------|------|
| `agent_init(ctx, max_actions, capture_w, capture_h, output_w, output_h, prompt_interval, dry_run)` | Initialize the context; `dry_run` is currently only stored in the ctx and unused by the loop |
| `agent_start(ctx, task_goal)` | Create a session and start |
| `agent_start_session(ctx, session_id)` | Continue an existing session |
| `agent_pause` / `agent_resume` / `agent_step_once` / `agent_stop` / `agent_emergency` | State control |
| `agent_get_state` / `agent_get_global` | Read the state / fetch the global context |
| `agent_set_wait_skip_policy(ctx, allow, max_blind_rounds, wait_settle_ms)` | Idle-query skip policy (off by default) |
| `agent_set_effort_policy(ctx, normal, escalated, escalate_after)` | Adaptive reasoning effort |
| `agent_set_active_profile(ctx, profile_id)` | Set the destination of `profile_updates` (must be called) |
| `agent_main_loop_task(pv)` | Main-loop task body |
| `action_executor_execute_all(actions, screen_w, screen_h)` | Execute an action batch and build the summary |
| `action_executor_set_view(view)` / `action_executor_set_run_timing(dialog_ms, settle_ms)` | Declare the coordinate view / configure `run` timing |
| `action_executor_get_last_summary()` / `action_executor_get_last_type_report()` | Previous action summary / device-side evidence for the last `type` |
| `action_executor_repetition_count()` / `action_executor_last_type()` / `action_executor_ime_toggle_count()` / `action_executor_reset_ime_toggles()` | Repetition detection and IME-toggle counting |
| `action_executor_emergency_release()` | Release all HID |

## Action types

| `action_type` | Key parameters | Behaviour |
|------|------|------|
| `click` / `move` | `box_2d`, `button` | With `box_2d`, move first then click (50ms settle after the move); `click` without `box_2d` clicks in place, `move` without `box_2d` is skipped with a warning |
| `drag` | `from_box`, `to_box`, `button`, `hold` | Move, wait 20ms, press, interpolate in 8 steps of 5ms each, optionally hold for `hold` ms, then release |
| `scroll` | `delta_x`, `delta_y` | Wheel scroll |
| `press` / `hotkey` | `key` | Single key or combination; IME-toggle keys are counted and followed by a 150ms wait |
| `run` | `command`, `clear_first`, `wait_after` | Win+R, wait for the dialog, optional ctrl+a, type, wait for settle, sample the fingerprint, Enter, verify after 700ms; sends one extra Enter if nothing changed |
| `type` | `text`, `clear_first`, `replace_chars`, `digits_numpad` | `replace_chars` sends shift+left N times (≤200) and is mutually exclusive with `clear_first`; `digits_numpad` requires Num Lock to be confirmed |
| `wait` / `release` | `wait_seconds` / `button` | Wait (≤60s) / release a button |
| `complete` / `error` / anything else | `message` | Recording only; unknown types are ignored with a warning |

Every action is additionally followed by `delay_after` (default 0.05s, cap 10s).

## Coordinates, view and status line

- `box_2d` is `[ymin, xmin, ymax, xmax]`, normalized to the image the model was actually shown; normalized coordinates first go through `view_map_to_full()` to map the ROI back to the full frame and are then scaled by the resolution — that is the single un-mapping point.
- Pixel coordinates are still tolerated, but the whole box is judged together: if any coordinate is > 2.0, both axes are treated as full-screen pixels with a rate-limited warning (no per-axis mixing), and the ROI mapping is not applied at all.
- `next_view` is sticky: `roi` (`[ymin,xmin,ymax,xmax]`) and `scale` (1/2/4) apply to the next screenshot and stay in force until the model changes them, omitting it means "unchanged", and the empty object `{}` means "back to the full frame"; a ROI whose extent on either axis is below `VIEW_MIN_EXTENT` (0.02) is ignored.
- Every step's user text carries a code-maintained `**Status:**` line: view, `step n/max`, cursor in normalized and pixel form, held mouse buttons, whether the screen changed, consecutive rounds without a new screenshot, how many of the last 15 actions share the last type, IME toggles sent, Num/Caps Lock and the device-side evidence for the last `type`.
- The reasoning budget starts at `low` and escalates to `high` while stuck, where stuck means the previous step produced no visible screen change or ≥ 4 of the last 15 actions share the same type; any visible progress drops it back immediately.

## Notes

- The main-loop task is created in `main.c` with a 16384-byte stack (the HTTP client and cJSON are heavy consumers).
- `need_screen` no longer gates the capture, and the code no longer forces it to `true` after `click`/`press`/`type`/`drag` (the system prompt now asks the model to do that); it only decides whether the API call may be skipped while the whole batch is `wait` and the model asked to be left alone — and that needs `allow_wait_skip` enabled, an unchanged fingerprint and no more than `max_blind_rounds` (default 3, `wait_settle_ms` default 1000ms).
- Reusing the encoded image requires the same frame fingerprint **and** the same view (`cloud_client_can_reuse_image`); otherwise it would be a crop of the previous frame.
- Repetition detection is a count, not a streak: it counts how many of the last 15 action records (`TYPE_HISTORY`) share the most recent type — ≥6 logs a repetition warning, ≥4 counts as stuck and triggers the effort escalation.
- `run` timing comes from config (`run_dialog_ms` / `run_settle_ms`, pushed via `action_executor_set_run_timing()`); after Enter it always waits 700ms and verifies the screen via the fingerprint, marking the summary `[NO SCREEN CHANGE]` if nothing ever moved.
- `digits_numpad` is refused and falls back to the main number row unless Num Lock is confirmed from the host's LED report, and the summary is marked `[keypad unavailable]`; the IME toggle keys are `shift`, `ctrl+space`, `win+space`, `alt+shift`, `ctrl+shift` and `shift+space`, and the counter resets per task.
- `agent_set_active_profile()` must be called: `agent_init()` zeroes the context, and with an empty active profile `profile_updates` get appended to a file literally named `.md`; note also that the profile sent to the model every step is the configured default profile, while `request_profile` only changes where `profile_updates` are written.
- Short-term memory (`stm_add()`) is read only by the WebUI and the display task and never enters the prompt; the model's context carries the session trajectory instead.
- Status-line and trajectory text is passed through `utf8_sanitize()` after truncation so that half a multi-byte character cannot make the request body invalid.
