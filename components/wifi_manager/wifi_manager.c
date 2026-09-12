#include "wifi_manager.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "wifi_mgr";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_events = NULL;
static int s_retry_count = 0;
static char s_ip_addr[32] = "0.0.0.0";
static bool s_initialized = false;

static time_status_t s_time = {0};

/* ── SNTP ─────────────────────────────────────────────────────────────── */

static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    s_time.synced = true;
    s_time.sync_count++;
    s_time.last_sync_us = esp_timer_get_time();

    /* TZ is applied here rather than at start: setting it before the first
     * sync is harmless but this guarantees the offset is in effect for the very
     * first localtime() call after the clock becomes valid. */
    setenv("TZ", "CST-8", 1);
    tzset();
    s_time.tz_applied = true;

    time_t now = 0;
    time(&now);
    struct tm t;
    localtime_r(&now, &t);
    ESP_LOGI(TAG, "Time synchronised: %04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
}

esp_err_t wifi_manager_start_sntp(const char *const *servers)
{
    if (s_time.started) {
        ESP_LOGW(TAG, "SNTP already running");
        return ESP_OK;
    }
    if (!wifi_manager_is_connected()) {
        ESP_LOGW(TAG, "SNTP not started: WiFi is not connected");
        return ESP_ERR_INVALID_STATE;
    }
    if (!servers || !servers[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, servers[0]);

    /* lwIP only stores CONFIG_LWIP_SNTP_MAX_SERVERS entries. Anything beyond
     * that used to be dropped in silence, which is exactly how a single
     * unreachable server ended up being the only one ever tried. */
    s_time.n_servers = 1;
    for (int i = 1; i < CONFIG_LWIP_SNTP_MAX_SERVERS; i++) {
        if (!servers[i]) break;
        esp_sntp_setservername(i, servers[i]);
        s_time.n_servers++;
    }
    for (int i = 0; i < s_time.n_servers; i++) {
        snprintf(s_time.servers[i], sizeof(s_time.servers[i]), "%s", servers[i]);
    }

    esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();
    s_time.started = true;

    ESP_LOGI(TAG, "SNTP started with %u server(s): %s%s%s (max %d)",
             s_time.n_servers,
             s_time.servers[0],
             s_time.n_servers > 1 ? ", " : "",
             s_time.n_servers > 1 ? s_time.servers[1] : "",
             CONFIG_LWIP_SNTP_MAX_SERVERS);
    return ESP_OK;
}

bool wifi_manager_time_synced(void)
{
    return s_time.synced;
}

void wifi_manager_get_time_status(time_status_t *out)
{
    if (out) *out = s_time;
}

int8_t wifi_manager_get_rssi(void)
{
    if (!s_initialized) return 0;
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

esp_err_t wifi_manager_get_mac(char *out, size_t out_len)
{
    if (!out || out_len < 18) return ESP_ERR_INVALID_ARG;
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) {
        snprintf(out, out_len, "n/a");
        return ESP_FAIL;
    }
    snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return ESP_OK;
}


static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < 5) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "WiFi disconnected, retry %d/5", s_retry_count);
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "WiFi connection failed after 5 retries");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_addr, sizeof(s_ip_addr),
                 IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "WiFi connected, IP: %s", s_ip_addr);
    }
}

esp_err_t wifi_manager_init(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) {
        ESP_LOGI(TAG, "No SSID configured — WiFi disabled");
        return ESP_OK;
    }

    if (s_initialized) {
        ESP_LOGW(TAG, "WiFi already initialized");
        return ESP_OK;
    }

    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) return ESP_ERR_NO_MEM;

    /* Init TCP/IP stack */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    /* Init WiFi (routed to C6 via esp_wifi_remote + esp_hosted SDIO) */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %d", err);
        return err;
    }

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to %s via C6...", ssid);

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected successfully");
        s_initialized = true;
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Connection failed");
        return ESP_FAIL;
    } else {
        ESP_LOGW(TAG, "Connection timed out");
        return ESP_ERR_TIMEOUT;
    }
}

esp_err_t wifi_manager_get_ip(char *ip_out, size_t ip_len)
{
    if (!ip_out || ip_len == 0) return ESP_ERR_INVALID_ARG;
    strncpy(ip_out, s_ip_addr, ip_len - 1);
    ip_out[ip_len - 1] = '\0';
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return s_ip_addr[0] != '0';
}
