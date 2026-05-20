# Victrl

> [[中文](log_CN.md)|English]

Current latest version: V2.1

## Victrl V1.0

2026.05.15 Conception

2026.05.18 Completion

## Victrl V2.0

2026.05.19 Completion

###   New Features:

  - ESP32 screen resolution dynamically issued by the host, no longer hardcoded
  - Image filenames now include timestamps (step_0001_162309.jpg)
  - Agent main loop reopens VideoCapture on each step, completely resolving the issue of MS2109 returning stale frames after extended idle
  - Three-layer memory system: L1 short-term history / L2 JSON plan file (supports interrupt/resume) / L3 Markdown device profile (cross-task accumulation)
  - MS2109 capture card debugging documentation, covering USB auto-suspend, color bar diagnostics, and zombie device recovery

###   Fixes:

  - ESP32 firmware keyMap[] PROGMEM bug — multi-character key names (enter, tab, backspace) failed lookup, keys unresponsive; removed PROGMEM, switched to direct RAM access
  - ESP32 firmware K command asciiToHid() type confusion — HID_SPACE (0x2C) was treated as ASCII comma, outputting space as , ; Python side now routes spaces through T command to avoid this
  - BLE HID typing character loss — original 5ms/char (~143 chars/s) exceeded BLE GATT throughput, changed to 25ms/char (~40 chars/s), sends K command per character
  - Chinese IME interference — IME interprets English keystrokes as pinyin, outputting full-width punctuation and garbled text; Python side explicitly maps all symbols through shift, system prompt adds IME detection rules
  - Model response missing done field causing entire response to be discarded — done made optional, defaults to false
  - MS2109 USB auto-suspend — video pipeline breaks after 2 seconds idle, outputting color bars; added udev rule + runtime defense

## Victrl V2.1

2026.05.20 Completion

### New Features:

  - Optimized prompts
  - Added mouse coordinates and actions to log

### Fixes:

Fixed mouse logic:

| File                      | Change                                                                                  | Reason                                           |
| ------------------------- | --------------------------------------------------------------------------------------- | ------------------------------------------------ |
| `esp32_hid/esp32_hid.ino` | Relative→absolute mouse; `lastAbsX`/`lastAbsY`/`lastButtons` state tracking             | Inaccurate coordinate positioning, drag disconnection |
| `core/agent.py`           | `drag` defaults to `hold=0` (auto-release); drag split into 8 smooth movement steps     | Old default `-1` caused button to never release  |
| `core/cloud_client.py`    | Added `hold` field comment `// ms to hold before releasing`                             | Model didn't know the meaning of `hold`          |