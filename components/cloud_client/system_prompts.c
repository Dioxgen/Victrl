#include "system_prompts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "sysprompts";

static char *s_prompt_full = NULL;
static char *s_prompt_short = NULL;
static size_t s_prompt_full_len = 0;
static size_t s_prompt_short_len = 0;

static char *load_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "Failed to open %s", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        ESP_LOGW(TAG, "File %s is empty", path);
        fclose(f);
        return NULL;
    }

    char *buf = (char *)malloc(size + 1);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate %ld bytes for %s", size, path);
        fclose(f);
        return NULL;
    }

    size_t bytes_read = fread(buf, 1, size, f);
    fclose(f);

    if (bytes_read != (size_t)size) {
        ESP_LOGE(TAG, "Failed to read %s (read %zu of %ld)", path, bytes_read, size);
        free(buf);
        return NULL;
    }

    buf[bytes_read] = '\0';
    *out_len = bytes_read;
    ESP_LOGI(TAG, "Loaded %s (%zu bytes)", path, *out_len);
    return buf;
}

esp_err_t system_prompts_init(const char *sysprompt_dir)
{
    char path[128];

    snprintf(path, sizeof(path), "%s/full.txt", sysprompt_dir);
    s_prompt_full = load_file(path, &s_prompt_full_len);
    if (!s_prompt_full) {
        ESP_LOGE(TAG, "Full system prompt is required but could not be loaded");
        return ESP_FAIL;
    }

    snprintf(path, sizeof(path), "%s/short.txt", sysprompt_dir);
    s_prompt_short = load_file(path, &s_prompt_short_len);
    if (!s_prompt_short) {
        ESP_LOGW(TAG, "Short system prompt not found, using full prompt as fallback");
        s_prompt_short = strdup(s_prompt_full);
        s_prompt_short_len = s_prompt_full_len;
    }

    return ESP_OK;
}

char *system_prompts_get_full(void)
{
    return s_prompt_full;
}

char *system_prompts_get_short(void)
{
    return s_prompt_short;
}

size_t system_prompts_get_full_len(void)
{
    return s_prompt_full_len;
}

size_t system_prompts_get_short_len(void)
{
    return s_prompt_short_len;
}
