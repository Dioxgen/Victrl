# storage_manager — Storage Management

> **English** | [中文](README_CN.md)

## Overview

The SD-card data layer: config loading, device profiles, sessions (conversation windows), plans, short-term memory and the task log. State falls into two classes — profiles and short-term memory live in RAM, while sessions, plans, trajectories and logs live on the SD card. One task corresponds to one session, and the task goal, plan (`plan.json`) and trajectory (`trajectory.txt`) all live in that session's own directory; the device profile describes the machine rather than the task, so it does not change with the session.

## Public API

| Module | Interface | Purpose |
|--------|-----------|---------|
| Config | `config_load()` | Fill in defaults, then override `runtime_config_t` from the `api` / `agent` / `hid` / `uvc` / `paths` / `http` / `wifi` sections |
| Profile | `profile_mgr_init()`, `profile_mgr_get()`, `profile_mgr_get_default()`, `profile_mgr_append()` | Init, read (single-entry cache) and de-duplicating append for `<profile_dir>/<id>.md` |
| Plan | `plan_mgr_init()`, `plan_mgr_load()`, `plan_mgr_save()`, `plan_mgr_get_current()` | Point at the session directory, read/merge/write `plan.json`, return a copy of the current plan |
| Plan | `plan_mgr_new_task()`, `plan_mgr_new_task_with_id()` | Reset to a fresh plan holding one starter milestone |
| Session | `session_mgr_init()`, `session_mgr_current_dir()` | Set and create the base directory; return the current session directory |
| Session | `session_mgr_create()`, `session_mgr_load()` | Create a session or switch the current one; both re-point the plan manager at that session directory |
| Session | `session_mgr_list()` | Enumerate sessions, newest first (the caller supplies the limit) |
| Session | `session_mgr_current_id()`, `session_mgr_current_goal()`, `session_mgr_current_steps()` | The current session's id, goal and step count |
| Session | `session_mgr_set_steps()` | Write the step count back to the current session's `meta.json` |
| Session | `session_mgr_append_turn()`, `session_mgr_get_trajectory()`, `session_mgr_write_trajectory()` | Append to, read whole, or overwrite the trajectory |
| Session | `session_mgr_delete()`, `session_mgr_rename()` | Delete a session (three files plus the directory) or rewrite its title |
| Task log | `task_log_init()`, `task_log_close()` | Open/close `<log_dir>/<task_id>/<task_id>.log` |
| Task log | `task_log_write_step()` | Write one step's prompt type, byte counts, timings, action summary, observation, self-evaluation, reasoning, plan and raw response |
| Task log | `task_log_write_timing()` | Write that step's latency breakdown, also emitted on one `ESP_LOGI` line |
| Task log | `task_log_write_note()` | Append free-form text (the end state), then `fsync()` |
| Task log | `task_log_save_frame()` | Save that step's screenshot; only called when `api.save_frames` is true |
| STM | `stm_init()`, `stm_add()`, `stm_get_all_formatted()`, `stm_clear()` | Fixed-length short-term memory |
| UTF-8 | `utf8_seq_len()`, `utf8_copy()`, `utf8_sanitize()` | The inline helpers in `utf8_util.h` |

## Sub-modules

| File | Purpose |
|------|---------|
| `config_loader.c` | Read runtime config from `/sdcard/config.json`; fall back silently to built-in defaults when missing or invalid |
| `profile_manager.c` | Read, cache and de-duplicating append for multi-device profiles (`.md`) |
| `plan_manager.c` | Merge, load and save the in-memory plan object and `<dir>/plan.json` |
| `session_manager.c` / `.h` | Sessions (conversation windows): directory, `meta.json`, trajectory and step count |
| `short_term_mem.c` | Fixed-length short-term memory that merges the two oldest entries when full |
| `task_log.c` / `.h` | Task log, latency breakdown, end state and optional screenshots |
| `utf8_util.h` | UTF-8 safe truncation and invalid-byte scrubbing (header-only, all `static inline`) |
| `storage_manager.h` | Aggregates the public declarations and `runtime_config_t`, and includes `session_manager.h` and `task_log.h` |

## SD-card layout

| Path | Contents |
|------|----------|
| `/sdcard/config.json` | Runtime configuration |
| `/sdcard/sysprompt/full.txt`, `short.txt` | Full and shortened system prompts (`paths.sysprompt_dir`) |
| `/sdcard/profiles/<id>.md` | Device profile, `win11_laptop.md` by default |
| `/sdcard/sessions/<id>/meta.json` | Session metadata: `id`, `title`, `goal`, `created`, `steps` |
| `/sdcard/sessions/<id>/plan.json` | That session's plan and milestones |
| `/sdcard/sessions/<id>/trajectory.txt` | That session's per-turn trajectory — the model's context |
| `/sdcard/plans/` | The old flat plan directory; `paths.plan_dir` is still configurable but is no longer written |
| `/sdcard/log/<task_id>/<task_id>.log` | Task log |
| `/sdcard/log/<task_id>/<task_id>_stepNNNN.jpg` | Per-step screenshots, produced only when `api.save_frames` is true |
| `/sdcard/web/index.html` | WebUI (`paths.web_dir`) |

## Sessions (conversation windows)

A session id is generated from local time as `%Y%m%d_%H%M%S`, with an `_NN` suffix on a collision within the same second (up to 100 attempts); ids accept only `[0-9A-Za-z_-]`, which is also the path-traversal guard. The directory and size limits are:

| Constant | Value | Meaning |
|----------|-------|---------|
| `SESSION_ID_LEN` | 24 | id buffer length; a valid id must be shorter. `session_mgr_init()` also requires `session_dir` length + 1 + 24 <= 64 |
| `SESSION_TITLE_LEN` | 64 | The `title` in `meta.json`, cut from the goal text by `utf8_copy()` |
| `SESSION_GOAL_LEN` | 1024 | Task goal limit in bytes (about 340 Chinese characters) |
| `SESSION_TRAJ_MAX` | 24576 | Trajectory limit; trimming is triggered once an append sees this size |
| `SESSION_TRAJ_KEEP` | 16384 | Bytes of tail kept after trimming |

Trimming happens just before an append: once the file has reached `SESSION_TRAJ_MAX`, the front is dropped in one batch and only the last ~`SESSION_TRAJ_KEEP` bytes are kept, advanced forward to a line boundary at a line starting with `#`, and a `[earlier steps trimmed]` line is inserted at the top. If no such line boundary exists it makes a hard byte cut. Each trajectory record is assembled by `agent_core` as `#N [HH:MM:SS] look=... | action: ...` plus indented `saw:` / `result:` / `plan:` lines, with every text field pre-truncated by `utf8_copy()`.

Creating a session (`session_mgr_create()`) does `mkdir` on the session directory, writes `meta.json`, then points the plan manager at that directory and writes the initial plan; switching sessions (`session_mgr_load()`) likewise re-points it and loads that session's `plan.json`. Listing sorts by id descending (the id is a timestamp, so newest comes first).

## Plan merging (`plan.json`)

`plan_mgr_save()` rewrites the whole `plan.json` every time, printed with `cJSON_PrintUnformatted()`, because this JSON is re-sent to the model on every step and indentation is pure token waste. Merge rules: cJSON's `Add*ToObject` appends rather than replaces, so an existing key must be removed before writing, and since `cJSON_DeleteItemFromObject()` deletes only the first match it has to be looped; `milestones` is replaced wholesale only when the model actually sends an array, and a missing `milestones` means "the plan is unchanged", not "delete the plan"; `current_milestone` is tracked independently of that array — the model may omit `milestones` and only advance the pointer — and it is written both at the top level (the LCD reads the top level) and inside `plan_update`; the nested `plan_update` keeps only the `summary` and `current_milestone` that have no top-level home, and is omitted entirely when it has neither, so that the same milestone array never appears twice in the file. When `s_task_id` is empty only the in-memory object is updated, no file is written, and `ESP_OK` is returned.

## Task log

Every task (or every continuation of an existing session) opens a log at `<log_dir>/<task_id>/<task_id>.log`, opened in append mode, so a continuation keeps writing the same file. The task name and timestamps are written at the start and at the end:

```
=== Victrl Task Log ===
Task: <task_id>
Started: YYYY-MM-DD HH:MM:SS
========================

--- Step N [HH:MM:SS] ---
Prompt: full|short | Req: <n>B | Resp: <n>B | API: <n>ms | Exec: <n>ms
Actions: ...
Observation: ...
Self-evaluation: ...
Reasoning: ...        (only when non-empty)
Plan: ...             (only when non-empty)
Response: ...         (only when non-empty)

Timing(ms): total=... capture=... prep=... (jpeg=... setup=... dec=... rs=... enc=... b64=... json=...) connect=... http=... parse=... sdlog=... exec=... sleep=...
```

Timestamps come from `localtime()`, and `wifi_manager` sets `TZ=CST-8`, so once SNTP has synced they are Beijing time. The `Timing(ms):` line breaks a step's wall-clock time into capture, prep (with the JPEG and base64 sub-items), connect, HTTP, parse, log write, action execution and sleep; it is also emitted via `ESP_LOGI`, so the breakdown is readable without pulling the card. Note that `task_log_write_step()`'s `exec_time_ms` is currently always passed as 0 by the caller, and the real HID execution time is the `exec=` field of the `Timing(ms):` line. At the end of a task `task_log_write_note()` appends the end state (pointer position, view, why the run stopped), and `task_log_close()` adds the `Ended:` line and performs a final `fsync()`.

## Short-term memory

- A fixed-length `char *` array whose length comes from `agent.history_max_len` in `config.json` (10 by default), `calloc()`ed in `stm_init()`.
- Once full, each `stm_add()` merges the two oldest entries into one with `"; "` and shifts the rest down, bringing the count back below the limit.
- The readers are the WebUI's `/api/log` and the LCD display task; the writer is the agent main loop.
- All operations are protected by the mutex created in `stm_init()`; `stm_get_all_formatted()` joins the entries with newlines and returns `No history yet.` when empty.

## Notes and caveats

- The task log is buffered: `_IOFBF` with a 4096-byte static buffer, `fflush()`ed on every step; `fsync()` runs only in `task_log_init()`, `task_log_write_note()`, `task_log_close()` and every 10 steps (`TASK_LOG_SYNC_INTERVAL`), so a power cut can lose the tail of the log.
- `task_log_close()` is called on all four exit paths — done, stop, `max_actions` and emergency (the call sites are in `agent_core.c`) — so the file is never left open.
- Session files (`meta.json`, `plan.json`, `trajectory.txt`) are written with `fopen()` / `fclose()` only and never `fsync()`ed; only the task log has a sync policy.
- `session_mgr_set_steps()` rewrites `meta.json` on every step, but `write_meta()` does not write the `created` field, so a session's creation time disappears once the first step count is stored.
- `session_mgr_list()` publishes only entries whose `meta.json` parses and whose directory name does not start with `.`; a directory with no valid `meta.json` (for example the empty shell left behind when the `meta.json` write fails after `mkdir`) is never published as a session.
- Locking is narrow: only `session_mgr_create()`, `session_mgr_load()`, `session_mgr_set_steps()` and `session_mgr_append_turn()` take their own mutex; `session_mgr_list()`, `session_mgr_delete()`, `session_mgr_rename()` and the trajectory read/write do not take one. There is no global `sd_access_mutex` in this component.
- `plan_manager.c` and `task_log.c` have no locking at all, and `plan_mgr_save()` relies on the caller for serialisation; the plan manager is a global singleton, so switching sessions means re-pointing it at another directory.
- An over-long goal is rejected by the caller rather than truncated (the WebUI's `/api/start` returns 400); `session_mgr_create()` itself truncates with `utf8_copy()` to `SESSION_GOAL_LEN`, which never splits a multi-byte character.
- `session_mgr_write_trajectory()` (the WebUI trajectory editor) does not trim and does not lock; it rejects text longer than `SESSION_TRAJ_MAX` outright with `ESP_ERR_INVALID_SIZE`.
- Config loading only warns and keeps the defaults, returning `ESP_OK`, when the file is missing, is <= 0 or > 8192 bytes, or fails to parse; string fields are guarded by `cJSON_IsString()`; `max_retries` is clamped to 3 and any other numeric value of 0 is replaced by its default — so the "0 = never rotate" documented for `system_prompt_interval` cannot survive `config_load()`, which turns 0 into 1.
- Profile de-duplication: a line is first normalised (lowercase, keep letters and digits, collapse everything else into a single space), and it counts as a duplicate only when the shared word count is >= 5 (`PROFILE_DUP_MIN_SHARED`) and that is >= 45% of the shorter line's words (`PROFILE_DUP_RATIO`); only words of length >= 3 are counted, and they are matched as substrings with `strstr()`. Once the file reaches 4096 bytes (`PROFILE_MAX_BYTES`) an append is refused with `ESP_ERR_NO_MEM`, while a duplicate returns `ESP_OK` without writing.
- Short-term memory is not a ring buffer: `stm_init()` does not free a previous array, and when the `malloc()` used for merging fails the code still executes `s_entries[s_count] = ...`, which writes out of bounds when the array is full.
- The UTF-8 helpers are all inline functions in `utf8_util.h`: `utf8_copy()` truncates on a character boundary and always NUL-terminates, and `utf8_sanitize()` rewrites invalid sequences in place as `?` byte-for-byte (length unchanged, returns the number of replacements). `cloud_client` calls `utf8_sanitize()` on the assembled request body as a safety net, because a JSON request body must be valid UTF-8 or the API rejects the entire request with a 400 — one Chinese character split in half was enough to break every single call.
- FatFS needs long file names and enough file locks: `sdkconfig.defaults` sets `CONFIG_FATFS_LFN_HEAP=y`, `CONFIG_FATFS_MAX_LFN=255`, `CONFIG_FATFS_FS_LOCK=4` and `CONFIG_FATFS_TIMEOUT_MS=10000`.
