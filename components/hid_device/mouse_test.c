#include "mouse_test.h"

#include <math.h>
#include "hid_device.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mouse_test";

#include "device/usbd.h"

void mouse_run_test(void)
{
    ESP_LOGI(TAG, "=== Mouse Test ===");

    /* Wait for USB to be enumerated by host */
    ESP_LOGI(TAG, "Waiting for USB mount...");
    int timeout = 150;
    while (timeout-- > 0) {
        if (tud_mounted()) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "USB mounted=%d", tud_mounted());
    if (!tud_mounted()) {
        ESP_LOGW(TAG, "USB not mounted — test skipped");
        return;
    }

    uint32_t w = 1920, h = 1080;
    uint32_t cx = w / 2, cy = h / 2;
    int radius = 200;

    /* Move to center first */
    hid_mouse_move_abs(cx, cy, w, h);
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Circle */
    for (int angle = 0; angle < 360; angle += 15) {
        float rad = angle * 3.14159f / 180.0f;
        uint32_t x = cx + (uint32_t)(radius * cosf(rad));
        uint32_t y = cy + (uint32_t)(radius * sinf(rad));
        hid_mouse_move_abs(x, y, w, h);
        vTaskDelay(pdMS_TO_TICKS(20));
        ESP_LOGI(TAG, "move to (%lu,%lu)", x, y);
    }

    /* Click test */
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "Left click at center");
    hid_mouse_click("left");
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGI(TAG, "Right click at (200,500)");
    hid_mouse_move_abs(200, 500, w, h);
    vTaskDelay(pdMS_TO_TICKS(200));
    hid_mouse_click("right");
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Drag test */
    ESP_LOGI(TAG, "Drag from (500,500) to (800,500)");
    hid_mouse_move_abs(500, 500, w, h);
    vTaskDelay(pdMS_TO_TICKS(100));
    hid_mouse_down("left");
    for (int step = 1; step <= 8; step++) {
        uint32_t dx = 500 + (300 * step) / 8;
        hid_mouse_move_abs(dx, 500, w, h);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    hid_mouse_up("left");

    /* Scroll */
    ESP_LOGI(TAG, "Scroll down then up");
    hid_mouse_scroll(0, -3);
    vTaskDelay(pdMS_TO_TICKS(500));
    hid_mouse_scroll(0, 3);

    hid_release_all();
    ESP_LOGI(TAG, "=== Mouse Test Complete ===");
}
