# Changelog

> **English** | [中文](CHANGELOG_CN.md)

A concise record of what changed and when. Dates are commit dates, not release dates — this project has no release cadence, and nothing here is claimed to be hardware-tested unless the entry says so.

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

## 2026-09-12 — the agent loop runs on the ESP32-P4

The harness moved off the PC and onto the chip. UVC capture through an MS2109 card, the P4 hardware JPEG codec, TinyUSB composite HID output, SD-card storage, ESP-Hosted WiFi on the C6, the NV3007 status panel and the WebUI all run on the device; the PC is no longer part of the loop.

The Linux/Python MVP was demoted to `prototype/` and kept as reference. See `docs/Prototype.md` for what the port cost.

Line endings were pinned to LF with `.gitattributes` so the system prompts on the SD card stay byte-identical between checkouts — the API prefix cache depends on exactly that.

## 2026-09-12 — documentation set

Every document exists as an English/Chinese pair: `Technical-Document` / `技术文档`, `Failure-Taxonomy` / `失败分类学`, `Input-Method-Troubles` / `输入法问题`, `Prototype` / `原型`, `MCU-Port` / `MCU移植`, `Compliance` / `合规与授权说明`.

Hardware photos were added under `Images/`, the architecture diagram was rewritten in Mermaid-8-compatible syntax, and the wiki was folded into `docs/`.

## 2026-05 — the Linux/Python prototype (V2.0)

The original MVP: the agent loop on a PC, driving a target machine through a USB capture card and a HID device. It is the source of the failure taxonomy and most of the input-method findings. Kept for reference under `prototype/`.
