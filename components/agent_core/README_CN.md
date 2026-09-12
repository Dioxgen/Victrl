# agent_core — Agent 核心

## 简介

Agent 状态机 + 主循环 + 动作执行器。整个系统的调度核心。

## 状态机

```
IDLE → RUNNING → PAUSED → STEP → STOPPING → EMERGENCY
```

通过 FreeRTOS EventGroup 控制状态转换，Mutex 保护状态读写。

## 主循环

1. 等待状态变为 RUNNING 或 STEP（IDLE/PAUSED 时阻塞）
2. 如果 `need_screen`，调用 UVC 采集一帧
3. 加载系统提示词（完整/精简轮换）、设备 Profile、当前计划、短期记忆
4. 调用 LLM API 发送截图+上下文
5. 解析 JSON 响应，提取动作数组
6. 执行动作（click/move/drag/scroll/press/type/wait/release）
7. 更新短期记忆、计划、Profile
8. 保存任务日志和压缩截图
9. 等待 `sleep_before_next`（最低 1s）后循环

## 坐标处理

- 支持归一化 [0,1] 和像素坐标混合输入
- **逐轴自动检测**：如果任一轴的坐标值 > 2.0，视为像素坐标直接使用；否则按归一化乘以分辨率

## 注意事项

- 主循环任务栈需 >= 16KB（HTTP 客户端和 cJSON 消耗大）
- 动作执行器内部跟踪最近 15 次动作类型，连续 6 次相同类型触发重复警告
- `need_screen` 在 click/press/type/drag 后强制置 true
- **click 时序保护**：移动后等待 50ms 再点击（主机需时间处理光标移动，否则点击可能落空）。drag 在 move→down 之间也有 20ms 保护，拖拽过程分 8 步插值每次 5ms
