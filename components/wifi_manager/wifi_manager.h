#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_manager_init(const char *ssid, const char *password);
esp_err_t wifi_manager_get_ip(char *ip_out, size_t ip_len);
bool wifi_manager_is_connected(void);

/*
 * Signal strength of the connected AP, in dBm (0 when not connected).
 * Handy for judging whether the link is the reason a request was slow.
 */
int8_t wifi_manager_get_rssi(void);

/* Station MAC address as "aa:bb:cc:dd:ee:ff", or "n/a" if unavailable. */
esp_err_t wifi_manager_get_mac(char *out, size_t out_len);

/* ── Time synchronisation ─────────────────────────────────────────────── */

/*
 * SNTP state, for display and diagnostics.
 *
 * Sync is deliberately asynchronous: blocking app_main() for 30s waiting for a
 * server meant a slow or unreachable NTP host stalled the whole boot. The
 * device now starts SNTP, carries on, and syncs whenever it happens — so the
 * status has to be observable rather than inferred from a boot-time wait.
 */
typedef struct {
    bool     started;        /* SNTP engine is running                      */
    bool     synced;         /* at least one successful sync                */
    uint32_t sync_count;     /* successful syncs since boot                 */
    uint32_t failures;       /* callback-reported failures                  */
    int64_t  last_sync_us;   /* esp_timer timestamp of the last sync, 0=none */
    char     servers[3][48]; /* configured server names ("" when unused)     */
    uint8_t  n_servers;
    bool     tz_applied;     /* TZ env var has been set                     */
} time_status_t;

/*
 * Start SNTP in the background. Safe to call once after the STA interface has
 * an IP; does nothing if WiFi is not connected or SNTP already runs.
 *
 * `servers` is a NULL-terminated array; up to CONFIG_LWIP_SNTP_MAX_SERVERS
 * entries are used. Extra entries are ignored by lwIP itself, so this also
 * reports how many were actually accepted.
 */
esp_err_t wifi_manager_start_sntp(const char *const *servers);

bool wifi_manager_time_synced(void);
void wifi_manager_get_time_status(time_status_t *out);

#ifdef __cplusplus
}
#endif
