# Victrl

> [中文|[English](log.md)]

当前最新版本：V2.1

## Victrl V1.0

2026.05.15 构思

2026.05.18 完成

## Victrl V2.0

2026.05.19 完成

###   新增：

  - ESP32 屏幕分辨率由主机动态下发，不再硬编码
  - 图像文件名追加时间戳（step_0001_162309.jpg）
  - Agent 主循环每步重新打开 VideoCapture，彻底解决 MS2109 长时间空闲后返回过期帧的问题
  - 三层记忆系统：L1 短期历史 / L2 JSON 计划文件（支持中断恢复）/ L3 Markdown 设备画像（跨任务累积）
  - MS2109 采集卡调试文档，覆盖 USB 自动挂起、彩条诊断、僵尸设备恢复

###   修复：

  - ESP32 固件 keyMap[] PROGMEM bug —— 多字符键名（enter、tab、backspace）查找失败，按键无反应；移除 PROGMEM，改为 RAM 直接访问
  - ESP32 固件 K 指令 asciiToHid() 类型混淆 —— HID_SPACE (0x2C) 被当作 ASCII 逗号处理，空格输出为 ,；Python 侧空格改为走 T 指令规避
  - BLE HID 打字丢字符 —— 原 5ms/字符 (~143 chars/s) 超出 BLE GATT 吞吐，改为 25ms/字符 (~40 chars/s)，逐字符 K 指令发送
  - 中文输入法干扰 —— IME 将英文按键解释为拼音，输出全角标点和乱码；Python 侧显式 shift 映射所有符号，系统提示词增加 IME 检测规则
  - 模型响应缺少 done 字段导致全响应被丢弃 —— done 改为可选字段，缺省 false
  - MS2109 USB 自动挂起 —— 空闲 2 秒后视频管线断裂输出彩条；增加 udev 规则 + 运行时防御

## Victrl V2.1

2026.05.20 完成

### 新增：

优化提示词

log 中添加鼠标坐标与 action

### 修复：

修复 uinput/OTG 模式逻辑

修复鼠标逻辑：

| 文件                      | 改动                                                        | 原因                         |
| ------------------------- | ----------------------------------------------------------- | ---------------------------- |
| `esp32_hid/esp32_hid.ino` | 相对→绝对鼠标；`lastAbsX`/`lastAbsY`/`lastButtons` 状态跟踪 | 坐标定位不准、拖拽断连       |
| `core/agent.py`           | `drag` 默认 `hold=0`（自动释放）；拖拽分 8 步平滑移动       | 旧默认 `-1` 导致按钮永不释放 |
| `core/cloud_client.py`    | `hold` 字段加说明 `// ms to hold before releasing`          | 模型不知道 `hold` 含义       |