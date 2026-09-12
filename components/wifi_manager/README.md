# wifi_manager — WiFi management (ESP32-C6 co-processor)

> **English** | [中文](README_CN.md)

## Overview

This component manages WiFi through the standard `esp_wifi_*()` API: the ESP32-P4 has no WiFi hardware of its own, so those calls are forwarded to the ESP32-C6 by `esp_wifi_remote` over ESP-Hosted SDIO, and the component itself contains no C6-specific logic. `wifi_manager_init()` initialises `esp_netif`, the default event loop and the default STA netif in turn, calls `esp_wifi_init()`, registers the `WIFI_EVENT` and `IP_EVENT_STA_GOT_IP` handlers, connects to the given AP with `WIFI_MODE_STA` and `threshold.authmode = WIFI_AUTH_WPA2_PSK`, then waits on an event group for at most 30s: `ESP_OK` once the IP arrives, `ESP_FAIL` when the automatic reconnect attempts are exhausted, `ESP_ERR_TIMEOUT` on timeout. An empty or NULL `ssid` disables WiFi and returns `ESP_OK` immediately.

Time synchronisation is a separate step that `wifi_manager_init()` never starts: the caller invokes `wifi_manager_start_sntp()` once it has an IP. That function hands the servers to lwIP, calls `esp_sntp_init()` and returns at once; the sync happens in the background and its result is written into `time_status_t` by the notification callback, to be read with `wifi_manager_time_synced()` / `wifi_manager_get_time_status()` — so a slow or unreachable NTP server no longer stalls startup.

## Public API

| API | Description |
|---|---|
| `esp_err_t wifi_manager_init(const char *ssid, const char *password)` | Initialise and connect the STA. An empty or NULL `ssid` logs `No SSID configured — WiFi disabled` and returns `ESP_OK`; a second call returns `ESP_OK` as well. Returns `ESP_OK` on a successful connection, `ESP_FAIL` when the automatic reconnects are exhausted, and `ESP_ERR_TIMEOUT` if neither happened within 30s. |
| `esp_err_t wifi_manager_get_ip(char *ip_out, size_t ip_len)` | Copy the current IP string; it is `0.0.0.0` until an IP is obtained. Returns `ESP_ERR_INVALID_ARG` if `ip_out` is NULL or `ip_len` is 0. |
| `bool wifi_manager_is_connected(void)` | Whether the station is connected, based on the most recent IP. |
| `int8_t wifi_manager_get_rssi(void)` | Signal strength of the connected AP in dBm; 0 when not connected or when the read fails. |
| `esp_err_t wifi_manager_get_mac(char *out, size_t out_len)` | Write the STA MAC as `aa:bb:cc:dd:ee:ff`. Returns `ESP_ERR_INVALID_ARG` if `out_len < 18`; on a failed read it writes `n/a` and returns `ESP_FAIL`. |
| `esp_err_t wifi_manager_start_sntp(const char *const *servers)` | Start background SNTP with `SNTP_OPMODE_POLL`. `servers` is a NULL-terminated array with at least 1 and at most `CONFIG_LWIP_SNTP_MAX_SERVERS` entries. Returns `ESP_OK` if already started, `ESP_ERR_INVALID_STATE` if WiFi is not connected, `ESP_ERR_INVALID_ARG` if `servers` is NULL or `servers[0]` is empty. |
| `bool wifi_manager_time_synced(void)` | Whether at least one sync has succeeded. |
| `void wifi_manager_get_time_status(time_status_t *out)` | Copy the current sync state; does nothing when `out` is NULL. |

### `time_status_t` fields

| Field | Description |
|---|---|
| `started` | The SNTP engine is running. |
| `synced` | At least one sync has succeeded. |
| `sync_count` | Successful syncs since boot. |
| `failures` | Counter reserved for a failure callback; the current implementation never writes it, so it is always 0. |
| `last_sync_us` | `esp_timer` timestamp of the last sync (µs); 0 means never synced. |
| `servers[3][48]`, `n_servers` | The server names actually in effect and how many there are. |
| `tz_applied` | The timezone environment variable has been set. |

## Notes

- The ESP32-P4 has no WiFi hardware, so WiFi comes entirely from the C6, and the C6 must have ESP-Hosted firmware flashed in advance; the `esp_wifi_remote` / `esp_hosted` transport configuration (SDIO) is not part of this component and is decided by the project configuration.
- Reconnects have no backoff: `WIFI_EVENT_STA_DISCONNECTED` calls `esp_wifi_connect()` again immediately, at most 5 times (`s_retry_count < 5`), after which `WIFI_FAIL_BIT` is set and `WiFi connection failed after 5 retries` is logged; the counter is cleared once an IP is obtained. The first connection is triggered by `WIFI_EVENT_STA_START` and does not count towards those 5.
- Only `threshold.authmode = WIFI_AUTH_WPA2_PSK` is set, so the target AP must support at least WPA2-PSK; open networks and WPA/WEP are rejected.
- `wifi_manager_is_connected()` looks only at `s_ip_addr`, and the disconnect event does not clear it: after losing the link the function can still return true until a reconnect succeeds or the device resets.
- With an empty `ssid`, `s_initialized` stays false, so `wifi_manager_get_rssi()` keeps returning 0.
- SNTP must be started by the caller: `wifi_manager_init()` does not do it, and `wifi_manager_start_sntp()` returns `ESP_ERR_INVALID_STATE` while WiFi is not connected; this project calls it once from `main.c` after seeing a connection, and only logs on failure rather than retrying.
- The server count is capped by `CONFIG_LWIP_SNTP_MAX_SERVERS` (`3` in this project's `sdkconfig.defaults`); the first entry is required and anything beyond the cap is ignored by lwIP. This project's `main.c` passes `ntp.aliyun.com`, `cn.pool.ntp.org` and `pool.ntp.org` in that order.
- Syncing is asynchronous and does not block startup; this project sets no sync interval and does not enable `CONFIG_LWIP_SNTP_STARTUP_DELAY`, so the first request is not delayed by a random amount.
- The timezone is set inside the callback for the first successful sync: `setenv` sets `TZ` to `CST-8` and `tzset()` follows, i.e. a fixed UTC+8 (China Standard Time) with no per-region configuration hook; before the first successful sync the offset used by `localtime()` cannot be trusted.
