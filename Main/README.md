> [[中文](README_CN.md)|English]

This folder contains all the software components of the Victrl MVP version.

## Architecture

```
Target Device HDMI → Capture Card USB → Victrl Host → LLM API → Victrl Host → Serial → ESP32 BLE Keyboard/Mouse Emulation → Target Device
```

## Hardware Preparation

| Component | Description |
|-----------|-------------|
| Linux Host | Linux system (x86_64 / ARM) with Python 3.12 |
| USB Capture Card | MacroSilicon MS2109 series |
| ESP32 Dev Board | Optional, for BLE HID bridge (ESP32 connects to target device via Bluetooth), or use S3 series for native OTG |
| HDMI Cable | Target device → Capture card |

> Reference: [ZERO Series | Radxa Docs](https://docs.radxa.com/zero)

Connection:
- HDMI: Target device HDMI output → Capture card HDMI input
- USB Capture Card: Capture card USB → Victrl host
- ESP32 (optional): Victrl host USB → ESP32 serial port; ESP32 BLE → Target device Bluetooth

> When not using ESP32, ensure the Victrl host can connect directly to the target device via its own OTG (start in uinput mode).

## Installation

### Victrl Host:

```bash
# 1. Enable uinput (only needed for uinput backend)
sudo modprobe uinput

# 2. Install Python dependencies
pip install -r requirements.txt

# 3. Create log directory
sudo mkdir -p /var/log/victrl
sudo chown $USER:$USER /var/log/victrl
```

### ESP32:

Flash the `esp32_hid/esp32_hid.ino` program to the ESP32 dev board. This program only uses the ESP32 core's built-in Bluetooth Low Energy libraries and requires no additional third-party Arduino libraries.

## Quick Start

```bash
# Real mode — uses config.json values by default
python main.py --task "Open browser and navigate to github.com/Dioxgen/Victrl"

# With ESP32 BLE HID backend
python main.py --hid-backend serial --serial-port /dev/ttyUSB0 --task "Open browser and navigate to github.com/Dioxgen/Victrl"

# Resume latest plan (no --task)
python main.py
```

Configuration is located in the `config.json` file — API credentials, device paths, and timeout settings are all in this file.

## HID

| Backend | Flag | How |
|---------|------|-----|
| uinput | `--hid-backend uinput` | Linux uinput virtual keyboard/mouse, connects directly to target device via OTG |
| serial | `--hid-backend serial` | Controls ESP32 via serial port, ESP32 connects to target device via BLE HID |

## HTTP API

```bash
curl http://127.0.0.1:8080/status
curl -X POST http://127.0.0.1:8080/start -H 'Content-Type: application/json' \
  -d '{"task":"Open terminal and run htop"}'
curl -X POST http://127.0.0.1:8080/stop
curl http://127.0.0.1:8080/profile
curl http://127.0.0.1:8080/plan
```

## File Structure

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

## Agent Loop

```
① Capture screen — OpenCV captures 1920×1080 MJPG frames from /dev/video0
② Assemble context — System prompt + device profile + current milestone + last 5 history entries + screenshot
③ Call LLM — POST to API, model analyzes the screen, decides the next action
④ Execute action — Parse JSON: click / type / press / scroll / drag / wait
⑤ Update memory — Action summary → L1, milestones → L2, new discoveries → L3
⑥ Check completion — force capture when done=true, model evaluates completion before exiting
```

## Memory System

| Layer | Storage | Purpose | Lifespan |
|-------|---------|---------|----------|
| L1 | Memory (list) | Last 10 action summaries | Single task |
| L2 | JSON file | Milestones + progress, interruptible/resumable | Single task |
| L3 | Markdown file | UI element positions, shortcuts, experience | Cross-task accumulation |

## Model Response Structure

The JSON format returned by the model for each step:

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
