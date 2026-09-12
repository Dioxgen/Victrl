#include "hid_reports.h"

#include "device/usbd.h"
#include "class/hid/hid_device.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

__attribute__((unused)) static const char *TAG = "hid_report";

/*
 * Report sending. The report descriptor declares:
 *   Keyboard: report ID 1
 *   Mouse:    report ID 2
 * tud_hid_keyboard_report(report_id, ...) and tud_hid_n_report(...) take the
 * report_id as the first parameter (instance is always 0 internally).
 *
 * Every send below first waits for the endpoint to be ready. TinyUSB returns
 * false when the previous report is still queued, and dropping a report means
 * dropping a keystroke or a click — so waiting is always preferable to
 * retrying blindly or ignoring the result.
 */

bool hid_wait_ready(uint32_t timeout_ms)
{
    if (!tud_mounted()) return false;

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (!tud_hid_ready()) {
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) return false;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return true;
}

bool hid_send_keyboard_report(uint8_t modifiers, const uint8_t keycodes[6])
{
    if (!tud_mounted()) return false;
    if (!hid_wait_ready(HID_REPORT_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "keyboard report dropped: endpoint busy >%dms",
                 HID_REPORT_TIMEOUT_MS);
        return false;
    }
    return tud_hid_keyboard_report(1, modifiers, keycodes);
}

bool hid_send_abs_mouse_report(uint8_t buttons, uint16_t x, uint16_t y)
{
    if (!tud_mounted()) return false;

    uint8_t report[6] = {
        buttons & 0x07, x & 0xFF, (x>>8) & 0xFF, y & 0xFF, (y>>8) & 0xFF, 0
    };
    for (int retry = 0; retry < 5; retry++) {
        if (hid_wait_ready(HID_REPORT_TIMEOUT_MS) &&
            tud_hid_n_report(0, 2, report, sizeof(report))) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
}

bool hid_send_mouse_scroll_report(uint8_t buttons, uint16_t x, uint16_t y,
                                  int8_t wheel)
{
    if (!tud_mounted()) return false;

    /* X/Y are absolute: repeating them keeps the cursor where it is. Sending
     * zeros here (as this used to) snapped the pointer to (0,0) on every
     * scroll event. */
    uint8_t report[6] = {
        buttons & 0x07, x & 0xFF, (x>>8) & 0xFF, y & 0xFF, (y>>8) & 0xFF,
        (uint8_t)wheel
    };
    for (int retry = 0; retry < 5; retry++) {
        if (hid_wait_ready(HID_REPORT_TIMEOUT_MS) &&
            tud_hid_n_report(0, 2, report, sizeof(report))) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
}
