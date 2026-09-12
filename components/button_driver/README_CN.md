# button_driver — GPIO 按键驱动

## 简介

通过 GPIO 中断 + FreeRTOS 任务通知实现的按键驱动，支持长短按区分。

## 引脚定义

| GPIO | 功能 |
|------|------|
| GPIO0 | 开始（短按 < 1s） |
| GPIO1 | 暂停/恢复（短按 < 1s）/ 停止（长按 >= 3s） |

## 工作机制

1. GPIO ISR 检测边沿变化（上升/下降），向 `btn_task` 发送通知
2. `btn_task` 等待通知 → 延迟 50ms 去抖 → 读取引脚电平确认
3. 记录按下时刻，释放时计算持续时间，区分长短按
4. 调用 Agent API（start/pause/resume/stop）

## 注意事项

- 高优先级任务（8），确保按键响应及时
- 去抖 50ms（FreeRTOS vTaskDelay）
- 长按阈值为 3 秒
- ISR 中使用 `xTaskNotifyFromISR`，不在 ISR 中做重活
