# hid_device — USB HID 键盘 + 绝对坐标鼠标

> **中文** | [English](README.md)

## 简介

基于 TinyUSB 的复合 HID 设备：一个 HID 接口中同时提供 6KRO 键盘和绝对坐标鼠标，供上位机通过 USB 向目标机注入按键、文本与鼠标操作。

## HID 报告描述符

| 项目 | 内容 |
|------|------|
| 接口 | 1 个 HID 接口，2 个顶层集合（键盘、鼠标） |
| 键盘 | Report ID `1`，标准启动键盘，8 字节 boot report |
| 鼠标 | Report ID `2`，绝对坐标 X/Y（0~32767）、3 个按键位、8 位相对滚轮（-127~127），共 6 字节 |
| 端点 | 中断 IN 端点 `0x81`，包长 16 字节，轮询间隔 1 ms |

## 设备身份（USB 描述符）

主机看到的这些字符串是有意为之，不是顺带产物：这台设备要的是可被识别，而不是被隐藏；主机若无法判断接上来的是什么，也就无法审计它。

| 字段 | 取值 |
|------|---------|
| 制造商（字符串 1） | `Victrl` |
| 产品（字符串 2） | `Victrl HID Bridge` |
| 序列号（字符串 3） | `VIC-` 加芯片 base MAC 的六个字节十六进制，例如 `VIC-3C71BF0A1B2C` |
| HID 接口（字符串 4） | `Victrl HID` |
| VID : PID | `0x303A` : `0x4001` |

`hid_device_init()` 用 `esp_read_mac(mac, ESP_MAC_BASE)` 生成序列号，并在调用 `tinyusb_driver_install()` **之前**写进 `s_usb_strings[3]`。顺序很关键：`descriptors_control.c` 是在 install 时把数组里的**指针**拷走的；而该缓冲是静态的，因此在设备整个生命周期内都有效。若 MAC 读取失败，则保留占位串 `0001` 并打一条警告。

此前每台设备都报固定的 `0001`，那只能识别型号、永远识别不出具体是哪一台。`CONFIG_TINYUSB_DESC_USE_ESPRESSIF_VID` 与 `CONFIG_TINYUSB_DESC_USE_DEFAULT_PID` 都处于开启状态，所以 `0x303A:0x4001` 是乐鑫默认值、与其他 ESP32 板子共用——真正用于识别某一台设备的是这些字符串，尤其是序列号。`sdkconfig` 里的 `CONFIG_TINYUSB_DESC_*` 字符串并不是主机实际收到的内容：固件传的是自己的 `s_usb_strings` 数组，而应用提供的数组优先级高于默认值。

## 绝对坐标映射

```
abs = (pixel × 32767) / screen_width
```

`pixel` 大于屏幕尺寸时会先被截断到屏幕尺寸（即钳位到右下角）；屏幕宽高为 0 时回退为默认的 1920×1080。

鼠标报告是**全量快照**：X/Y 是绝对字段，因此每次移动、点击、拖拽和滚轮都要重复当前位置与按键状态。模块内部维护 `s_last_x`、`s_last_y`、`s_last_buttons`，滚轮只改变 wheel 字节。

## 公开 API

| 函数 | 功能 |
|------|------|
| `hid_device_init()` | 初始化 TinyUSB，注册设备/配置/字符串描述符与 HID 报告描述符 |
| `hid_device_set_screen_size()` | 设置默认屏幕尺寸（仅影响 `hid_mouse_get_state()` 的回报值） |
| `hid_device_set_type_profile()` | 设置打字节奏（`hold_ms`、`gap_ms`、`jitter_ms`、`word_pause_ms`） |
| `hid_device_get_type_profile()` | 读取当前打字节奏 |
| `hid_mouse_move_abs()` | 绝对坐标移动（像素坐标 + 屏幕尺寸） |
| `hid_mouse_click()` | 点击：`left` / `right` / `middle` / `double_left` |
| `hid_mouse_down()` / `hid_mouse_up()` | 按下/释放按键（拖拽用） |
| `hid_mouse_scroll()` | 垂直滚轮（`delta_x` 暂不支持，见注意事项） |
| `hid_mouse_get_state()` | 回报当前指针绝对坐标、按住的按键与映射所用屏幕尺寸 |
| `hid_key_press()` | 按键组合，如 `ctrl+c`；最多 6 个非修饰键 |
| `hid_type_string()` | 输入 ASCII 文本，返回 `hid_type_result_t` |
| `hid_release_all()` | 释放所有键盘按键与鼠标按键 |
| `hid_keyboard_led_state()` | 最近一次主机输出报告的 LED 原始字节（bit0 Num、bit1 Caps、bit2 Scroll） |
| `hid_keyboard_led_seen()` | 主机是否已至少下发过一次输出报告 |
| `hid_type_set_digits_on_keypad()` | 数字是否走小键盘（默认关闭） |
| `hid_ensure_num_lock()` | 用主机的 LED 报告确认并（必要时）打开 Num Lock |

### 打字结果与打字节奏

`hid_type_string()` 返回 `hid_type_result_t`：`len`（输入字符串长度）、`queued`（成功入队的按下报告数）、`skipped`（无键位映射而被跳过的字符数）。`queued == len` 说明设备已把每个按键发到总线上，屏幕上仍然缺字或串字就是下游（多数是正在激活的中文输入法）造成的。

`hid_type_profile_t` 默认值：`hold_ms = 15`、`gap_ms = 35`、`jitter_ms = 25`、`word_pause_ms = 120`。按键间隔为 `gap_ms + (随机 0 ~ jitter_ms-1)`，空格额外加 `word_pause_ms`；`hold_ms` 为 0 时会被强制回 15。

## 子模块

| 文件 | 功能 |
|------|------|
| `hid_device.c` | TinyUSB 初始化、描述符、HID 操作实现、状态跟踪、打字节奏 |
| `hid_reports.c` | 底层报告发送：端点就绪等待、键盘/鼠标/滚轮报告 |
| `hid_keymap.c` | 键名 → HID 键码映射（字母、数字、F1~F12、编辑键、方向键、小键盘、符号等），修饰键检测，Shift 符号判定 |
| `hid_test.c` | 键盘与鼠标自检程序 `hid_run_self_test()` |
| `mouse_test.c` | 鼠标自检程序 `mouse_run_test()` |

## 注意事项

- 设备层用 `TINYUSB_PORT_FULL_SPEED_0`（USB OTG 1.1，Full Speed）承载 HID，以避开占用 OTG 0 的 MS2109 采集设备。
- 鼠标报告必须用 `tud_hid_n_report(0, 2, ...)` 直接传 report_id=2，不能用 `tud_hid_report(0, ...)`；键盘报告走 report_id=1。
- 每次发送前都会等端点就绪，超时为 `HID_REPORT_TIMEOUT_MS = 200` ms；未挂载时所有发送直接失败。
- 鼠标报告共尝试 5 次、相邻两次之间延时 2 ms；但每次都先等待端点就绪，单次等待上限是 200 ms，所以“5 次 × 2 ms”并不是它真正的时间开销。
- `hid_key_press()` 按住 50 ms 后释放；`hid_mouse_click()` 按下后保持 100 ms 再释放，`double_left` 是间隔 50 ms 的两次左键。
- `hid_type_string()` 发送失败时原地重试一次（间隔 20 ms），仍失败则记错误日志并跳过该字符；无键位映射的字符（非 ASCII/中文）直接计入 `skipped`。
- 按键间隔是刻意偏慢的人工节奏，可靠性优先于速度；打错一个字符的代价远大于省下的毫秒。
- 水平滚轮未在报告描述符中声明，`hid_mouse_scroll()` 的 `delta_x` 只打警告日志、不会发出；补上需要新增 AC Pan 字段。
- 数字可以改走小键盘路径以绕过会吞掉主键盘数字行的中文输入法：默认关闭，且必须先由 `hid_ensure_num_lock()` 确认 Num Lock 已打开，否则小键盘会发出方向键与 Home/End，比丢数字更糟。
- `hid_ensure_num_lock()` 在主机尚未下发过 LED 报告时直接返回 false（拒绝猜），确认打开时最多轮询 12 次 × 50 ms（约 600 ms）；小键盘数字键码为 0x59~0x61（`1`~`9`）与 0x62（`0`）。
- LED 输出报告通过 `tud_hid_set_report_cb()` 记录，Num/Caps/Scroll Lock 三种状态对设备可见，这是唯一的宿主键盘状态来源。
- `hid_keymap_lookup()` 对命名键做大小写不敏感匹配，对符号键做精确匹配；单个字母或数字即使不在表中也能自动映射。
- `hid_keymap.h` 中定义了右修饰键掩码 `MOD_RCTRL`、`MOD_RSHIFT`、`MOD_RALT`、`MOD_RGUI`，但组合键解析只识别 `ctrl`、`shift`、`alt`、`gui`/`win`，右修饰键目前不可达。
- `hid_test.c` 与 `mouse_test.c` 都会先等 USB 挂载（最多 150 × 100 ms，约 15 s），未挂载则跳过测试。
