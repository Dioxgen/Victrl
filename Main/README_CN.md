# Victrl

> [中文|[English](README.md)]

## 项目结构

```
victrl/
├── main.py                   # 入口，命令行参数解析
├── config.json                # 运行时配置（API、设备、路径）
├── config.py                  # 加载 config.json，导出常量
├── core/                      # 核心子系统
│   ├── agent.py               # 主循环：采集 → 推理 → 执行 → 记录
│   ├── uvc_capture.py         # 画面采集（OpenCV 优先，V4L2/ffmpeg 回退）
│   ├── v4l2_direct.py         # 纯 Python V4L2 ioctl/mmap（零额外依赖）
│   ├── hid_controller.py      # uinput 虚拟键鼠
│   ├── serial_hid.py          # ESP32 串口 HID 桥接
│   └── cloud_client.py        # LLM API 客户端 + 系统提示词
├── memory/                    # 三层记忆系统
│   ├── short_term.py          # L1: 最近动作历史（自动压缩）
│   ├── plan_manager.py        # L2: JSON 任务计划（断点续传）
│   └── profile_manager.py     # L3: Markdown 设备画像（跨任务累积知识）
├── api/
│   └── server.py              # Flask HTTP API (127.0.0.1:8080)
├── utils/
│   ├── logger.py              # 日志配置
│   ├── coordinates.py         # 归一化 ↔ 像素坐标转换
│   ├── system_utils.py        # 系统检测（uinput、video 设备）
│   └── exceptions.py          # 自定义异常类
├── esp32_hid/
│   └── esp32_hid.ino          # ESP32 固件（BLE 键盘+鼠标）
├── docs/
│   └── ms2109-capture-card.md # 采集卡调试指南
├── plans/                     # 计划文件（JSON）
├── profiles/                  # 设备画像（Markdown）
├── log/                       # 任务日志
└── requirements.txt
```

## 硬件准备

| 组件 | 说明 |
|------|------|
| Linux 主机 | Python 3.12 可用的 Linux (x86_64 / ARM)，运行 Victrl |
| USB 采集卡 | MacroSilicon MS2109 系列，HDMI → USB，MJPEG 输出 |
| ESP32 开发板 | 可选，用于 BLE HID 桥接（ESP32 对连被控设备蓝牙）或者可用 S3 等系列获得原生 OTG |
| HDMI 线 | 被控设备 → 采集卡 |

> 参考文献：[ZERO 系列 | Radxa Docs](https://docs.radxa.com/zero)

连线方式：

- HDMI：被控设备 HDMI 输出 → 采集卡 HDMI 输入
- USB 采集卡：采集卡 USB → Victrl 主机
- ESP32（可选）：Victrl 主机 USB → ESP32 串口；ESP32 BLE → 被控设备蓝牙

> 不使用 ESP32 时，请确保 Victrl 主机可以通过自身的 OTG 直连被控设备（uinput 模式启动）。

## 采集卡配置

在新 Linux 主机上首次使用 MS2109 采集卡时，**必须完成以下 4 步配置**，否则会遇到花屏、冻结、无画面等问题。

### 步骤 1：确认设备识别

```bash
# 应看到 MacroSilicon 设备
lsusb

# 应看到 /dev/video0 和 /dev/video1
ls -la /dev/video*

# 确认采集节点名称
cat /sys/class/video4linux/video0/name   # 输出: USB Video: USB Video
```

> **注意：** MS2109 会注册两个 `/dev/video` 节点。只用 `/dev/video0`（视频采集），`/dev/video1` 是元数据节点，没有视频输出。

### 步骤 2：检查视频格式

```bash
v4l2-ctl -d /dev/video0 --all
# 确认: Format Video Capture — 1920×1080 MJPG 30fps
# 确认: Video input: 0 (Camera 1: ok)
```

MS2109 在 1080p 分辨率下**只支持 MJPEG**，不支持 YUYV。Victrl 已自动配置 MJPEG 格式。

### 步骤 3：禁用 USB 自动挂起

MS2109 在 USB 空闲 **2 秒**后自动挂起。唤醒后视频管线断裂，输出的是**彩条测试图**而非实时画面。由于 LLM API 调用约需 30 秒，每一步都会触发此问题。

创建 udev 规则永久禁用自动挂起：

```bash
sudo tee /etc/udev/rules.d/99-ms2109-nosuspend.rules <<'EOF'
ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="534d", ATTR{idProduct}=="2109", ATTR{power/control}="on"
EOF

sudo udevadm control --reload-rules
sudo udevadm trigger
```

验证生效：

```bash
# 拔插采集卡后检查
find /sys/devices -name "idVendor" | xargs grep -l "534d" | head -1 | sed 's/idVendor/power\/control/' | xargs cat
# 应输出: on
```

### 步骤 4：验证画面采集

```bash
# 拍一张测试图
python3 -c "
import cv2, numpy as np
cap = cv2.VideoCapture('/dev/video0')
cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*'MJPG'))
cap.set(cv2.CAP_PROP_FRAME_WIDTH, 1920)
cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 1080)
ok, frame = cap.read()
if ok:
    is_bars = frame[:,:,0].std() > 100 and frame[:,:,1].std() > 100
    print('❌ 彩条测试图 — 检查HDMI信号或USB电源' if is_bars else '✅ 实时画面正常')
    cv2.imwrite('/tmp/test_capture.jpg', frame)
    print('图片已保存: /tmp/test_capture.jpg')
else:
    print('❌ 采集失败')
cap.release()
"
```

如果看到彩条：
- HDMI 信号断开 → 检查 HDMI 线连接和信号源是否开机
- USB 挂起未禁用 → 重新执行步骤 3
- 采集卡僵尸状态 → 物理拔插

## ESP32 固件烧录

```bash
# 1. 用 Arduino IDE 打开
#    esp32_hid/esp32_hid.ino

# 2. 选择开发板: ESP32 Dev Module

# 3. 烧录到 ESP32

# 4. 烧录完成后，ESP32 会作为 BLE 键盘+鼠标广播
#    在被控设备上搜索蓝牙设备 "Victrl HID" 并配对
```

固件特点：
- 零外部依赖，仅使用 ESP32 内置 BLE 库
- 串口协议 115200 baud，每行一条指令
- 配对一次后自动重连

## 安装

```bash
# 1. 安装 Python 依赖
pip install -r requirements.txt

# 2. 创建日志目录
mkdir -p ./log
```

## 快速开始

确保 `config.json` 中已填写 API 凭据和采集卡设备路径：

```bash
# 编辑配置
vim config.json

# 使用 ESP32 BLE HID 模式（推荐）
python main.py --hid-backend serial --serial-port /dev/ttyUSB0 --task "打开浏览器"

# 使用 uinput 模式（Linux 主机直连）
sudo modprobe uinput
python main.py --hid-backend uinput --task "打开设置"

# 恢复上次未完成的任务（无需 --task）
python main.py --hid-backend serial
```

### 常用参数

```
--task "任务描述"       要执行的任务
--hid-backend serial    HID 后端（serial = ESP32 BLE，uinput = 虚拟键鼠）
--serial-port PATH      ESP32 串口路径（默认 /dev/ttyUSB0）
--debug                 开启调试日志
--dry-run               模拟模式（mock 模型 + 无 HID）
--max-actions N         最大动作步数（默认 200）
```

所有运行时可配置项均在 `config.json` 中。

## HID 后端对比

| 后端 | 参数 | 原理 | 适用场景 |
|------|------|------|----------|
| serial | `--hid-backend serial` | 串口→ESP32→BLE HID→被控设备 | 任何支持蓝牙键鼠的设备 |
| uinput | `--hid-backend uinput` | Linux uinput 虚拟键鼠，OTG 直连 | 被控设备通过 USB 连接到 Victrl 主机 |

## HTTP API

Victrl 启动后自动开启 HTTP API 服务，支持远程控制：

```bash
# 查看状态
curl http://127.0.0.1:8080/status

# 启动任务
curl -X POST http://127.0.0.1:8080/start \
  -H 'Content-Type: application/json' \
  -d '{"task": "打开记事本，输入 Hello World"}'

# 停止任务
curl -X POST http://127.0.0.1:8080/stop

# 查看设备画像
curl http://127.0.0.1:8080/profile

# 查看当前计划
curl http://127.0.0.1:8080/plan
```

## 常见问题

### Q: 采集画面是彩条而不是桌面？

A: 两个原因：(1) USB 自动挂起 → 执行步骤 3 的 udev 规则；(2) HDMI 无信号 → 检查连线、分辨率、HDCP。

### Q: 模型打字出现乱码或错字？

A: 被控设备可能开启了中文输入法（IME）。IME 会将英文按键解释为拼音，产生乱码。模型已被告知注意此问题，会自动尝试切换输入法。

### Q: ESP32 按键无反应？

A: 确认被控设备已通过蓝牙配对 "Victrl HID"。配对一次后会自动重连。检查串口路径是否正确（`ls /dev/ttyUSB*`）。

### Q: 如何在新设备上从头开始？

A: 删除 `profiles/device_profile.md`，删除 `plans/` 下的旧计划文件。Victrl 会在第一次运行时重新学习新设备。

## 许可证

Apache 2.0
