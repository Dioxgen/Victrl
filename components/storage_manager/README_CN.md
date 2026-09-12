# storage_manager — 存储管理

> **中文** | [English](README.md)

## 简介

SD 卡数据管理层：配置加载、设备 Profile、会话（对话窗口）、计划、短期记忆与任务日志。数据分两类——Profile 与短期记忆常驻内存，会话、计划、轨迹与日志落在 SD 卡上。一次任务对应一个会话，任务目标、计划（`plan.json`）与轨迹（`trajectory.txt`）都放在该会话自己的目录里；设备 Profile 描述的是机器而不是任务，所以不随会话切换。

## 公开 API

| 模块 | 接口 | 说明 |
|------|------|------|
| Config | `config_load()` | 先填默认值，再按 `api` / `agent` / `hid` / `uvc` / `paths` / `http` / `wifi` 分段覆盖 `runtime_config_t` |
| Profile | `profile_mgr_init()`、`profile_mgr_get()`、`profile_mgr_get_default()`、`profile_mgr_append()` | `<profile_dir>/<id>.md` 的初始化、读取（单条缓存）与去重追加 |
| Plan | `plan_mgr_init()`、`plan_mgr_load()`、`plan_mgr_save()`、`plan_mgr_get_current()` | 指向会话目录，读写并合并 `plan.json`，取当前计划的副本 |
| Plan | `plan_mgr_new_task()`、`plan_mgr_new_task_with_id()` | 重置为只含一条起始里程碑的新计划 |
| Session | `session_mgr_init()`、`session_mgr_current_dir()` | 设定并创建根目录；返回当前会话目录 |
| Session | `session_mgr_create()`、`session_mgr_load()` | 新建会话或切换当前会话，两者都会把 plan manager 重指向该会话目录 |
| Session | `session_mgr_list()` | 枚举会话，最新在前（上限由调用方给出） |
| Session | `session_mgr_current_id()`、`session_mgr_current_goal()`、`session_mgr_current_steps()` | 当前会话的 id、目标与步数 |
| Session | `session_mgr_set_steps()` | 把步数写回当前会话的 `meta.json` |
| Session | `session_mgr_append_turn()`、`session_mgr_get_trajectory()`、`session_mgr_write_trajectory()` | 轨迹的追加、整体读取与整体覆盖 |
| Session | `session_mgr_delete()`、`session_mgr_rename()` | 删除会话（三个文件加目录）或改写标题 |
| Task log | `task_log_init()`、`task_log_close()` | 打开/关闭 `<log_dir>/<task_id>/<task_id>.log` |
| Task log | `task_log_write_step()` | 写一步的提示词类型、字节数、耗时、动作摘要、观察、自评、推理、计划与原始响应 |
| Task log | `task_log_write_timing()` | 写该步的耗时分解，同时用 `ESP_LOGI` 输出一行 |
| Task log | `task_log_write_note()` | 追加自由文本（结束状态），写后 `fsync()` |
| Task log | `task_log_save_frame()` | 保存该步截图，仅在 `api.save_frames` 为 true 时被调用 |
| STM | `stm_init()`、`stm_add()`、`stm_get_all_formatted()`、`stm_clear()` | 固定长度短期记忆 |
| UTF-8 | `utf8_seq_len()`、`utf8_copy()`、`utf8_sanitize()` | `utf8_util.h` 里的内联 helper |

## 子模块

| 文件 | 功能 |
|------|------|
| `config_loader.c` | 从 `/sdcard/config.json` 读取运行时配置，缺失或非法时静默回退到内置默认值 |
| `profile_manager.c` | 多设备 Profile（`.md`）的读取、缓存与去重追加 |
| `plan_manager.c` | 内存中的计划对象与 `<dir>/plan.json` 的合并、加载与保存 |
| `session_manager.c` / `.h` | 会话（对话窗口）：目录、`meta.json`、轨迹与步数 |
| `short_term_mem.c` | 固定长度短期记忆，写满后合并最早两条 |
| `task_log.c` / `.h` | 任务日志、耗时分解、结束状态与可选截图 |
| `utf8_util.h` | UTF-8 安全截断与非法字节清洗（仅头文件，全部 `static inline`） |
| `storage_manager.h` | 汇总公开声明与 `runtime_config_t`，并 include `session_manager.h`、`task_log.h` |

## SD 卡目录结构

| 路径 | 内容 |
|------|------|
| `/sdcard/config.json` | 运行时配置 |
| `/sdcard/sysprompt/full.txt`、`short.txt` | 完整与精简系统提示词（`paths.sysprompt_dir`） |
| `/sdcard/profiles/<id>.md` | 设备 Profile，默认 `win11_laptop.md` |
| `/sdcard/sessions/<id>/meta.json` | 会话元数据：`id`、`title`、`goal`、`created`、`steps` |
| `/sdcard/sessions/<id>/plan.json` | 该会话的计划与里程碑 |
| `/sdcard/sessions/<id>/trajectory.txt` | 该会话的逐轮轨迹，也就是模型的上下文 |
| `/sdcard/plans/` | 旧的扁平计划目录；`paths.plan_dir` 仍可配置，但已不再写入 |
| `/sdcard/log/<task_id>/<task_id>.log` | 任务日志 |
| `/sdcard/log/<task_id>/<task_id>_stepNNNN.jpg` | 每步截图，仅当 `api.save_frames` 为 true 时产生 |
| `/sdcard/web/index.html` | WebUI（`paths.web_dir`） |

## 会话（对话窗口）

会话 id 由本地时间的 `%Y%m%d_%H%M%S` 生成，同一秒内冲突时追加 `_NN` 后缀（最多试 100 次）；id 只接受 `[0-9A-Za-z_-]`，这也是路径穿越防护。目录与容量上限如下：

| 常量 | 值 | 含义 |
|------|-----|------|
| `SESSION_ID_LEN` | 24 | id 缓冲长度，合法 id 必须更短；`session_mgr_init()` 还要求 `session_dir` 长度 + 1 + 24 <= 64 |
| `SESSION_TITLE_LEN` | 64 | `meta.json` 的 `title`，由目标文本经 `utf8_copy()` 截得 |
| `SESSION_GOAL_LEN` | 1024 | 任务目标上限（字节，约 340 个汉字） |
| `SESSION_TRAJ_MAX` | 24576 | 轨迹上限；追加时达到该值才触发一次裁剪 |
| `SESSION_TRAJ_KEEP` | 16384 | 裁剪后保留的尾部字节数 |

轨迹裁剪发生在追加之前：文件已达到 `SESSION_TRAJ_MAX` 时，一次性丢掉前面部分、只保留最后约 `SESSION_TRAJ_KEEP` 字节，并向后推进到行首以 `#` 开头的行边界，然后在开头插入一行 `[earlier steps trimmed]`；找不到这样的行边界时按字节硬切。单条轨迹记录由 `agent_core` 组装成 `#N [HH:MM:SS] look=... | action: ...` 加缩进的 `saw:` / `result:` / `plan:` 三行，各文本字段先经 `utf8_copy()` 预截断。

新建会话（`session_mgr_create()`）会 `mkdir` 会话目录、写 `meta.json`，然后把 plan manager 指向该目录并写入初始计划；切换会话（`session_mgr_load()`）同样重指向并加载该会话的 `plan.json`。列出会话时按 id 降序排列（id 即时间戳，因此最新在前）。

## 计划合并（`plan.json`）

`plan_mgr_save()` 每次都重写整个 `plan.json`，用 `cJSON_PrintUnformatted()` 输出，因为这份 JSON 每步都要发给模型，缩进纯属浪费 token。合并规则：cJSON 的 `Add*ToObject` 是追加而不是覆盖，所以写回前要先删掉已存在的同名键，且 `cJSON_DeleteItemFromObject()` 只删第一个匹配，必须循环删除；`milestones` 只有在模型确实发来数组时才整体替换，缺失表示“计划没变”而不是删除计划；`current_milestone` 独立于数组处理，模型可以不重发 `milestones` 而只推进指针，它同时写进顶层（LCD 读顶层）和 `plan_update` 内；嵌套的 `plan_update` 只保留顶层没有归宿的 `summary` 与 `current_milestone`，两个字段都没有时整个 `plan_update` 不写入，避免同一份里程碑数组在文件里出现两次。`s_task_id` 为空时只更新内存对象、不写文件，直接返回 `ESP_OK`。

## 任务日志

每次任务（或续跑一个会话）打开一个日志文件，路径为 `<log_dir>/<task_id>/<task_id>.log`，以追加模式打开，因此续跑会接着写同一个文件。开头与结尾写入任务名和时间：

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
Reasoning: ...        (仅当非空)
Plan: ...             (仅当非空)
Response: ...         (仅当非空)

Timing(ms): total=... capture=... prep=... (jpeg=... setup=... dec=... rs=... enc=... b64=... json=...) connect=... http=... parse=... sdlog=... exec=... sleep=...
```

时间戳取自 `localtime()`，`wifi_manager` 设了 `TZ=CST-8`，所以 SNTP 同步之后是北京时间。`Timing(ms):` 那一行把一步的墙钟时间拆成采集、准备（含 JPEG 与 base64 子项）、连接、HTTP、解析、日志、执行与休眠；它同时以 `ESP_LOGI` 输出，不必拔卡就能看。注意 `task_log_write_step()` 的 `exec_time_ms` 目前由调用方恒传 0，真实的 HID 执行耗时在 `Timing(ms):` 行的 `exec=` 里。任务结束时 `task_log_write_note()` 追加结束状态（指针位置、视图、停止原因），`task_log_close()` 补上 `Ended:` 行并做最后一次 `fsync()`。

## 短期记忆

- 固定长度的 `char *` 数组，长度由 `config.json` 的 `agent.history_max_len` 决定（默认 10），`stm_init()` 里 `calloc()` 分配。
- 写满后每次 `stm_add()` 把最早两条用分号加空格合并成一条再整体前移，条目数回到上限以下。
- 读取方是 WebUI 的 `/api/log` 与 LCD 显示任务；写入方是 agent 主循环。
- 所有操作由 `stm_init()` 创建的互斥量保护；`stm_get_all_formatted()` 用换行连接各条，空时返回 `No history yet.`。

## 注意事项

- 任务日志是缓冲写：`_IOFBF` 加 4096 字节静态缓冲，每步只 `fflush()`；`fsync()` 只在 `task_log_init()`、`task_log_write_note()`、`task_log_close()` 以及每 10 步（`TASK_LOG_SYNC_INTERVAL`）执行，所以断电可能丢掉日志尾部。
- `task_log_close()` 在 done、stop、`max_actions`、emergency 四条退出路径上都会被调用（调用点在 `agent_core.c`），不会把文件一直开着。
- 会话文件（`meta.json`、`plan.json`、`trajectory.txt`）只用 `fopen()` / `fclose()` 写入，没有任何 `fsync()`；只有任务日志有同步策略。
- `session_mgr_set_steps()` 每步重写 `meta.json`，但 `write_meta()` 不写 `created` 字段，因此会话创建时间在第一次记步数之后就消失了。
- `session_mgr_list()` 只发布能解析出 `meta.json`、且目录名不以 `.` 开头的条目；没有合法 `meta.json` 的目录（例如建目录后写 `meta.json` 失败留下的空壳）不会被当作会话发布。
- 加锁范围有限：只有 `session_mgr_create()`、`session_mgr_load()`、`session_mgr_set_steps()`、`session_mgr_append_turn()` 取自己的互斥量；`session_mgr_list()`、`session_mgr_delete()`、`session_mgr_rename()` 与轨迹读写都不取锁。本组件没有全局 `sd_access_mutex`。
- `plan_manager.c` 与 `task_log.c` 完全没有锁，`plan_mgr_save()` 靠调用方串行化；plan manager 是全局单例，切换会话就是把它重指向另一个目录。
- 目标超长由调用方拒绝而不是截断（WebUI 的 `/api/start` 返回 400）；`session_mgr_create()` 自己会用 `utf8_copy()` 截断到 `SESSION_GOAL_LEN`，保证不会切开多字节字符。
- `session_mgr_write_trajectory()`（WebUI 轨迹编辑器）不裁剪、不加锁，直接拒绝超过 `SESSION_TRAJ_MAX` 的文本并返回 `ESP_ERR_INVALID_SIZE`。
- 配置加载在文件缺失、大小 <= 0 或 > 8192 字节、JSON 解析失败时都只告警并保留默认值、返回 `ESP_OK`；字符串字段都有 `cJSON_IsString()` 保护；`max_retries` 被钳到 3，其余数值为 0 时会被改成默认值——因此 `system_prompt_interval` 注释里的“0 = 永不轮换”在 `config_load()` 之后不可能成立，0 会被改成 1。
- Profile 去重：先把行归一化（转小写、只保留字母与数字、其他字符压缩成一个空格），共享词数 >= 5（`PROFILE_DUP_MIN_SHARED`）且 >= 较短一行词数的 45%（`PROFILE_DUP_RATIO`）才算重复；只统计长度 >= 3 的词，且用 `strstr()` 子串匹配。文件达到 4096 字节（`PROFILE_MAX_BYTES`）后拒绝追加并返回 `ESP_ERR_NO_MEM`，判为重复时返回 `ESP_OK` 但不写。
- 短期记忆不是环形缓冲：`stm_init()` 不释放上一次的数组；合并用的 `malloc()` 失败时仍会执行 `s_entries[s_count] = ...`，在写满的情况下会越界。
- UTF-8 helper 全部是 `utf8_util.h` 里的内联函数：`utf8_copy()` 按字符边界截断并保证 NUL 结尾，`utf8_sanitize()` 就地用 `?` 逐字节替换非法序列（长度不变，返回替换个数）。`cloud_client` 在请求体组装完成后调用 `utf8_sanitize()` 兜底，因为 JSON 请求体必须是合法 UTF-8，否则 API 会以 400 拒绝整个请求——一个被切成两半的汉字就足以让每一次调用都失败。
- FatFS 需要长文件名与足够的文件锁：`sdkconfig.defaults` 设置 `CONFIG_FATFS_LFN_HEAP=y`、`CONFIG_FATFS_MAX_LFN=255`、`CONFIG_FATFS_FS_LOCK=4`、`CONFIG_FATFS_TIMEOUT_MS=10000`。
