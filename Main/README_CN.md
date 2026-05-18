> [中文|[English](README.md)]

此文件夹是 Vicrtl MVP 版本的所有软件部分。

## 架构

```
被控设备 HDMI→ 采集卡 USB→ Victrl 主机 → LLM API Victrl 主机 → 串口 ESP32 BLE 模拟键鼠 → 被控设备
```

## 硬件准备

| 组件 | 说明 |
|------|------|
| Linux 主机 | Python 3.12 可用的 Linux (x86_64 / ARM) |
| USB 采集卡 | MacroSilicon MS2109 系列 |
| ESP32 开发板 | 可选，用于 BLE HID 桥接（ESP32 对连被控设备蓝牙）或者可用 S3 等系列获得原生 OTG |
| HDMI 线 | 被控设备 → 采集卡 |

> 参考文献：[ZERO 系列 | Radxa Docs](https://docs.radxa.com/zero)

连线方式：
- HDMI：被控设备 HDMI 输出 → 采集卡 HDMI 输入
- USB 采集卡：采集卡 USB → Victrl 主机
- ESP32（可选）：Victrl 主机 USB → ESP32 串口；ESP32 BLE → 被控设备蓝牙

> 不使用 ESP32 时，请确保 Victrl 主机可以通过自身的 OTG 直连被控设备（uinput 模式启动）。

## 安装

### Victrl 主机：

```bash
# 1. Enable uinput (only needed for uinput backend)
sudo modprobe uinput

# 2. Install Python dependencies
pip install -r requirements.txt

# 3. Create log directory
sudo mkdir -p /var/log/victrl
sudo chown $USER:$USER /var/log/victrl
```

### ESP32：

将 `esp32_hid/esp32_hid.ino` 程序烧录至 ESP32 开发板。该程序仅使用 ESP32 内核自带的蓝牙低功耗库，无需依赖任何额外的 Arduino 第三方库。

## 快速开始

```bash
# Real mode — uses config.json values by default
python main.py --task "Open browser and navigate to github.com/Dioxgen/Victrl"

# With ESP32 BLE HID backend
python main.py --hid-backend serial --serial-port /dev/ttyUSB0 --task "Open browser and navigate to github.com/Dioxgen/Victrl"

# Resume latest plan (no --task)
python main.py
```

配置信息位于 `config.json` 文件中，接口凭证、设备路径以及超时设置均在此文件内。

## HID

| Backend | Flag | How |
|---------|------|-----|
| uinput | `--hid-backend uinput` | Linux uinput 虚拟键鼠，通过 OTG 直连被控设备 |
| serial | `--hid-backend serial` | 通过串口控制 ESP32，ESP32 以 BLE HID 连接被控设备 |

## HTTP API

```bash
curl http://127.0.0.1:8080/status
curl -X POST http://127.0.0.1:8080/start -H 'Content-Type: application/json' \
  -d '{"task":"Open terminal and run htop"}'
curl -X POST http://127.0.0.1:8080/stop
curl http://127.0.0.1:8080/profile
curl http://127.0.0.1:8080/plan
```

## 文件结构

```
victrl/
├── main.py                   # Entry point, CLI arguments
├── config.json               # Primary configuration (all runtime settings)
├── config.py                 # Loads config.json, exports constants
├── core/                     # Core subsystems
│   ├── agent.py              # Main agent loop (capture → query → execute)
│   ├── uvc_capture.py        # Screen capture (OpenCV primary, V4L2/ffmpeg fallback)
│   ├── v4l2_direct.py        # Pure Python V4L2 ioctl/mmap (no ffmpeg dependency)
│   ├── hid_controller.py     # uinput keyboard/mouse
│   ├── serial_hid.py         # ESP32 serial HID bridge
│   └── cloud_client.py       # LLM API client + system prompt
├── memory/                   # Three-layer memory system
│   ├── short_term.py         # L1: Recent action history (auto-compressing)
│   ├── plan_manager.py       # L2: Task plans as JSON files (checkpoint/resume)
│   └── profile_manager.py    # L3: Device profile in Markdown (cross-task knowledge)
├── api/
│   └── server.py             # Flask HTTP API at 127.0.0.1:8080
├── utils/
│   ├── logger.py             # Logging configuration
│   ├── coordinates.py        # Normalized ↔ pixel coordinate conversion
│   ├── system_utils.py       # Hardware checks (uinput, video devices)
│   └── exceptions.py         # Custom exception classes
├── esp32_hid/
│   └── esp32_hid.ino         # ESP32 firmware (BLE keyboard + mouse, zero external libs)
├── plans/                    # Plan file storage (JSON)
├── profiles/                 # Device profile storage (Markdown)
└── requirements.txt
```

## Agent 循环

```
① 抓屏 — OpenCV 从 /dev/video0 采集 1920×1080 MJPG 帧
② 组装上下文 — 系统提示词 + 设备画像 + 当前 milestone + 最近 5 条历史 + 屏幕截图
③ 调用 LLM — POST 到 API，模型分析画面，决定下一步动作
④ 执行动作 — 解析 JSON：click / type / press / scroll / drag / wait
⑤ 更新记忆 — 动作摘要 → L1，milestones → L2，新发现 → L3
⑥ 检查完成 — done=true 时强制抓屏验证，模型评估完成度后方可退出
```

## 记忆系统

| 层 | 存储 | 作用 | 生命周期 |
|----|------|------|----------|
| L1 | 内存 (list) | 最近 10 条动作摘要 | 单次任务 |
| L2 | JSON 文件 | milestones + 进度，可中断恢复 | 单次任务 |
| L3 | Markdown 文件 | UI 元素位置、快捷键、经验 | 跨任务累积 |

## 模型响应结构

模型每步返回的 JSON 格式数据：

```json
{
  "action_type": "click|type|press|scroll|drag|wait|release|complete|error",
  "box_2d": [ymin, xmin, ymax, xmax],    // normalized [0,1]
  "observation": "What I see on screen and why I chose this action",
  "plan_update": {
    "summary": "Current state description",
    "milestones": [                         // direction, not specific steps
      {"id": 1, "description": "Open the target app", "status": "in_progress"}
    ]
  },
  "need_screen": true,
  "done": false,
  "verification": "What on screen proves the goal is achieved"   // required when done
}
```

## License

Apache 2.0
