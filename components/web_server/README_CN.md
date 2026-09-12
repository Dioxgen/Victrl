# web_server — HTTP 服务器 + WebUI

> **中文** | [English](README.md)

## 简介

基于 esp_http_server 的 HTTP/1.1 服务器，为设备提供 WebUI 和 REST API：任务控制、系统状态与日志、SD 卡文件管理、会话（对话窗口）管理和画面快照。

对外的接口只有两个：`web_server_start(port, web_dir)` 与 `web_server_stop()`。端口与 WebUI 目录来自 `config.json`（`http.port`、`paths.web_dir`），默认 `80` 与 `/sdcard/web`。

## 路由

共 24 个 handler、23 个 URI（`/api/sessions/trajectory` 同时注册 GET 与 POST）。

| 路由 | 方法 | 功能 |
|------|------|------|
| `/` | GET | 从 `web_dir/index.html` 读取 WebUI；文件缺失或为空时返回内置提示页 |
| `/api/status` | GET | 系统状态 JSON（见下） |
| `/api/log` | GET | 最近动作日志，JSON：`state`、`step`、`task`、`log` |
| `/api/plan` | GET | 当前任务计划 JSON；无计划时返回 `{"summary":"No plan","milestones":[]}` |
| `/api/profiles` | GET | 当前 Profile：`{"active":"...","profiles":[]}` |
| `/api/start` | POST | 开始任务（body：`{"task":"..."}`，可选 `{"session":"<id>"}` 继续已有会话） |
| `/api/stop` | POST | 停止任务 |
| `/api/pause` | POST | 暂停任务 |
| `/api/resume` | POST | 恢复任务 |
| `/api/step` | POST | 单步执行 |
| `/api/emergency` | POST | 紧急停止 |
| `/api/reboot` | POST | 先回复 `{"ok":true,"rebooting":true}`，约 300 ms 后 `esp_restart()` |
| `/api/fs` | GET | 目录列表（`?path=`，缺省 `/sdcard`）：`path`、`truncated`、`entries[]`（`name`/`type`/`size`），目录在前、名称不区分大小写排序 |
| `/api/fs/read` | GET | 读取文件（`?path=`），返回 `text/plain; charset=utf-8` |
| `/api/fs/write` | POST | 写文件（`{"path":"...","content":"..."}`），返回 `{"ok":true}` |
| `/api/fs/delete` | POST | 删除文件或空目录（`{"path":"..."}`）；非空目录报错 |
| `/api/fs/mkdir` | POST | 新建目录（`{"path":"..."}`） |
| `/api/sessions` | GET | 会话列表：`current`、`current_goal`、`current_steps`、`sessions[]`（`id`/`title`/`goal`/`created`/`steps`/`traj_bytes`），最多 24 条 |
| `/api/sessions/select` | POST | 切换当前会话（`{"id":"..."}`）；agent 非 IDLE 时拒绝 |
| `/api/sessions/delete` | POST | 删除会话（`{"id":"..."}`）；agent 非 IDLE 时拒绝，删的是当前会话时自动回退到最新会话 |
| `/api/sessions/rename` | POST | 重命名会话标题（`{"id":"...","title":"..."}`） |
| `/api/sessions/trajectory` | GET | 当前会话轨迹，返回 `text/plain; charset=utf-8` |
| `/api/sessions/trajectory` | POST | 覆盖当前会话轨迹（`{"content":"..."}`），超长时返回 400 |
| `/api/snapshot` | GET | `image/jpeg`；默认返回最近一次发给模型的那张图（已裁切/缩放）；`?live=1` 则实时抓一帧（2 s 超时） |

`/api/status` 的 JSON 分组：`state`/`actions`/`fails`/`task`/`profile`/`session`/`session_goal`/`max_actions`/`wait_mode`/`effort`/`stuck_streak`、`wifi`（`connected`/`ip`/`rssi`/`mac`）、`time`（`synced`/`sync_count`/`failures`/`servers_configured`/`uptime_s`/`servers[]`/`local`/`epoch`/`since_sync_s`）、`heap.internal` 与 `heap.psram`（各含 `free`/`largest`）、`fs`（`total_kb`/`free_kb`）、`last_step`（上一步耗时分解）、`reset_reason`。

## WebUI

`/sdcard/web/index.html` 加载的单页 Dashboard，三个标签页：

- **Dashboard**：任务控制（输入框 + Start/Pause/Resume/Step/Stop/急停）、系统信息、网络与时间、画面预览（可切换实时画面）、上一步耗时分解、计划、运行日志；标签页激活时每 2 秒自动刷新。
- **会话**：会话列表（切换/重命名/删除），以及当前会话轨迹的查看与编辑。
- **SD 卡文件**：目录浏览、新建目录/文件、编辑并保存（保存 `config.json` 后需重启生效，页面提供重启按钮）。

页面还会调用 `/api/start`、`/api/stop`、`/api/pause`、`/api/resume`、`/api/step`、`/api/emergency`、`/api/reboot`、`/api/snapshot`。

## 注意事项

- **所有路由都没有鉴权**，且 JSON 响应带 `Access-Control-Allow-Origin: *`。同网段内任何人都能控制 agent、读写 SD 卡、重启设备，只应部署在可信局域网。
- 文件 API 的路径守卫 `fs_path_ok()`：路径必须以 `/sdcard` 开头且后面紧跟 `/` 或结束，长度小于 256 字节，且整条路径不得包含 `..`；`/api/fs/delete` 额外拒绝删除 `/sdcard` 本身。查询参数会做 percent-decode，但 `+` 不会被当作空格。
- 文件大小限制：读取上限 128 KiB（`FS_MAX_READ`，131072 字节），写入请求体上限 192 KiB（`FS_MAX_WRITE`，196608 字节，为 JSON 转义留出余量），超限返回 400。目录列表最多 64 项，超出时 `truncated` 为 `true`。
- 请求体由 `recv_body()` 循环收齐（`httpd_req_recv()` 可能只返回部分数据）；`content_len` 为 0 或超过上限直接返回 400。
- 启动顺序：`web_server_start()` 必须在 `wifi_manager_init()` 之后调用，网络栈（`esp_netif_init()` + `esp_event_loop_create_default()`）在那里初始化；`port` 为 0 时配置层已回退到 80。
- 服务器配置：`max_uri_handlers = 32`（当前注册 24 个 handler，余量 8 个；继续加路由需同步提高，否则注册失败只会记一条日志并继续）、`stack_size = 8192`（文件 API 在 handler 内联做 FATFS + cJSON）、`lru_purge_enable = true`。
- 相关 Kconfig（`sdkconfig.defaults`）：`CONFIG_HTTPD_MAX_URI_LEN=512`、`CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024`；socket 数上限 `CONFIG_LWIP_MAX_SOCKETS=16`（WiFi、HTTP 服务器与云端客户端共用）。
- `/api/log` 的 JSON 只转义换行、双引号和反斜杠；`/api/fs/read` 与 `/api/sessions/trajectory` 直接返回 `text/plain`，不做 JSON 包装；`/api/snapshot` 带 `Cache-Control: no-store`，避免浏览器缓存旧帧。
- 修改 SD 卡上的 `config.json` 后需重启才生效，可用 `POST /api/reboot` 或 WebUI 的重启按钮。
- 会话在 SD 卡上的布局为 `/sdcard/sessions/<id>/`（`meta.json`、`plan.json`、`trajectory.txt`）；轨迹长度上限 24576 字节（`SESSION_TRAJ_MAX`）。
