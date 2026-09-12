# hid_device — USB HID 键盘 + 绝对坐标鼠标

## 简介

基于 TinyUSB 的复合 HID 设备，同时提供键盘（6KRO）和绝对坐标鼠标功能。

## HID 报告描述符

- **1 个 HID 接口**，包含 2 个顶层集合
- **键盘**：Report ID 1，标准启动键盘（8 字节 boot report）
- **鼠标**：Report ID 2，绝对坐标 0~32767 + 3 按键 + 滚轮

## 绝对坐标映射

```
abs = (pixel × 32767) / screen_width
```

每次报告都是**全量快照**：同时包含位置和按键状态。维护 `last_x`、`last_y`、`last_buttons` 三个状态变量。

## 支持的操作

| 函数 | 功能 |
|------|------|
| `hid_mouse_move_abs()` | 绝对坐标移动 |
| `hid_mouse_click()` | 点击 (left/right/middle/double_left) |
| `hid_mouse_down/up()` | 按下/释放（拖拽用） |
| `hid_mouse_scroll()` | 滚轮 |
| `hid_key_press()` | 按键组合 (如 ctrl+c) |
| `hid_type_string()` | 输入 ASCII 文本 |
| `hid_release_all()` | 释放所有按键和鼠标按钮 |

## 子模块

| 文件 | 功能 |
|------|------|
| `hid_device.c` | TinyUSB 初始化、HID 操作实现 |
| `hid_reports.c` | 底层报告发送（tud_hid_n_report） |
| `hid_keymap.c` | 70+ 按键名 → HID 键码映射、修饰键检测、Shift 符号处理 |
| `hid_test.c` | 键盘自检程序 |
| `mouse_test.c` | 鼠标自检程序 |

## 注意事项

- ESP32-P4 有两个 USB OTG。MS2109 用 Slot 0 (FS)，HID 用 Slot 0 (FS) — 不同 PHY
- 鼠标报告 MUST 使用 `tud_hid_n_report(0, 2, ...)` 传入 report_id=2，不能用 `tud_hid_report(0, ...)` (report_id=0)
- 鼠标报告数据 6 字节（buttons + X(16bit) + Y(16bit) + wheel），TinyUSB 自动前导 report ID
- 连续报告间可能端点忙，已内置 5 次重试（每次 2ms 间隔）
- 按键按住 100ms 后释放（Windows 需要 >= 50ms 才能识别）
- 文本输入速度约 45ms/字符
