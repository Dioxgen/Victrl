#include "task_log.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "esp_log.h"

static const char *TAG = "task_log";

/*
 * Durability vs latency.
 *
 * The log used to be unbuffered (_IONBF) with an explicit fflush+fsync on
 * every single fprintf — roughly a dozen SD writes plus two filesystem syncs
 * per step, all on the agent loop's critical path.
 *
 * Now the stream is buffered and only ffush()ed per step; the expensive fsync
 * (which forces the FAT and directory entry out to the card) runs every
 * TASK_LOG_SYNC_INTERVAL steps and at close. A power cut can therefore lose
 * the tail of the log, which is an acceptable trade for an unattended agent
 * whose full transcript also goes to the serial console.
 */
#define TASK_LOG_BUFFER_SIZE     4096
#define TASK_LOG_SYNC_INTERVAL   10

static char s_log_dir[64] = {0};
static char s_task_id[32] = {0};
static char s_log_path[256] = {0};
static FILE *s_log_file = NULL;
static char s_log_buf[TASK_LOG_BUFFER_SIZE];
static uint32_t s_steps_since_sync = 0;

esp_err_t task_log_init(const char *log_dir, const char *task_id)
{
    if (!log_dir || !task_id) return ESP_ERR_INVALID_ARG;

    strncpy(s_log_dir, log_dir, sizeof(s_log_dir) - 1);
    strncpy(s_task_id, task_id, sizeof(s_task_id) - 1);
    /* Create task folder */
    char task_dir[192];
    snprintf(task_dir, sizeof(task_dir), "%s/%s", log_dir, task_id);
    mkdir(task_dir, 0755);
    snprintf(s_log_path, sizeof(s_log_path), "%s/%s.log", task_dir, task_id);

    s_log_file = fopen(s_log_path, "a");
    if (!s_log_file) {
        ESP_LOGW(TAG, "Failed to create log file: %s", s_log_path);
        return ESP_FAIL;
    }
    /* Buffered writes; durability is handled by the periodic fsync below. */
    setvbuf(s_log_file, s_log_buf, _IOFBF, sizeof(s_log_buf));
    s_steps_since_sync = 0;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    fprintf(s_log_file, "=== Victrl Task Log ===\n");
    fprintf(s_log_file, "Task: %s\n", task_id);
    fprintf(s_log_file, "Started: %04d-%02d-%02d %02d:%02d:%02d\n",
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
            t->tm_hour, t->tm_min, t->tm_sec);
    fprintf(s_log_file, "========================\n\n");
    fflush(s_log_file);
    fsync(fileno(s_log_file));

    ESP_LOGI(TAG, "Task log opened: %s (fp=%p)", s_log_path, s_log_file);
    return ESP_OK;
}

esp_err_t task_log_write_step(uint32_t step, const char *observation,
                               const char *self_eval, const char *action_summary,
                               const char *reasoning, const char *plan_json,
                               const char *raw_response, bool used_full_prompt,
                               uint32_t request_bytes, uint32_t response_bytes,
                               uint32_t api_time_ms, uint32_t exec_time_ms)
{
    if (!s_log_file) return ESP_ERR_INVALID_STATE;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    fprintf(s_log_file, "--- Step %lu [%02d:%02d:%02d] ---\n",
            step, t->tm_hour, t->tm_min, t->tm_sec);
    fprintf(s_log_file, "Prompt: %s | Req: %luB | Resp: %luB | API: %lums | Exec: %lums\n",
            used_full_prompt ? "full" : "short",
            request_bytes, response_bytes, api_time_ms, exec_time_ms);
    fprintf(s_log_file, "Actions: %s\n",
            action_summary ? action_summary : "(none)");
    fprintf(s_log_file, "Observation: %s\n",
            observation ? observation : "(none)");
    fprintf(s_log_file, "Self-evaluation: %s\n",
            self_eval ? self_eval : "(none)");

    if (reasoning && reasoning[0]) {
        fprintf(s_log_file, "Reasoning: %s\n", reasoning);
    }
    if (plan_json && plan_json[0]) {
        fprintf(s_log_file, "Plan: %s\n", plan_json);
    }
    if (raw_response && raw_response[0]) {
        fprintf(s_log_file, "Response: %s\n", raw_response);
    }

    fprintf(s_log_file, "\n");
    fflush(s_log_file);

    /* fsync forces the FAT and directory entry to the card — far more
     * expensive than the data write itself, so only do it periodically. */
    if (++s_steps_since_sync >= TASK_LOG_SYNC_INTERVAL) {
        fsync(fileno(s_log_file));
        s_steps_since_sync = 0;
    }

    return ESP_OK;
}

esp_err_t task_log_write_note(const char *text)
{
    if (!s_log_file) return ESP_ERR_INVALID_STATE;
    /* fsync here, unlike per-step writes: this is the last thing written before
     * the file is closed, and losing it would lose the end state. */
    fprintf(s_log_file, "%s\n", text ? text : "");
    fflush(s_log_file);
    fsync(fileno(s_log_file));
    return ESP_OK;
}

esp_err_t task_log_write_timing(uint32_t step, const step_timing_t *t)
{
    if (!t) return ESP_ERR_INVALID_ARG;

    /* Console first, so a single log line is enough to see the breakdown
     * without pulling the SD card. */
    ESP_LOGI(TAG,
             "step %lu timing(ms): total=%lu capture=%lu prep=%lu "
             "(jpeg=%lu setup=%lu dec=%lu rs=%lu enc=%lu b64=%lu json=%lu) "
             "connect=%lu http=%lu parse=%lu sdlog=%lu exec=%lu sleep=%lu",
             step, t->total_ms, t->capture_ms, t->prep_ms,
             t->compress_ms, t->jpeg_setup_ms, t->jpeg_decode_ms,
             t->jpeg_resample_ms, t->jpeg_encode_ms,
             t->base64_ms, t->serialize_ms,
             t->connect_ms, t->http_ms, t->parse_ms,
             t->log_ms, t->exec_ms, t->sleep_ms);

    if (!s_log_file) return ESP_ERR_INVALID_STATE;

    fprintf(s_log_file,
            "Timing(ms): total=%lu capture=%lu prep=%lu "
            "(jpeg=%lu setup=%lu dec=%lu rs=%lu enc=%lu b64=%lu json=%lu) "
            "connect=%lu http=%lu parse=%lu sdlog=%lu exec=%lu sleep=%lu\n",
            t->total_ms, t->capture_ms, t->prep_ms,
            t->compress_ms, t->jpeg_setup_ms, t->jpeg_decode_ms,
            t->jpeg_resample_ms, t->jpeg_encode_ms,
            t->base64_ms, t->serialize_ms,
            t->connect_ms, t->http_ms, t->parse_ms,
            t->log_ms, t->exec_ms, t->sleep_ms);
    fflush(s_log_file);
    return ESP_OK;
}

esp_err_t task_log_save_frame(uint32_t step, const uint8_t *data, size_t len)
{
    if (!data || !len) return ESP_ERR_INVALID_ARG;

    char path[192];
    snprintf(path, sizeof(path), "%s/%s/%s_step%04lu.jpg",
             s_log_dir, s_task_id, s_task_id, step);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "Failed to create frame file: %s", path);
        return ESP_FAIL;
    }
    fwrite(data, 1, len, f);
    fclose(f);
    ESP_LOGI(TAG, "Frame saved: %s (%zu bytes)", path, len);
    return ESP_OK;
}

esp_err_t task_log_close(void)
{
    if (!s_log_file) return ESP_OK;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    fprintf(s_log_file, "========================\n");
    fprintf(s_log_file, "Ended: %04d-%02d-%02d %02d:%02d:%02d\n",
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
            t->tm_hour, t->tm_min, t->tm_sec);
    fflush(s_log_file);
    fsync(fileno(s_log_file));   /* final sync: nothing may be lost now */
    fclose(s_log_file);
    s_log_file = NULL;
    s_steps_since_sync = 0;

    ESP_LOGI(TAG, "Task log closed: %s", s_log_path);
    return ESP_OK;
}
