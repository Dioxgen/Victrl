#include "hid_test.h"

#include "hid_device.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hid_test";

static void test_keyboard_chars(void)
{
    ESP_LOGI(TAG, "--- Keyboard: letters ---");
    hid_type_string("abcdefghijklmnopqrstuvwxyz");
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "--- Keyboard: uppercase ---");
    hid_type_string("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "--- Keyboard: numbers ---");
    hid_type_string("0123456789");
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "--- Keyboard: symbols ---");
    hid_type_string("!@#$%^&*()_+-=[]{}|;:',.<>?/`~");
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "--- Keyboard: space/enter/tab ---");
    hid_type_string("hello world\tfrom\tvictrl\n");
    vTaskDelay(pdMS_TO_TICKS(200));
}

static void test_keyboard_special(void)
{
    ESP_LOGI(TAG, "--- Keyboard: special keys ---");
    hid_key_press("f1");
    vTaskDelay(pdMS_TO_TICKS(100));
    hid_key_press("escape");
    vTaskDelay(pdMS_TO_TICKS(100));
    hid_key_press("enter");
    vTaskDelay(pdMS_TO_TICKS(100));
    hid_key_press("tab");
    vTaskDelay(pdMS_TO_TICKS(100));
    hid_key_press("backspace");
    vTaskDelay(pdMS_TO_TICKS(100));
    hid_key_press("ctrl+a");
    vTaskDelay(pdMS_TO_TICKS(100));
    hid_key_press("ctrl+c");
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void test_mouse_move(void)
{
    ESP_LOGI(TAG, "--- Mouse: move to corners (1920x1080) ---");
    uint32_t w = 1920, h = 1080;

    /* Center */
    hid_mouse_move_abs(w/2, h/2, w, h);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Top-left */
    hid_mouse_move_abs(0, 0, w, h);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Top-right */
    hid_mouse_move_abs(w, 0, w, h);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Bottom-right */
    hid_mouse_move_abs(w, h, w, h);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Bottom-left */
    hid_mouse_move_abs(0, h, w, h);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Back to center */
    hid_mouse_move_abs(w/2, h/2, w, h);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Cross pattern */
    ESP_LOGI(TAG, "--- Mouse: cross pattern ---");
    for (int i = 0; i < 10; i++) {
        uint32_t x = (w * i) / 10;
        hid_mouse_move_abs(x, h/2, w, h);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    for (int i = 0; i < 10; i++) {
        uint32_t y = (h * i) / 10;
        hid_mouse_move_abs(w/2, y, w, h);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void test_mouse_buttons(void)
{
    ESP_LOGI(TAG, "--- Mouse: buttons ---");

    hid_mouse_move_abs(200, 200, 1920, 1080);
    vTaskDelay(pdMS_TO_TICKS(100));
    hid_mouse_click("left");
    vTaskDelay(pdMS_TO_TICKS(200));

    hid_mouse_move_abs(400, 200, 1920, 1080);
    vTaskDelay(pdMS_TO_TICKS(100));
    hid_mouse_click("right");
    vTaskDelay(pdMS_TO_TICKS(200));

    hid_mouse_move_abs(600, 200, 1920, 1080);
    vTaskDelay(pdMS_TO_TICKS(100));
    hid_mouse_click("middle");
    vTaskDelay(pdMS_TO_TICKS(200));

    hid_mouse_move_abs(800, 200, 1920, 1080);
    vTaskDelay(pdMS_TO_TICKS(100));
    hid_mouse_click("double_left");
    vTaskDelay(pdMS_TO_TICKS(200));
}

static void test_mouse_drag(void)
{
    ESP_LOGI(TAG, "--- Mouse: drag ---");

    hid_mouse_move_abs(100, 800, 1920, 1080);
    vTaskDelay(pdMS_TO_TICKS(50));
    hid_mouse_down("left");

    for (int step = 1; step <= 8; step++) {
        uint32_t x = 100 + (400 * step) / 8;
        uint32_t y = 800 + (200 * step) / 8;
        hid_mouse_move_abs(x, y, 1920, 1080);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    hid_mouse_up("left");
    vTaskDelay(pdMS_TO_TICKS(200));
}

static void test_mouse_scroll(void)
{
    ESP_LOGI(TAG, "--- Mouse: scroll ---");

    for (int i = 0; i < 5; i++) {
        hid_mouse_scroll(0, -1);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    for (int i = 0; i < 5; i++) {
        hid_mouse_scroll(0, 1);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
}

#include "class/hid/hid_device.h"
#include "device/usbd.h"

void hid_run_self_test(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  HID Self-Test Starting");
    ESP_LOGI(TAG, "  Connect target PC via USB OTG port");
    ESP_LOGI(TAG, "========================================");

    /* Wait for USB enumeration */
    ESP_LOGI(TAG, "Waiting for USB mount...");
    int timeout = 150;
    while (timeout-- > 0) {
        if (tud_mounted()) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "USB mounted=%d", tud_mounted());

    if (!tud_mounted()) {
        ESP_LOGW(TAG, "USB not mounted after 15s — test skipped");
        return;
    }

    /* Extra settling time for Windows to finish HID enumeration */
    vTaskDelay(pdMS_TO_TICKS(2000));

    test_keyboard_chars();
    test_keyboard_special();
    test_mouse_move();
    test_mouse_buttons();
    test_mouse_drag();
    test_mouse_scroll();

    hid_release_all();

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  HID Self-Test Complete");
    ESP_LOGI(TAG, "========================================");
}
