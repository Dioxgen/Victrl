#include "display_task.h"

#include <stdio.h>
#include <string.h>
#include "NV3007_driver.h"
#include "agent_core.h"
#include "storage_manager.h"
#include "utf8_util.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display";

static TaskHandle_t s_task = NULL;

#define COLOR_BG      0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_GREEN   0x07E0
#define COLOR_YELLOW  0xFFE0
#define COLOR_RED     0xF800
#define COLOR_CYAN    0x07FF

/* ── Normalized layout (0.0 – 1.0, relative to LCD_W / LCD_H) ─────── */

#define NX(v)  ((uint16_t)((v) * LCD_W))
#define NY(v)  ((uint16_t)((v) * LCD_H))

/* Layout: row 1-2 are absolute (normalized), log rows cascade with 5px gaps */
#define ROW1_Y      0.02f   /* IP + Step + State */
#define ROW2_Y      0.18f   /* Task name */
/* Log rows: 12px font + 2px gap between lines */
#define LOG_ROW(n)  ((uint16_t)(NY(ROW2_Y) + 14 + (n) * 14))

#define IP_X        0.00f
#define IP_W        0.30f
#define STEP_X      0.32f
#define STATE_X     0.58f
#define TASK_X      0.00f

#define FONT_SM     12
#define FONT_MD     16

static uint16_t state_color(agent_state_t s)
{
    switch (s) {
    case AGENT_STATE_RUNNING:  return COLOR_GREEN;
    case AGENT_STATE_PAUSED:   return COLOR_YELLOW;
    case AGENT_STATE_EMERGENCY:return COLOR_RED;
    default: return COLOR_WHITE;
    }
}

static const char *state_str(agent_state_t s)
{
    switch (s) {
    case AGENT_STATE_RUNNING:   return "RUN";
    case AGENT_STATE_PAUSED:    return "PAUSE";
    case AGENT_STATE_STEP:      return "STEP";
    case AGENT_STATE_STOPPING:  return "STOP";
    case AGENT_STATE_EMERGENCY: return "EMERG";
    default: return "IDLE";
    }
}

static void display_task(void *pv)
{
    NV3007_Init();
    NV3007_FastFill(COLOR_BG);

    while (1) {
        agent_ctx_t *agent = agent_get_global();
        if (!agent) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }

        agent_state_t st = agent_get_state(agent);
        char ip[32] = "no network";
        wifi_manager_get_ip(ip, sizeof(ip));

        char line[72];
        NV3007_FastFill(COLOR_BG);

        /* ── Row 1: IP | Step N/M | STATE ── */
        snprintf(line, sizeof(line), "IP:%.15s", ip);
        NV3007_ShowString(NX(IP_X), NY(ROW1_Y), line, COLOR_WHITE, COLOR_BG, FONT_SM, 0);

        /* Step: show action count. If plan has milestones, show milestone progress */
        uint32_t step = agent->action_count;
        cJSON *plan = plan_mgr_get_current();
        int total_ms = 0, cur_ms = 0;
        if (plan) {
            cJSON *ms = cJSON_GetObjectItem(plan, "milestones");
            if (ms && cJSON_IsArray(ms)) total_ms = cJSON_GetArraySize(ms);
            cJSON *pu = cJSON_GetObjectItem(plan, "plan_update");
            if (pu) {
                cJSON *cm = cJSON_GetObjectItem(pu, "current_milestone");
                if (cm) cur_ms = cm->valueint;
            }
            cJSON_Delete(plan);
        }
        if (total_ms > 0) {
            snprintf(line, sizeof(line), "Step:%lu %d/%d", step, cur_ms, total_ms);
        } else {
            snprintf(line, sizeof(line), "Step:%lu", step);
        }
        NV3007_ShowString(NX(STEP_X), NY(ROW1_Y), line, COLOR_CYAN, COLOR_BG, FONT_SM, 0);

        snprintf(line, sizeof(line), "%s", state_str(st));
        NV3007_ShowString(NX(STATE_X), NY(ROW1_Y), line, state_color(st), COLOR_BG, FONT_SM, 0);

        /* ── Row 2: Task name ── */
        if (agent->task_goal[0]) {
            /* Only the first ~55 bytes fit on the panel, and the goal is
             * usually Chinese: cut on a character boundary rather than leaving
             * a stray byte at the end of the line. */
            char task[60];
            utf8_copy(task, 56, agent->task_goal);
            NV3007_ShowString(NX(TASK_X), NY(ROW2_Y), task, COLOR_WHITE, COLOR_BG, FONT_SM, 1);
        }

        /* ── Rows 3-6: Short-term memory log ── */
        char *history = stm_get_all_formatted();
        if (history) {
            char *lines[20];
            int n = 0;
            /* strtok_r: the HID task also tokenises strings, and strtok's
             * global cursor is shared between tasks. */
            char *saveptr = NULL;
            char *tok = strtok_r(history, "\n", &saveptr);
            while (tok && n < 20) {
                lines[n++] = tok;
                tok = strtok_r(NULL, "\n", &saveptr);
            }
            /* Show up to 5 most recent, 5px gap between lines */
            int start = n > 5 ? n - 5 : 0;
            for (int i = 0; i < 5 && (start + i) < n; i++) {
                char *l = lines[start + i];
                char sl[55];
                strncpy(sl, l, sizeof(sl) - 1);
                sl[sizeof(sl) - 1] = '\0';
                if (strlen(sl) > 50) sl[50] = '\0';
                NV3007_ShowString(NX(TASK_X), LOG_ROW(i), sl,
                                  COLOR_WHITE, COLOR_BG, FONT_SM, 0);
            }
            free(history);
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
    }
}

esp_err_t display_task_init(void)
{
    BaseType_t ret = xTaskCreate(display_task, "lcd_refresh",
                                  3072, NULL, 2, &s_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create display task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Display task started");
    return ESP_OK;
}

void display_task_notify(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}
