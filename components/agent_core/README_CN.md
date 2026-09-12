# agent_core — Agent 核心

> **中文** | [English](README.md)

## 简介

Agent 状态机 + 主循环 + 动作执行器，是整机的调度核心：抓屏、组装上下文、调用 LLM、执行 HID 动作、把结果写回会话轨迹与日志。

## 文件

| 文件 | 职责 |
|------|------|
| `agent_core.c` / `agent_core.h` | 状态机、主循环、上下文与状态行组装、会话/轨迹/日志、自适应推理预算 |
| `action_executor.c` / `action_executor.h` | 动作执行（鼠标/键盘）、坐标反映射、重复检测、输入法通道与失败证据 |

## 状态机

状态为 `IDLE` / `RUNNING` / `PAUSED` / `STEP` / `STOPPING` / `EMERGENCY`，事件位 `AGENT_EVT_RUN` / `AGENT_EVT_STEP` / `AGENT_EVT_EMERGENCY` / `AGENT_EVT_STOP` 经 FreeRTOS EventGroup 传递，`state_mutex` 保护状态读写。

- `agent_start` / `agent_start_session` 只能从 `IDLE` 进入；`agent_pause` 只能从 `RUNNING`；`agent_resume` / `agent_step_once` 只能从 `PAUSED`。
- 主循环把 `STEP` 与 `RUNNING` 同等视为“继续执行”（`agent_should_continue()`）；`agent_step_once()` 只能从 `PAUSED` 调用。
- `agent_stop` 或达到 `max_actions` 会关闭日志并回到 `IDLE`；`agent_emergency` 立即释放全部 HID 并回到 `IDLE`。
- 所有异常路径（采帧失败、LLM 失败）都回到 `IDLE`，不退出主循环任务——旧实现会永久删除该任务，之后只能断电重启。

## 主循环

1. 非 `RUNNING` / `STEP` 时阻塞等待事件位。
2. 每步都用 `uvc_capture_one_frame_sig()` 抓一帧（5000ms 超时）；连续失败 3 次回到 `IDLE`。
3. 用帧指纹（长度 + 哈希）与上一帧比较，得到“屏幕是否变化”的客观结论；指纹相同且视野相同时复用已编码载荷，省掉压缩与 base64。
4. 选择系统提示词：`system_prompt_interval == 0`（默认）时永远发完整版以保住 API 前缀缓存，否则按步数间隔轮换。
5. 组装 user 文本：目标、状态行、设备 Profile、上一步动作摘要、当前计划、会话轨迹。
6. 选择推理预算，然后 `cloud_client_query()` 发送截图 + 上下文。
7. 写任务日志（含逐步耗时分解），并把耗时发布给 WebUI。
8. 解析 `actions` / `done` / `need_screen` / `sleep_before_next` 并执行动作；整批只有 `wait` 且 `need_screen:false` 时进入等待模式。
9. 保存 `plan_update`、追加 `profile_updates`、处理 `request_profile`，把本轮追加到会话轨迹，再采用 `next_view`。
10. `done:true` 则记录结束状态、关日志、回 `IDLE`；否则等待 `sleep_before_next`（下限 1s、上限 300s）后继续。

## 会话与轨迹

- `agent_start` 新建会话（`session_mgr_create`）；`agent_start_session` 续接既有会话，恢复其目标、计划、轨迹与步数。
- 会话目录 `/sdcard/sessions/<id>/`，含 `meta.json`、`plan.json`、`trajectory.txt`；设备 Profile 描述机器而非任务，跨会话共享。
- 轨迹就是模型的记忆：每轮一条 `#步号 [时间] look=<full|ROI(...)>`，后接 `action:` / `saw:` / `result:` / `plan:`；坐标一律是全帧归一化，`look=` 说明该步实际看到的区域。
- 任务日志写在 `/sdcard/log/<session_id>/`；原始模型响应只进日志，不回灌上下文。

## 公开 API

| 接口 | 说明 |
|------|------|
| `agent_init(ctx, max_actions, capture_w, capture_h, output_w, output_h, prompt_interval, dry_run)` | 初始化上下文；`dry_run` 目前只存进 ctx，主循环未使用 |
| `agent_start(ctx, task_goal)` | 新建会话并开始 |
| `agent_start_session(ctx, session_id)` | 续接既有会话 |
| `agent_pause` / `agent_resume` / `agent_step_once` / `agent_stop` / `agent_emergency` | 状态控制 |
| `agent_get_state` / `agent_get_global` | 查询状态 / 取全局上下文 |
| `agent_set_wait_skip_policy(ctx, allow, max_blind_rounds, wait_settle_ms)` | 空闲跳过策略（默认关闭） |
| `agent_set_effort_policy(ctx, normal, escalated, escalate_after)` | 自适应推理预算 |
| `agent_set_active_profile(ctx, profile_id)` | 设定 `profile_updates` 的写入目标（必须调用） |
| `agent_main_loop_task(pv)` | 主循环任务体 |
| `action_executor_execute_all(actions, screen_w, screen_h)` | 执行一批动作并生成摘要 |
| `action_executor_set_view(view)` / `action_executor_set_run_timing(dialog_ms, settle_ms)` | 声明坐标视野 / 配置 `run` 时序 |
| `action_executor_get_last_summary()` / `action_executor_get_last_type_report()` | 上一步动作摘要 / 最近一次 `type` 的设备侧证据 |
| `action_executor_repetition_count()` / `action_executor_last_type()` / `action_executor_ime_toggle_count()` / `action_executor_reset_ime_toggles()` | 重复检测、输入法切换计数 |
| `action_executor_emergency_release()` | 释放全部 HID |

## 动作类型

| `action_type` | 关键参数 | 行为 |
|------|------|------|
| `click` / `move` | `box_2d`, `button` | 有 `box_2d` 先移动再点击（移动后等 50ms）；`click` 无 `box_2d` 则在原位点击，`move` 无 `box_2d` 跳过并告警 |
| `drag` | `from_box`, `to_box`, `button`, `hold` | 移动后等 20ms 按下，8 步插值、每步 5ms，可选 `hold` 毫秒后释放 |
| `scroll` | `delta_x`, `delta_y` | 滚轮 |
| `press` / `hotkey` | `key` | 单键或组合键；输入法切换键会计数并追加 150ms 等待 |
| `run` | `command`, `clear_first`, `wait_after` | Win+R → 等 dialog → 可选 ctrl+a → 输入 → 等 settle → 采样指纹 → Enter → 等 700ms 验证，未变化则补一次 Enter |
| `type` | `text`, `clear_first`, `replace_chars`, `digits_numpad` | `replace_chars` 用 shift+left N 次（≤200）且与 `clear_first` 互斥；`digits_numpad` 需 Num Lock 已确认 |
| `wait` / `release` | `wait_seconds` / `button` | 等待（≤60s）/ 抬起按键 |
| `complete` / `error` / 其他 | `message` | 记录用；未知类型忽略并告警 |

每个动作之后还会等待 `delay_after`（默认 0.05s，上限 10s）。

## 坐标、视野与状态行

- `box_2d` 为 `[ymin, xmin, ymax, xmax]`，归一化到模型实际看到的那张图；归一化坐标先经 `view_map_to_full()` 从 ROI 映射回全帧再乘分辨率，这是唯一的反映射点。
- 兼容像素坐标但整条 box 一起判定：任一坐标 > 2.0 就把两轴都按全屏像素处理并限频告警（不做逐轴混合），此时不再套用 ROI 映射。
- `next_view` 是粘性的：`roi`（`[ymin,xmin,ymax,xmax]`）与 `scale`（1/2/4）对下一张截图生效并保持到模型改变它，不发送表示不变，空对象 `{}` 表示回到全帧；任一轴范围小于 `VIEW_MIN_EXTENT`（0.02）的 ROI 被忽略。
- 每步的 user 文本带一条代码维护的 `**Status:**` 行：视野、`step n/max`、光标归一化与像素坐标、按住的鼠标键、屏幕是否变化、连续未看屏幕轮数、最近 15 次动作中同类型次数、已发送的输入法切换次数、Num/Caps Lock、最近一次 `type` 的设备侧证据。
- 推理预算默认 `low`，卡住后升级为 `high`：卡住 = 上一步没有可见屏幕变化，或最近 15 次动作中同类型 ≥ 4 次；出现可见进展立即回落。

## 注意事项

- 主循环任务在 `main.c` 中以 16384 字节栈创建（HTTP 客户端与 cJSON 占用大）。
- `need_screen` 不再阻止抓屏，代码也不再在 `click`/`press`/`type`/`drag` 后强制置 `true`（改由系统提示词要求模型自行置 true）；它只决定“整批动作为 `wait` 且模型要求不看屏幕”时能否跳过 API 调用，且需要 `allow_wait_skip` 打开、指纹未变、未超过 `max_blind_rounds`（默认 3，`wait_settle_ms` 默认 1000ms）。
- 复用已编码图片要求帧指纹相同且视野相同（`cloud_client_can_reuse_image`）；否则裁的是上一帧。
- 重复检测是计数而非“连续”：在最近 15 条动作记录（`TYPE_HISTORY`）中统计与最后一次同类型的条数，≥6 打重复告警，≥4 视为卡住并触发推理预算升级。
- `run` 的时序来自配置（`run_dialog_ms` / `run_settle_ms`，经 `action_executor_set_run_timing()` 下发），Enter 后固定等 700ms 用帧指纹验证屏幕变化；始终无变化时在摘要里标记 `[NO SCREEN CHANGE]`。
- `digits_numpad` 在 Num Lock 未被主机 LED 报告确认时会被拒绝并回落主键区，摘要标记 `[keypad unavailable]`；切换键包括 `shift`、`ctrl+space`、`win+space`、`alt+shift`、`ctrl+shift`、`shift+space`，计数每任务重置。
- `agent_set_active_profile()` 必须调用：`agent_init()` 会清零上下文，active_profile 为空时 `profile_updates` 会被追加到名为 `.md` 的文件；另外每步发往模型的是配置的默认 Profile，`request_profile` 只改变 `profile_updates` 的写入目标。
- 短期记忆（`stm_add()`）只供 WebUI 与显示任务读取，不进入提示词；进入模型上下文的是会话轨迹。
- 状态行与轨迹文本在截断后都会做 `utf8_sanitize()`，避免半个多字节字符让请求体非法。
