# web_server — HTTP server + WebUI

> **English** | [中文](README_CN.md)

## Overview

An HTTP/1.1 server built on esp_http_server that gives the device a WebUI and a REST API: task control, system status and logs, SD-card file management, conversation-session management, and screen snapshots.

The public API is just two calls: `web_server_start(port, web_dir)` and `web_server_stop()`. The port and the WebUI directory come from `config.json` (`http.port`, `paths.web_dir`) and default to `80` and `/sdcard/web`.

## Routes

24 handlers over 23 URIs (`/api/sessions/trajectory` registers both GET and POST).

| Route | Method | Purpose |
|-------|--------|---------|
| `/` | GET | Serve the WebUI from `web_dir/index.html`; if the file is missing or empty, return a built-in placeholder page |
| `/api/status` | GET | System status JSON (see below) |
| `/api/log` | GET | Recent action log as JSON: `state`, `step`, `task`, `log` |
| `/api/plan` | GET | Current task plan as JSON; with no plan, `{"summary":"No plan","milestones":[]}` |
| `/api/profiles` | GET | Active profile: `{"active":"...","profiles":[]}` |
| `/api/start` | POST | Start a task (body: `{"task":"..."}`, optional `{"session":"<id>"}` to resume an existing session) |
| `/api/stop` | POST | Stop the task |
| `/api/pause` | POST | Pause the task |
| `/api/resume` | POST | Resume the task |
| `/api/step` | POST | Execute a single step |
| `/api/emergency` | POST | Emergency stop |
| `/api/reboot` | POST | Reply `{"ok":true,"rebooting":true}` first, then `esp_restart()` after about 300 ms |
| `/api/fs` | GET | Directory listing (`?path=`, defaults to `/sdcard`): `path`, `truncated`, `entries[]` (`name`/`type`/`size`), directories first and names sorted case-insensitively |
| `/api/fs/read` | GET | Read a file (`?path=`), returns `text/plain; charset=utf-8` |
| `/api/fs/write` | POST | Write a file (`{"path":"...","content":"..."}`), returns `{"ok":true}` |
| `/api/fs/delete` | POST | Delete a file or an empty directory (`{"path":"..."}`); a non-empty directory is an error |
| `/api/fs/mkdir` | POST | Create a directory (`{"path":"..."}`) |
| `/api/sessions` | GET | Session list: `current`, `current_goal`, `current_steps`, `sessions[]` (`id`/`title`/`goal`/`created`/`steps`/`traj_bytes`), at most 24 entries |
| `/api/sessions/select` | POST | Switch the active session (`{"id":"..."}`); refused while the agent is not IDLE |
| `/api/sessions/delete` | POST | Delete a session (`{"id":"..."}`); refused while the agent is not IDLE, and deleting the active session falls back to the newest one |
| `/api/sessions/rename` | POST | Rename a session title (`{"id":"...","title":"..."}`) |
| `/api/sessions/trajectory` | GET | Trajectory of the current session, returns `text/plain; charset=utf-8` |
| `/api/sessions/trajectory` | POST | Overwrite the current session trajectory (`{"content":"..."}`); too long returns 400 |
| `/api/snapshot` | GET | `image/jpeg`; by default the exact image last sent to the model (already cropped/scaled); `?live=1` captures a fresh frame (2 s timeout) |

The `/api/status` JSON groups: `state`/`actions`/`fails`/`task`/`profile`/`session`/`session_goal`/`max_actions`/`wait_mode`/`effort`/`stuck_streak`, `wifi` (`connected`/`ip`/`rssi`/`mac`), `time` (`synced`/`sync_count`/`failures`/`servers_configured`/`uptime_s`/`servers[]`/`local`/`epoch`/`since_sync_s`), `heap.internal` and `heap.psram` (each with `free`/`largest`), `fs` (`total_kb`/`free_kb`), `last_step` (the timing breakdown of the previous step), and `reset_reason`.

## WebUI

A single-page Dashboard loaded from `/sdcard/web/index.html`, with three tabs:

- **Dashboard**: task control (input box plus Start/Pause/Resume/Step/Stop/emergency-stop), system info, network and time, screen preview (switchable to a live feed), previous-step timing breakdown, plan, and the run log; auto-refreshes every 2 seconds while the tab is active.
- **Sessions**: the session list (switch/rename/delete) and viewing/editing the current session's trajectory.
- **SD card files**: directory browsing, creating directories/files, and editing and saving them (a saved `config.json` only takes effect after a reboot, and the page offers a reboot button).

The page also calls `/api/start`, `/api/stop`, `/api/pause`, `/api/resume`, `/api/step`, `/api/emergency`, `/api/reboot`, and `/api/snapshot`.

## Notes

- **No route is authenticated**, and JSON responses carry `Access-Control-Allow-Origin: *`. Anyone on the same network segment can control the agent, read and write the SD card, and reboot the device, so it should only be deployed on a trusted LAN.
- The file API's path guard `fs_path_ok()` requires the path to start with `/sdcard` followed immediately by `/` or the end of the string, to be shorter than 256 bytes, and to contain no `..` anywhere; `/api/fs/delete` additionally refuses to delete `/sdcard` itself. Query parameters are percent-decoded, but `+` is not treated as a space.
- File size limits: reads are capped at 128 KiB (`FS_MAX_READ`, 131072 bytes) and write request bodies at 192 KiB (`FS_MAX_WRITE`, 196608 bytes, leaving room for JSON escaping); exceeding either returns 400. A directory listing returns at most 64 entries, and `truncated` is `true` when it was cut short.
- Request bodies are read to completion in a loop by `recv_body()` (`httpd_req_recv()` may return only part of the data); a `content_len` of 0 or one above the limit returns 400 immediately.
- Startup order: `web_server_start()` must be called after `wifi_manager_init()`, which is where the network stack is initialized (`esp_netif_init()` + `esp_event_loop_create_default()`); a `port` of 0 has already been replaced by 80 in the config layer.
- Server configuration: `max_uri_handlers = 32` (24 handlers are registered today, leaving 8 slots; adding more routes means raising this, otherwise registration fails with nothing but a log line and startup continues), `stack_size = 8192` (the file API does FATFS + cJSON work inline in the handler), and `lru_purge_enable = true`.
- Related Kconfig settings (`sdkconfig.defaults`): `CONFIG_HTTPD_MAX_URI_LEN=512` and `CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024`; the socket ceiling is `CONFIG_LWIP_MAX_SOCKETS=16`, shared by WiFi, the HTTP server, and the cloud client.
- `/api/log` only escapes newlines, double quotes, and backslashes in its JSON; `/api/fs/read` and `/api/sessions/trajectory` return `text/plain` directly with no JSON envelope; `/api/snapshot` sends `Cache-Control: no-store` so the browser cannot show a stale frame.
- Editing `config.json` on the SD card only takes effect after a reboot, which can be triggered with `POST /api/reboot` or the WebUI's reboot button.
- Sessions are laid out on the SD card as `/sdcard/sessions/<id>/` (`meta.json`, `plan.json`, `trajectory.txt`), and the trajectory is capped at 24576 bytes (`SESSION_TRAJ_MAX`).
