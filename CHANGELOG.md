# Changelog

> **English** | [中文](CHANGELOG_CN.md)

A concise record of what changed and when. Dates here are real dates, not commit dates: the ESP32-P4 work was done in a separate workspace and imported into this repository on 2026-09-12, so several commits carry September dates while the work they contain is older.

## 2026-09 — the project is revived on a faster multimodal model

The original bring-up was latency-bound: the API accounted for 64–91 % of every step, and a step costs roughly its output tokens divided by the model's throughput. The project was paused rather than tuned, because the dominant term was not ours to optimise.

It was restarted once the current model generation — DeepSeek V4.1 flash, `model_name: "deepseek-flash"` — made the loop practical: much higher output throughput, and native vision, which removes the OCR or image-description layer the design would otherwise need before it could see a screen at all. Nothing in the harness had to change to accommodate it, which is the point of keeping the model a stateless function behind one config key.

## 2026-09-12 — component documentation audit

### Fixed

`wifi_manager`: the STA address is cleared the moment the link drops, so `wifi_manager_is_connected()` (which reads `s_ip_addr`) no longer reports a live connection — and the LCD and the WebUI no longer show a stale address — after a disconnect.

`wifi_manager`: SNTP server names are copied only into the slots `time_status_t.servers` really has, and `n_servers` never exceeds what was copied, so raising `CONFIG_LWIP_SNTP_MAX_SERVERS` above 3 can no longer write past the struct — `web_server.c` indexes that array up to `n_servers`.

`storage_manager`: `stm_add()` no longer writes one element past the end of the short-term-memory array when the oldest-two merge allocation fails; it drops the oldest entry instead. A failed `strdup()` is also no longer stored as a NULL hole that every reader would walk with `strlen()`.

`storage_manager`: `meta.json` keeps its `created` field across the per-step rewrites, so a session no longer loses its creation time the first time its step counter is updated.

`agent_core`: "step once" now actually stops after one iteration, and skips the inter-step sleep. The STEP to PAUSED transition was unreachable — `agent_should_continue()` is true for STEP — so a single step behaved exactly like RUN and never paused.

`sdmmc_driver`: the mount-failure log names the Kconfig option that exists (`CONFIG_SDMMC_DRV_FORMAT_IF_MOUNT_FAILED`).

### Changed

All 12 component READMEs were audited against their sources and rewritten; each component now carries an English `README.md` beside its `README_CN.md`. The audit corrected claims that no longer matched the code — among them an invented `jpeg_get_info()`, an invented `ESP_COLOR_FOURCC_BGR24`, an invented `sd_access_mutex`, a JPEG quality default of 60 rather than 25, fonts described in points rather than pixels, and stale UVC stream fields.

The root READMEs now point at the per-component documentation.

### Removed

Three stale `README.md.old` files.

### Known issues, recorded and deliberately not changed

`NV3007_driver`: in the `F9/F2 fix` block, `NV3007_WriteByte(0x17)` sends a bare data byte with no preceding register write, so `0xF9` is never addressed — most likely a missing `C8(0xF9,0x17)`. The panel initialises and runs correctly as it stands, so the sequence was left untouched.

`wifi_manager`: `time_status_t.failures` is exposed in `/api/status` but never incremented. lwIP's SNTP has no failure callback, so a real value would need a "started but never synced" watchdog. The field is left in place, but treat a zero as "not measured" rather than "no failures".

## 2026-09-11 — the input-method channel experiments

The numeric-keypad hypothesis was falsified: on Microsoft Pinyin the keypad digits are consumed as candidate-selection keys, exactly like the main number row. The experiment is recorded in `docs/Technical-Document.md` along with the reason it took so long to settle — the `type` action summary had to start recording the **channel** it used before "the keypad was eaten too" could be told apart from "the keypad was never tried".

## 2026-05-30 — the agent loop runs on the ESP32-P4

The harness moved off the PC and onto the chip. UVC capture through an MS2109 card, the P4 hardware JPEG codec, TinyUSB composite HID output, SD-card storage, ESP-Hosted WiFi on the C6, the NV3007 status panel and the WebUI all run on the device; the PC is no longer part of the loop.

Line endings were pinned to LF with `.gitattributes` so the system prompts on the SD card stay byte-identical between checkouts — the API prefix cache depends on exactly that.

## 2026-05 — the Linux/Python prototype (V2.0)

The original MVP: the agent loop on a PC, driving a target machine through a USB capture card and a HID device. It is the source of the failure taxonomy and most of the input-method findings, and it was the first proof that the loop works at all. Kept for reference under `prototype/`; see `docs/Prototype.md` for what moving it onto the chip cost.
