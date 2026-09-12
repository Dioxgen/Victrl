# Victrl — Wiki

Victrl is a **Hardware AI Agent**: a single device, independent of the system it controls, that
automates any machine the way a person does — look at the screen, press the keys.

```
capture the display  →  press keyboard/mouse  →  observe the result
```

No API, no accessibility layer, no ADB/VNC/RDP, no driver, nothing installed on the target. It
attaches as an ordinary display sink plus a standard USB keyboard and mouse, so it works on
anything with a video output and a USB port — including BIOS screens, installers, and old or closed
machines that no software agent can reach.

## The two implementations

This project exists in two forms. **They are not one codebase** — the second is a re-implementation,
not a port of shared code. The repository contains only the second one.

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

| Page | What it covers |
|---|---|
| [Prototype](Prototype) | The Linux/Python implementation: what it was, what it proved, and why it was retired |
| [MCU Port](MCU-Port) | The current ESP32-P4 implementation, and what moving the harness onto a chip actually cost |
| [Failure Taxonomy](Failure-Taxonomy) | The most transferable result: how a harness tells "never arrived" from "was reinterpreted" |
| [Input Method Troubles](Input-Method-Troubles) | The longest and most expensive bug in the project's history |

## The one design decision that defines everything

**The agent loop runs on the device.** The LLM is a stateless function called over HTTPS; the loop,
the memory, the plan, the status line, the verification and the failure classification all live on
the board.

That is not a packaging choice. A cloud agent framework keeps its memory on a server, so the agent
*is* the server. Here the agent is the object sitting on the desk: it carries its own state, sees
through its own capture card, and acts through its own keyboard. The model is a component it calls,
not the thing it is.

## Reading order

1. The [README](../) — what it is, the measured numbers, and the compliance requirements. Read the
   compliance notes before connecting anything to anything.
2. The [technical documentation](../blob/main/docs/技术文档.md) — architecture and the full decision
   record (Chinese).
3. This wiki — the engineering history, i.e. *why* the code looks the way it does.

## A note on what is published here

Both the successes and the failures are written down, including the ones that took days. A hardware
demo that only shows the happy path is not evidence of anything, and the failure write-ups are the
part most likely to be useful to someone building something similar.
