# display_task — LCD 状态显示

> **中文** | [English](README.md)

## 简介

在 NV3007 SPI LCD（横屏 428×142）上显示 Agent 运行状态的 FreeRTOS 任务：任务名 `lcd_refresh`、优先级 2、栈 3072 字节。任务启动时自行调用 `NV3007_Init()` 并清屏，之后每 2 秒或被 `display_task_notify()` 唤醒时整屏重绘（每轮先 `NV3007_FastFill(COLOR_BG)` 清屏，再重画全部内容）。

## 显示内容

字体固定为 12 像素高（`FONT_SM` → `ascii_1206`，每字符 6×12 像素）；行、列锚点由 `NX(v)`、`NY(v)` 归一化宏换算，行距则是固定的 14 像素。

| 位置 | 内容 | 来源 | 颜色 |
|------|------|------|------|
| 第 1 行 y=2，x=0 | `IP:<ip>`（`IP:%.15s`，最多 15 字符） | `wifi_manager_get_ip()`；未连接时是它的初始值 `0.0.0.0` | 白色 |
| 第 1 行 x=136 | `Step:<action_count>`；计划里 `milestones` 非空时显示 `Step:<action_count> <current_milestone>/<总数>` | `agent->action_count`，以及 `plan_mgr_get_current()` 的 `milestones` 与 `plan_update.current_milestone` | 青色 |
| 第 1 行 x=248 | `RUN`、`PAUSE`、`STEP`、`STOP`、`EMERG`，其余状态为 `IDLE` | `agent_get_state()` | RUNNING 绿、PAUSED 黄、EMERGENCY 红，其余白 |
| 第 2 行 y=25，x=0 | 任务名，最多 55 字节 | `agent->task_goal`；为空时不绘制 | 白色 |
| 第 3–7 行 y=39/53/67/81/95 | 短期记忆最近 5 条，每条最多 50 字节 | `stm_get_all_formatted()` 按 `\n` 拆行后取末尾 5 行 | 白色 |

## 更新机制

- 循环末尾是 `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000))`：2 秒超时刷新，`display_task_notify()` 用 `xTaskNotifyGive()` 立即唤醒。
- `agent_get_global()` 返回 `NULL` 时只等待 2 秒后重试，本轮不绘制。
- `plan_mgr_get_current()` 返回的是副本，由本组件用 `cJSON_Delete()` 释放；`stm_get_all_formatted()` 返回的字符串由本组件用 `free()` 释放。

## 注意事项

- 调用方只需调用 `display_task_init()`；`NV3007_Init()` 和首次清屏都在 `display_task()` 内部完成，不需要在 `main` 里重复初始化。
- 任务名行用 `utf8_copy(task, 56, agent->task_goal)` 截断：最多 55 字节，且不会切断 UTF-8 字符。日志行仍是字节截断——`strncpy` 到 54 字节后再硬切到 50 字节，可能切断多字节字符。
- `NV3007_ShowString()` 只绘制 `' '`–`'~'` 范围内的字节，遇到其他字节立即停止，字体也没有中文字模，所以中文任务名和中文日志不会显示。
- 日志拆行用 `strtok_r` 而不是 `strtok`，因为 HID 任务也在分词，`strtok` 的全局游标会在任务之间互相干扰。
- 归一化宏只作用于行列锚点；12 像素字高和 14 像素行距都是固定值，换分辨率不会自动缩放。
- 面板走 `NV3007_SPI_HOST`（默认 `SPI2_HOST`），SD 卡走 sdmmc 主机 `SDMMC_HOST_SLOT_0`，两者不共用总线。
- `FONT_MD`（16）已定义但未被使用。
- `action_count` 与 `task_goal` 是直接读 `agent_ctx_t` 的字段，只有状态经 `agent_get_state()` 的互斥量保护。
- `display_task_init()` 在 `xTaskCreate()` 失败时返回 `ESP_ERR_NO_MEM`；`main.c` 未检查返回值。
- `display_task_notify()` 在当前仓库里没有任何调用点。
