#include "storage_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "stm";

static char **s_entries = NULL;
static size_t s_count = 0;
static size_t s_max_len = 10;
static SemaphoreHandle_t s_mutex = NULL;

void stm_init(size_t max_len)
{
    s_max_len = max_len > 0 ? max_len : 10;
    s_entries = (char **)calloc(s_max_len, sizeof(char *));
    s_count = 0;
    s_mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "Init: max_len=%zu", s_max_len);
}

void stm_add(const char *summary)
{
    if (!summary || !s_mutex) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_count >= s_max_len) {
        /* Compress: merge oldest two entries */
        size_t merged_len = strlen(s_entries[0]) + strlen(s_entries[1]) + 16;
        char *merged = (char *)malloc(merged_len);
        if (merged) {
            snprintf(merged, merged_len, "%s; %s", s_entries[0], s_entries[1]);
            free(s_entries[0]);
            free(s_entries[1]);
            s_entries[0] = merged;
            for (size_t i = 1; i < s_count - 1; i++) {
                s_entries[i] = s_entries[i + 1];
            }
            s_entries[s_count - 1] = NULL;
            s_count--;
        }
    }

    s_entries[s_count] = strdup(summary);
    s_count++;

    xSemaphoreGive(s_mutex);
}

char *stm_get_all_formatted(void)
{
    if (!s_mutex) return strdup("No history yet.");

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_count == 0) {
        xSemaphoreGive(s_mutex);
        return strdup("No history yet.");
    }

    size_t total = 0;
    for (size_t i = 0; i < s_count; i++) {
        total += strlen(s_entries[i]) + 2;
    }

    char *result = (char *)malloc(total + 1);
    if (!result) {
        xSemaphoreGive(s_mutex);
        return strdup("(memory error)");
    }

    result[0] = '\0';
    for (size_t i = 0; i < s_count; i++) {
        strcat(result, s_entries[i]);
        if (i < s_count - 1) strcat(result, "\n");
    }

    xSemaphoreGive(s_mutex);
    return result;
}

void stm_clear(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_count; i++) {
        free(s_entries[i]);
        s_entries[i] = NULL;
    }
    s_count = 0;
    xSemaphoreGive(s_mutex);
}
