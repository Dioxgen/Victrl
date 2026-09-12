# wifi_manager — WiFi 管理（ESP32-C6 协处理）

> **中文** | [English](README.md)

## 简介

本组件用标准 `esp_wifi_*()` API 管理 WiFi：ESP32-P4 自身没有 WiFi 硬件，这些调用由 `esp_wifi_remote` 经 ESP-Hosted SDIO 转发到 ESP32-C6 执行，组件代码里没有 C6 专有逻辑。`wifi_manager_init()` 依次初始化 `esp_netif`、默认事件循环与默认 STA netif，调用 `esp_wifi_init()`，注册 `WIFI_EVENT` 与 `IP_EVENT_STA_GOT_IP` 回调，以 `WIFI_MODE_STA`、`threshold.authmode = WIFI_AUTH_WPA2_PSK` 连接指定 AP，然后在事件组上最多等待 30s：拿到 IP 返回 `ESP_OK`，自动重连尝试耗尽返回 `ESP_FAIL`，超时返回 `ESP_ERR_TIMEOUT`。`ssid` 为空或 NULL 时不启用 WiFi，直接返回 `ESP_OK`。

时间同步不由 `wifi_manager_init()` 启动，而是由调用方在拿到 IP 之后调用 `wifi_manager_start_sntp()`。它把服务器交给 lwIP、调用 `esp_sntp_init()` 后立即返回，同步在后台完成，结果由同步回调写入 `time_status_t`，再用 `wifi_manager_time_synced()` / `wifi_manager_get_time_status()` 查询：慢或不可达的 NTP 服务器不会再阻塞启动。

## 公开 API

| 接口 | 说明 |
|---|---|
| `esp_err_t wifi_manager_init(const char *ssid, const char *password)` | 初始化并连接 STA。`ssid` 为空或 NULL 时打印 `No SSID configured — WiFi disabled` 并返回 `ESP_OK`；已初始化过则返回 `ESP_OK`。连接成功返回 `ESP_OK`，自动重连耗尽返回 `ESP_FAIL`，30s 内既未连上也未耗尽重试返回 `ESP_ERR_TIMEOUT`。 |
| `esp_err_t wifi_manager_get_ip(char *ip_out, size_t ip_len)` | 取出当前 IP 字符串，尚未拿到 IP 时为 `0.0.0.0`。`ip_out` 为 NULL 或 `ip_len` 为 0 返回 `ESP_ERR_INVALID_ARG`。 |
| `bool wifi_manager_is_connected(void)` | 是否已连接，依据最近一次获得的 IP 判断。 |
| `int8_t wifi_manager_get_rssi(void)` | 已连接 AP 的信号强度，单位 dBm；未连接或读取失败时返回 0。 |
| `esp_err_t wifi_manager_get_mac(char *out, size_t out_len)` | 输出 `aa:bb:cc:dd:ee:ff` 形式的 STA MAC。`out_len < 18` 返回 `ESP_ERR_INVALID_ARG`；读取失败时写入 `n/a` 并返回 `ESP_FAIL`。 |
| `esp_err_t wifi_manager_start_sntp(const char *const *servers)` | 以 `SNTP_OPMODE_POLL` 启动后台 SNTP。`servers` 是 NULL 结尾数组，至少 1 个、最多 `CONFIG_LWIP_SNTP_MAX_SERVERS` 个。已启动返回 `ESP_OK`；WiFi 未连接返回 `ESP_ERR_INVALID_STATE`；`servers` 为 NULL 或 `servers[0]` 为空返回 `ESP_ERR_INVALID_ARG`。 |
| `bool wifi_manager_time_synced(void)` | 是否至少成功同步过一次。 |
| `void wifi_manager_get_time_status(time_status_t *out)` | 拷贝当前同步状态；`out` 为 NULL 时什么都不做。 |

### time_status_t 字段

| 字段 | 说明 |
|---|---|
| `started` | SNTP 引擎已启动。 |
| `synced` | 至少成功同步过一次。 |
| `sync_count` | 开机以来成功同步的次数。 |
| `failures` | 预留给失败回调的计数，当前实现从不写入，恒为 0。 |
| `last_sync_us` | 上次同步时刻的 `esp_timer` 时间戳（µs），0 表示从未同步。 |
| `servers[3][48]`、`n_servers` | 实际生效的服务器名与实际个数。 |
| `tz_applied` | 时区环境变量已设置。 |

## 注意事项

- ESP32-P4 没有 WiFi 硬件，WiFi 完全由 C6 提供，且 C6 必须预先烧录 ESP-Hosted 固件；`esp_wifi_remote` 与 `esp_hosted` 的传输配置（SDIO）不在本组件内，由工程配置决定。
- 重连没有退避：`WIFI_EVENT_STA_DISCONNECTED` 里立刻再次 `esp_wifi_connect()`，最多 5 次（`s_retry_count < 5`），仍失败则置 `WIFI_FAIL_BIT` 并打印 `WiFi connection failed after 5 retries`；计数在拿到 IP 后清零。首次连接由 `WIFI_EVENT_STA_START` 触发，不计入这 5 次。
- 只设置了 `threshold.authmode = WIFI_AUTH_WPA2_PSK`，因此目标 AP 至少要支持 WPA2-PSK；开放网络与 WPA/WEP 会被拒绝。
- `wifi_manager_is_connected()` 只看 `s_ip_addr`，而断开事件不会清空它：掉线后该函数可能仍然返回 `true`，直到重连成功或复位。
- `ssid` 为空时 `s_initialized` 始终为 false，`wifi_manager_get_rssi()` 因此一直返回 0。
- SNTP 需要调用方自己启动：`wifi_manager_init()` 不做这件事，WiFi 未连接时 `wifi_manager_start_sntp()` 返回 `ESP_ERR_INVALID_STATE`；本工程在 `main.c` 里检测到已连接才调用一次，失败只记日志、不重试。
- 服务器数量上限是 `CONFIG_LWIP_SNTP_MAX_SERVERS`（本工程 `sdkconfig.defaults` 设为 3），第 1 个必需，超出的条目会被 lwIP 忽略；本工程 `main.c` 依次传入 `ntp.aliyun.com`、`cn.pool.ntp.org`、`pool.ntp.org`。
- 同步是异步的、不阻塞启动；本工程未设置同步间隔，也没开 `CONFIG_LWIP_SNTP_STARTUP_DELAY`，因此首个请求不会被随机延迟。
- 时区在第一次同步成功的回调里设置：`setenv` 把 `TZ` 设为 `CST-8`，随后调用 `tzset()`，即固定 UTC+8（中国标准时间），没有按区域配置的入口；同步成功前 `localtime()` 的时区偏移不可信。
