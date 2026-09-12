#include "button_driver.h"

#include "agent_core.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "btn";

#define BTN_START   GPIO_NUM_0
#define BTN_PAUSE   GPIO_NUM_1
#define DEBOUNCE_MS 50
#define LONG_PRESS_MS 3000

static TaskHandle_t s_btn_task = NULL;

static void IRAM_ATTR btn_isr(void *arg)
{
    uint32_t gpio = (uint32_t)arg;
    BaseType_t wake = pdFALSE;
    xTaskNotifyFromISR(s_btn_task, gpio, eSetValueWithOverwrite, &wake);
    portYIELD_FROM_ISR(wake);
}

static void btn_task(void *pv)
{
    uint32_t gpio;
    TickType_t press_time[2] = {0};
    bool pressed[2] = {false};

    while (1) {
        if (xTaskNotifyWait(0, 0, &gpio, pdMS_TO_TICKS(100)) == pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));

            int level = gpio_get_level(gpio);
            int idx = (gpio == BTN_START) ? 0 : 1;

            if (level == 0 && !pressed[idx]) {
                /* Pressed */
                pressed[idx] = true;
                press_time[idx] = xTaskGetTickCount();
                ESP_LOGI(TAG, "GPIO%lu pressed", gpio);

            } else if (level == 1 && pressed[idx]) {
                /* Released */
                pressed[idx] = false;
                TickType_t elapsed = xTaskGetTickCount() - press_time[idx];
                uint32_t ms = elapsed * portTICK_PERIOD_MS;

                agent_ctx_t *agent = agent_get_global();
                if (!agent) continue;

                if (gpio == BTN_START) {
                    /* GPIO0: short press = start task */
                    if (ms < LONG_PRESS_MS) {
                        ESP_LOGI(TAG, "START button (short press %lums)", ms);
                        agent_start(agent, "Execute the default task");
                    }
                } else if (gpio == BTN_PAUSE) {
                    if (ms >= LONG_PRESS_MS) {
                        /* GPIO1: long press = stop/emergency */
                        ESP_LOGI(TAG, "STOP button (long press %lums)", ms);
                        agent_state_t st = agent_get_state(agent);
                        if (st == AGENT_STATE_RUNNING || st == AGENT_STATE_PAUSED) {
                            agent_stop(agent);
                        }
                    } else {
                        /* GPIO1: short press = pause/resume toggle */
                        ESP_LOGI(TAG, "PAUSE toggle (short press %lums)", ms);
                        agent_state_t st = agent_get_state(agent);
                        if (st == AGENT_STATE_RUNNING) {
                            agent_pause(agent);
                        } else if (st == AGENT_STATE_PAUSED) {
                            agent_resume(agent);
                        }
                    }
                }
            }
        }
    }
}

esp_err_t button_driver_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(BTN_START) | BIT64(BTN_PAUSE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    /* 6KB, not the original 2KB: this task calls agent_start() from a button
     * press, which now creates a session (cJSON, file writes, logging) — a
     * much deeper call chain than the old plan_mgr_new_task() it used to hit. */
    BaseType_t ret = xTaskCreate(btn_task, "btn_monitor",
                                  6144, NULL, 8, &s_btn_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create button task");
        return ESP_ERR_NO_MEM;
    }

    gpio_install_isr_service(0);
    gpio_isr_handler_add(BTN_START, btn_isr, (void *)BTN_START);
    gpio_isr_handler_add(BTN_PAUSE, btn_isr, (void *)BTN_PAUSE);

    ESP_LOGI(TAG, "Button driver initialized (GPIO0=start, GPIO1=pause/stop)");
    return ESP_OK;
}
