# The Prototype — Victrl on Linux, in Python

> **English** | [中文](原型.md)

*Historical. Frozen. Not the code in this repository.*

Before any of this ran on a microcontroller, the same idea ran on a PC. The prototype was the cheapest way to answer one question: **can a model looking at a video capture of a screen, driving it through an emulated keyboard and mouse, actually complete tasks?** If the answer had been no, there would have been no point building hardware for it.

## The two implementations, side by side

They are **not one codebase**. The second is a re-implementation, not a port of shared code. The repository contains only the second one.

| | **Prototype** | **Current** |
|---|---|---|
| Where | Linux PC (Ubuntu) | **ESP32-P4, no operating system** |
| Language | Python | C (ESP-IDF 6.0.1 + FreeRTOS) |
| Screen capture | V4L2 / OpenCV / ffmpeg on `/dev/video0` | UVC host driver, MJPEG 1920×1080 |
| HID output | serial bridge → ESP32 BLE HID, or Linux `uinput` | **native TinyUSB composite** (keyboard + absolute mouse) |
| Cloud calls | Ark SDK (Doubao-seed) | stateless HTTPS, Responses-style API |
| Memory | three layers (L1 history / L2 plan / L3 profile) on disk | SD-card sessions: trajectory + plan + profile |
| Control surface | Flask HTTP API on `:8080` | **on-device WebUI** (dashboard, sessions, preview, file browser) |
| Size / power | a PC | one board, one capture card, one SD card |
| Status | **frozen, historical** | actively developed |

## Shape

```
┌─ Victrl host: a Linux PC ──────────────────────────────────────────┐
│                                                                    │
│  /dev/video0 ──▶ UvcCapture ──▶ agent.py ──▶ CloudClient ──▶ LLM   │
│  (capture card)   OpenCV/V4L2      │            (Ark SDK)           │
│                                    │                               │
│                                    ├──▶ memory: L1 history          │
│                                    │            L2 plan             │
│                                    │            L3 profile          │
│                                    │                               │
│                                    ├──▶ serial bridge ──▶ ESP32 ──▶ BLE HID
│                                    └──▶ uinput (fallback) ──▶ USB HID
│                                                                    │
│  Flask HTTP API :8080  ◀── control / status                        │
└────────────────────────────────────────────────────────────────────┘
```

| Module | Role |
|---|---|
| `core/agent.py` | the main loop |
| `core/cloud_client.py` | model calls (Ark SDK, Doubao-seed) |
| `core/uvc_capture.py`, `core/v4l2_direct.py` | frame capture, two paths |
| `core/hid_controller.py`, `core/serial_hid.py` | keyboard/mouse synthesis |
| `memory/short_term.py`, `plan_manager.py`, `profile_manager.py` | L1 / L2 / L3 |
| `utils/coordinates.py` | normalised ↔ pixel mapping |
| `api/server.py` | Flask control API |

## What it proved

- **The loop works.** Capture → model → JSON action → HID → re-capture, repeatedly, on real tasks.
- **A model can be driven entirely by pixels and a text status line.** No target-side integration of any kind was needed, on any of Windows, Linux, macOS or Android.
- **The state belongs in three layers, not one.** Transient history, the current plan, and durable facts about *this machine* have different lifetimes and different costs, and mixing them makes the agent worse. That conclusion carried over to the MCU version unchanged — today it is the trajectory, the plan, and the device profile.
- **The HID side is the fragile side.** Almost none of the difficulty was in the model; it was in making keystrokes and clicks land exactly where they were aimed.

## Why it was retired

Nothing was wrong with it. It answered its question and then became a liability:

- **It could not make the claim the project is about.** "An AI agent that automates anything" is much weaker when it needs a PC, a Linux install, and a serial bridge to run. The claim that matters is that the agent is *a device*.
- **Latency was dominated by the round trip, so the PC bought nothing.** Every step is 3–7 seconds of waiting on a model. Running the loop on a desktop CPU was never the bottleneck.
- **Two implementations is one too many.** Keeping a Python version current alongside the firmware means every protocol change, every prompt change and every coordinate fix has to be made twice, and the second one is always the one that rots.

The prototype is therefore frozen at its last version and kept for reference only. Its architecture is not a template for the current code; the current code re-derived the same structure from the constraints of the chip.

## What carried over, and what did not

| Idea | Fate |
|---|---|
| See → act → observe loop | carried over, essentially unchanged |
| Normalised coordinates, one mapping function | carried over, and made stricter |
| Three-layer memory | carried over in spirit: trajectory / plan / profile |
| Text status line as ground truth | carried over and greatly expanded — it now reports cursor position, lock keys, screen-change, and input-method toggle count |
| Serial HID bridge to an ESP32 | **dropped** — the MCU does HID natively now |
| Flask control API | **replaced** by an on-device WebUI |
| Python's tolerance for sloppy resource handling | **dropped, painfully** — see [MCU Port](MCU-Port.md) |
