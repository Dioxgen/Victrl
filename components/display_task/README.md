# display_task — LCD status display

> **English** | [中文](README_CN.md)

## Overview

A FreeRTOS task that shows Agent status on the NV3007 SPI LCD (landscape 428×142); the task is named `lcd_refresh`, runs at priority 2 with a 3072-byte stack. On start it calls `NV3007_Init()` itself and clears the panel, then redraws the whole screen every 2 seconds or as soon as `display_task_notify()` wakes it (every round begins with `NV3007_FastFill(COLOR_BG)` before all content is redrawn).

## What is displayed

The font is fixed at 12 pixels high (`FONT_SM` → `ascii_1206`, 6×12 pixels per character); row and column anchors are converted by the `NX(v)` and `NY(v)` normalization macros, while the row pitch is a fixed 14 pixels.

| Position | Content | Source | Color |
|------|------|------|------|
| row 1, y=2, x=0 | `IP:<ip>` (`IP:%.15s`, at most 15 characters) | `wifi_manager_get_ip()`; while disconnected this is its initial value `0.0.0.0` | white |
| row 1, x=136 | `Step:<action_count>`, or `Step:<action_count> <current_milestone>/<total>` when the plan's `milestones` array is non-empty | `agent->action_count`, plus `milestones` and `plan_update.current_milestone` from `plan_mgr_get_current()` | cyan |
| row 1, x=248 | `RUN`, `PAUSE`, `STEP`, `STOP`, `EMERG`, and `IDLE` for any other state | `agent_get_state()` | green for RUNNING, yellow for PAUSED, red for EMERGENCY, white otherwise |
| row 2, y=25, x=0 | task name, at most 55 bytes | `agent->task_goal`; nothing is drawn when it is empty | white |
| rows 3–7, y=39/53/67/81/95 | the 5 most recent short-term memory entries, at most 50 bytes each | `stm_get_all_formatted()`, split on `\n` with the last 5 lines kept | white |

## Refresh mechanism

- The loop ends with `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000))`: a 2-second timeout refresh, and `display_task_notify()` wakes it immediately through `xTaskNotifyGive()`.
- When `agent_get_global()` returns `NULL`, the task only waits 2 seconds and retries without drawing.
- `plan_mgr_get_current()` returns a duplicate, which this component releases with `cJSON_Delete()`; the string returned by `stm_get_all_formatted()` is released with `free()`.

## Notes

- The caller only needs to call `display_task_init()`; `NV3007_Init()` and the first screen clear both happen inside `display_task()`, so repeated initialization in `main` is not needed.
- The task-name row is truncated with `utf8_copy(task, 56, agent->task_goal)`: at most 55 bytes, and a UTF-8 character is never split. Log rows are still truncated by bytes — `strncpy` to 54 bytes and then a hard cut at 50 bytes — so they can split a multi-byte character.
- `NV3007_ShowString()` draws only bytes in the `' '`–`'~'` range and stops at the first byte outside it, and the font carries no CJK glyphs, so Chinese task names and Chinese log lines are not shown.
- Log splitting uses `strtok_r` rather than `strtok`, because the HID task also tokenizes strings and the global cursor of `strtok` is shared between tasks.
- The normalization macros apply to the row and column anchors only; the 12-pixel glyph height and the 14-pixel row pitch are fixed values, so another resolution does not scale them.
- The panel is on `NV3007_SPI_HOST` (default `SPI2_HOST`) while the SD card uses the sdmmc host `SDMMC_HOST_SLOT_0`, so the two do not share a bus.
- `FONT_MD` (16) is defined but unused.
- `action_count` and `task_goal` are read directly from the `agent_ctx_t` fields; only the state goes through the mutex inside `agent_get_state()`.
- `display_task_init()` returns `ESP_ERR_NO_MEM` when `xTaskCreate()` fails; `main.c` does not check the return value.
- `display_task_notify()` has no call site anywhere in the repository.
