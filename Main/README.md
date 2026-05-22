# Victrl

> [[中文](README_CN.md)|English]

## Project Structure

```
victrl/
├── main.py                   # Entry point, CLI argument parsing
├── config.json                # Runtime configuration (API, device, paths)
├── config.py                  # Loads config.json, exports constants
├── core/                      # Core subsystems
│   ├── agent.py               # Main loop: capture → inference → execute → record
│   ├── uvc_capture.py         # Screen capture (OpenCV primary, V4L2/ffmpeg fallback)
│   ├── v4l2_direct.py         # Pure Python V4L2 ioctl/mmap (zero extra dependencies)
│   ├── hid_controller.py      # uinput virtual keyboard/mouse
│   ├── serial_hid.py          # ESP32 serial HID bridge
│   └── cloud_client.py        # LLM API client + system prompt
├── memory/                    # Three-layer memory system
│   ├── short_term.py          # L1: Recent action history (auto-compressing)
│   ├── plan_manager.py        # L2: JSON task plans (checkpoint/resume)
│   └── profile_manager.py     # L3: Markdown device profile (cross-task accumulated knowledge)
├── api/
│   └── server.py              # Flask HTTP API (127.0.0.1:8080)
├── utils/
│   ├── logger.py              # Logging configuration
│   ├── coordinates.py         # Normalized ↔ pixel coordinate conversion
│   ├── system_utils.py        # System checks (uinput, video devices)
│   └── exceptions.py          # Custom exception classes
├── esp32_hid/
│   └── esp32_hid.ino          # ESP32 firmware (BLE keyboard + mouse)
├── docs/
│   └── ms2109-capture-card.md # Capture card debugging guide
├── plans/                     # Plan files (JSON)
├── profiles/                  # Device profiles (Markdown)
├── log/                       # Task logs
└── requirements.txt
```

## Hardware Preparation

It is recommended to use MS2109 acquisition card and ESP32 Dev Module.

| Component | Description |
|-----------|-------------|
| Linux Host | Linux system (x86_64 / ARM) with Python 3.12, runs Victrl |
| USB Capture Card | MacroSilicon MS2109 series, HDMI → USB, MJPEG output |
| ESP32 Dev Board | Optional, for BLE HID bridge (ESP32 connects to target device via Bluetooth), or use S3 series for native OTG |
| HDMI Cable | Target device → Capture card |

> Reference: [ZERO Series | Radxa Docs](https://docs.radxa.com/zero)

Connection:

- HDMI: Target device HDMI output → Capture card HDMI input
- USB Capture Card: Capture card USB → Victrl host
- ESP32 (optional): Victrl host USB → ESP32 serial port; ESP32 BLE → Target device Bluetooth

> When not using ESP32, ensure the Victrl host can connect directly to the target device via its own OTG (start in uinput mode).

## Capture Card Configuration

When using an MS2109 capture card on a new Linux host for the first time, **all 4 steps below must be completed**, otherwise you will encounter artifacts, freezing, or no video.

### Step 1: Verify Device Recognition

```bash
# Should see the MacroSilicon device
lsusb

# Should see /dev/video0 and /dev/video1
ls -la /dev/video*

# Confirm capture node name
cat /sys/class/video4linux/video0/name   # Output: USB Video: USB Video
```

> **Note:** MS2109 registers two `/dev/video` nodes. Only use `/dev/video0` (video capture); `/dev/video1` is a metadata node with no video output.

### Step 2: Check Video Format

```bash
v4l2-ctl -d /dev/video0 --all
# Confirm: Format Video Capture — 1920×1080 MJPG 30fps
# Confirm: Video input: 0 (Camera 1: ok)
```

MS2109 **only supports MJPEG** at 1080p, not YUYV. Victrl is already configured to use MJPEG format.

### Step 3: Disable USB Auto-Suspend

MS2109 auto-suspends after **2 seconds** of USB idle. After wake-up, the video pipeline is broken and outputs a **color bar test pattern** instead of the live feed. Since LLM API calls take ~30 seconds, this issue triggers on every step.

Create a udev rule to permanently disable auto-suspend:

```bash
sudo tee /etc/udev/rules.d/99-ms2109-nosuspend.rules <<'EOF'
ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="534d", ATTR{idProduct}=="2109", ATTR{power/control}="on"
EOF

sudo udevadm control --reload-rules
sudo udevadm trigger
```

Verify it's active:

```bash
# After re-plugging the capture card, check
find /sys/devices -name "idVendor" | xargs grep -l "534d" | head -1 | sed 's/idVendor/power\/control/' | xargs cat
# Should output: on
```

### Step 4: Verify Screen Capture

```bash
# Take a test shot
python3 -c "
import cv2, numpy as np
cap = cv2.VideoCapture('/dev/video0')
cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*'MJPG'))
cap.set(cv2.CAP_PROP_FRAME_WIDTH, 1920)
cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 1080)
ok, frame = cap.read()
if ok:
    is_bars = frame[:,:,0].std() > 100 and frame[:,:,1].std() > 100
    print('❌ Color bars — check HDMI signal or USB power' if is_bars else '✅ Live feed OK')
    cv2.imwrite('/tmp/test_capture.jpg', frame)
    print('Image saved: /tmp/test_capture.jpg')
else:
    print('❌ Capture failed')
cap.release()
"
```

If you see color bars:
- HDMI signal lost → Check HDMI connection and ensure the source is powered on
- USB suspend not disabled → Re-do Step 3
- Capture card in zombie state → Physically re-plug

## ESP32 Firmware Flashing

```bash
# 1. Open with Arduino IDE
#    esp32_hid/esp32_hid.ino

# 2. Select board: ESP32 Dev Module

# 3. Flash to ESP32

# 4. After flashing, ESP32 will broadcast as a BLE keyboard + mouse
#    On the target device, search for Bluetooth device "Victrl HID" and pair
```

Firmware features:
- Zero external dependencies, only uses ESP32 built-in BLE libraries
- Serial protocol at 115200 baud, one command per line
- Auto-reconnects after initial pairing

## Installation

```bash
# 1. Install Python dependencies
pip install -r requirements.txt

# 2. Create log directory
mkdir -p ./log
```

## Quick Start

Make sure `config.json` contains the API credentials and capture card device path:

```bash
# Edit configuration, it is recommended to use the Doubao-seed series APIs
vim config.json

# Use ESP32 BLE HID mode (recommended)
python main.py --hid-backend serial --serial-port /dev/ttyUSB0 --task "Open browser"

# Use uinput mode (Linux host direct connection)
sudo modprobe uinput
python main.py --hid-backend uinput --task "Open settings"

# Resume an unfinished task (no --task needed)
python main.py --hid-backend serial
```

### Common Parameters

```
--task "task description"       The task to execute
--hid-backend serial            HID backend (serial = ESP32 BLE, uinput = virtual HID)
--serial-port PATH              ESP32 serial port path (default /dev/ttyUSB0)
--debug                         Enable debug logging
--dry-run                       Simulation mode (mock model + no HID)
--max-actions N                 Max action steps (default 200)
```

All runtime-configurable options are in `config.json`.

## HID Backend Comparison

| Backend | Flag | Principle | Use Case |
|---------|------|-----------|----------|
| serial | `--hid-backend serial` | Serial→ESP32→BLE HID→Target | Any device supporting Bluetooth keyboard/mouse |
| uinput | `--hid-backend uinput` | Linux uinput virtual HID, OTG direct | Target device connected via USB to Victrl host |

## HTTP API

Victrl automatically starts an HTTP API service after launch for remote control:

```bash
# Check status
curl http://127.0.0.1:8080/status

# Start a task
curl -X POST http://127.0.0.1:8080/start \
  -H 'Content-Type: application/json' \
  -d '{"task": "Open Notepad, type Hello World"}'

# Stop the task
curl -X POST http://127.0.0.1:8080/stop

# View device profile
curl http://127.0.0.1:8080/profile

# View current plan
curl http://127.0.0.1:8080/plan
```

## FAQ

### Q: Captured image shows color bars instead of the desktop?

A: Two possible causes: (1) USB auto-suspend → apply the udev rule from Step 3; (2) No HDMI signal → check connections, resolution, and HDCP.

### Q: Model typing produces garbled or incorrect characters?

A: The target device may have a Chinese IME activated. The IME interprets English keystrokes as pinyin, producing garbled text. The model has been informed of this issue and will automatically attempt to switch input methods.

### Q: ESP32 keys are not responding?

A: Verify that the target device has paired with "Victrl HID" via Bluetooth. After initial pairing, it reconnects automatically. Check the serial port path (`ls /dev/ttyUSB*`).

### Q: How to start fresh on a new device?

A: Delete `profiles/device_profile.md` and remove old plan files from `plans/`. Victrl will re-learn the new device on the first run.

## License

Apache 2.0
