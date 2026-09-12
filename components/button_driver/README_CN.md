# button_driver — GPIO 按键驱动

> **中文** | [English](README.md)

## 简介

基于 GPIO 中断加 FreeRTOS 任务通知的按键驱动：GPIO0 与 GPIO1 共用一个 `btn_task`，在按键释放时按按住时长判定短按或长按，并通过 `agent_core` 的接口控制全局 Agent 的状态。

## 引脚定义

| GPIO | 宏 | 配置 | 功能 |
|------|-----|------|------|
| GPIO0 | `BTN_START` | 输入、内部上拉、`GPIO_INTR_ANYEDGE` | 启动任务 |
| GPIO1 | `BTN_PAUSE` | 输入、内部上拉、`GPIO_INTR_ANYEDGE` | 暂停/恢复、停止 |

两脚均使能内部上拉，因此按下为低电平（0），松开为高电平（1）。

## 按键行为

| GPIO | 释放时按住时长 | 动作 |
|------|----------------|------|
| GPIO0 | < `LONG_PRESS_MS`（3000 ms） | 调用 `agent_start()` 启动默认任务 |
| GPIO0 | >= 3000 ms | 无动作（源码中没有长按分支） |
| GPIO1 | < 3000 ms | 当前为 `AGENT_STATE_RUNNING` 时 `agent_pause()`，为 `AGENT_STATE_PAUSED` 时 `agent_resume()`，其他状态不处理 |
| GPIO1 | >= 3000 ms | 当前为 `AGENT_STATE_RUNNING` 或 `AGENT_STATE_PAUSED` 时 `agent_stop()`，其他状态不处理 |

长短按在释放时判定，按住期间不会触发任何动作。

GPIO0 短按时传给 `agent_start()` 的目标文本是源码中硬编码的固定串，不接受外部输入：

```c
agent_start(agent, "Execute the default task");
```

## 工作机制

1. `button_driver_init()` 用 `gpio_config()` 把两个引脚配为输入、使能内部上拉、双边沿中断，然后 `gpio_install_isr_service(0)` 并逐脚 `gpio_isr_handler_add()` 注册 `btn_isr`。
2. `btn_isr` 带 `IRAM_ATTR`，用 `xTaskNotifyFromISR()` 以 `eSetValueWithOverwrite` 把触发中断的 GPIO 号作为通知值发给 `s_btn_task`，再 `portYIELD_FROM_ISR()`。
3. `btn_task` 用 `xTaskNotifyWait()` 等待通知，超时 `pdMS_TO_TICKS(100)`；收到通知后固定 `vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS))`（50 ms），再读引脚电平。
4. 电平为 0 且该脚此前未按下时记录按下时刻并置位 `pressed[]`；电平为 1 且此前已按下时视为释放，用 `xTaskGetTickCount()` 的差值乘 `portTICK_PERIOD_MS` 得到按住毫秒数。
5. 释放时按上表调用 `agent_get_global()` 取到的全局 Agent 接口；该函数返回 `NULL` 时本次事件被丢弃。判定用 `gpio == BTN_START` 映射到 `press_time[0]`/`pressed[0]`，其余映射到下标 1。

## 注意事项

- `btn_task` 优先级为 8，栈为 6144 字节（6 KB）。源码注释说明栈由原先的 2 KB 提高到该值，因为按键现在会走到 `agent_start()`（创建 session、cJSON、文件写入、日志），调用链比过去的 `plan_mgr_new_task()` 深得多；调小栈之前请先确认这条路径。
- 去抖方式是收到通知后固定延时 `DEBOUNCE_MS`（50 ms）再采样电平，不是自适应滤波；短于该窗口的抖动或脉冲会被丢掉。
- 长按阈值是 `LONG_PRESS_MS`（3000 ms）。只有 GPIO1 使用长按分支，GPIO0 长按不产生任何动作。
- 通知使用 `eSetValueWithOverwrite`，通知值只保留最后一次的 GPIO 号；任务不依赖通知值判断电平，而是重新调用 `gpio_get_level()` 采样。
- 两个按键共用同一个任务与同一组 `press_time`/`pressed` 状态。
- ISR 内只调用 `xTaskNotifyFromISR()`，不在中断上下文里做其他工作。
- `button_driver_init()` 在 `xTaskCreate()` 失败时返回 `ESP_ERR_NO_MEM`；`gpio_install_isr_service()` 的返回值未被检查。
