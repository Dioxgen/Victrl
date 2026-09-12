# button_driver — GPIO button driver

> **English** | [中文](README_CN.md)

## Overview

A button driver built on GPIO interrupts plus FreeRTOS task notifications: GPIO0 and GPIO1 share a single `btn_task`, which decides short versus long press from the hold time when the button is released and drives the global Agent state through the `agent_core` API.

## Pin definitions

| GPIO | Macro | Configuration | Function |
|------|-----|------|------|
| GPIO0 | `BTN_START` | input, internal pull-up, `GPIO_INTR_ANYEDGE` | start the task |
| GPIO1 | `BTN_PAUSE` | input, internal pull-up, `GPIO_INTR_ANYEDGE` | pause/resume, stop |

Both pins enable the internal pull-up, so a press reads low (0) and a release reads high (1).

## Button behaviour

| GPIO | Hold time at release | Action |
|------|----------------|------|
| GPIO0 | < `LONG_PRESS_MS` (3000 ms) | start the default task via `agent_start()` |
| GPIO0 | >= 3000 ms | no action (the source has no long-press branch) |
| GPIO1 | < 3000 ms | `agent_pause()` when the state is `AGENT_STATE_RUNNING`, `agent_resume()` when it is `AGENT_STATE_PAUSED`, nothing otherwise |
| GPIO1 | >= 3000 ms | `agent_stop()` when the state is `AGENT_STATE_RUNNING` or `AGENT_STATE_PAUSED`, nothing otherwise |

Short versus long press is decided at release; nothing fires while the button is held.

On a short press GPIO0 passes a fixed, hardcoded task string to `agent_start()`, with no external input:

```c
agent_start(agent, "Execute the default task");
```

## How it works

1. `button_driver_init()` uses `gpio_config()` to set both pins to input with the internal pull-up enabled and any-edge interrupts, then calls `gpio_install_isr_service(0)` and registers `btn_isr` per pin with `gpio_isr_handler_add()`.
2. `btn_isr` is marked `IRAM_ATTR` and uses `xTaskNotifyFromISR()` with `eSetValueWithOverwrite` to hand the GPIO number of the interrupting pin to `s_btn_task` as the notification value, then calls `portYIELD_FROM_ISR()`.
3. `btn_task` waits for a notification with `xTaskNotifyWait()` and a `pdMS_TO_TICKS(100)` timeout; after receiving one it always waits a fixed `vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS))` (50 ms) and then samples the pin level.
4. A level of 0 on a pin that was not previously pressed records the press timestamp and sets `pressed[]`; a level of 1 on a pin that was previously pressed counts as a release, and the `xTaskGetTickCount()` difference multiplied by `portTICK_PERIOD_MS` gives the hold time in milliseconds.
5. On release it calls the global Agent API obtained from `agent_get_global()` according to the table above; if that function returns `NULL` the event is dropped. The test `gpio == BTN_START` maps to `press_time[0]`/`pressed[0]`, and anything else to index 1.

## Notes

- `btn_task` runs at priority 8 with a 6144-byte (6 KB) stack. A source comment records that the stack was raised to this value from the original 2 KB because a button press now reaches `agent_start()` (session creation, cJSON, file writes, logging), a much deeper call chain than the old `plan_mgr_new_task()`; confirm that path before shrinking the stack.
- Debouncing is a fixed `DEBOUNCE_MS` (50 ms) delay after the notification followed by a level sample, not an adaptive filter; bounce or pulses shorter than that window are dropped.
- The long-press threshold is `LONG_PRESS_MS` (3000 ms). Only GPIO1 uses the long-press branch; a long press on GPIO0 produces no action at all.
- The notification uses `eSetValueWithOverwrite`, so the notification value keeps only the last GPIO number; the task does not rely on that value for the level but re-samples it with `gpio_get_level()`.
- Both buttons share the same task and the same `press_time`/`pressed` state.
- The ISR does nothing but call `xTaskNotifyFromISR()`; no other work happens in interrupt context.
- `button_driver_init()` returns `ESP_ERR_NO_MEM` if `xTaskCreate()` fails; the return value of `gpio_install_isr_service()` is not checked.
