#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "agent_core.h"
#include "action_executor.h"
#include "cloud_client.h"
#include "button_driver.h"
#include "display_task.h"
#include "hid_device.h"
#include "hid_test.h"
#include "mouse_test.h"
#include "storage_manager.h"
#include "web_server.h"
#include "system_prompts.h"
#include "usb/uvc_host.h"
#include "uvc_capture_card_driver.h"
#include "wifi_manager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdmmc_driver.h"
#include <time.h>

static const char *TAG = "main";

/* MS2109 capture card config */
#define MS2109_VID  0x534D
#define MS2109_PID  0x2109

static const uvc_host_stream_config_t s_uvc_cfg = {
    .usb = {
        .vid              = MS2109_VID,
        .pid              = MS2109_PID,
        .dev_addr         = UVC_HOST_ANY_DEV_ADDR,
        .uvc_stream_index = 0,
    },
    .vs_format = {
        .h_res  = 1920,
        .v_res  = 1080,
        .fps    = 30,
        .format = UVC_VS_FORMAT_MJPEG,
    },
    .advanced = {
        .number_of_frame_buffers = 6,
        .number_of_urbs          = 4,
        .urb_size                = 0,
        .frame_size              = 1024 * 1024,
        .frame_heap_caps         = MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA,
    },
};

void app_main(void)
{
    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, "  Victrl Standard v1.0 — ESP32-P4");
    ESP_LOGI(TAG, "==========================================");

    /* 1. Init NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    /* 2. Init SD card first (needed before config load) */
    ESP_LOGI(TAG, "Initializing SD card...");
    sdmmc_driver_config_t sd_cfg;
    sdmmc_driver_get_default_config(&sd_cfg);
    sdmmc_card_t *sd_card = NULL;
    esp_err_t err = sdmmc_driver_init(&sd_cfg, &sd_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD card init failed: %d", err);
        return;
    }
    ESP_LOGI(TAG, "SD card initialized");

    /* 3. Load config from SD card.
     *
     * Static, not stack: app_main must also absorb the UVC, cloud-client and
     * session setup call chains, and this is ~1KB of the frame. */
    static runtime_config_t cfg;
    err = config_load("/sdcard/config.json", &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Config load failed, using defaults");
    }

    /* 4. Init UVC capture with config resolution (SD already mounted, auto-skipped) */
    uvc_host_stream_config_t uvc_cfg = s_uvc_cfg;
    uvc_cfg.vs_format.h_res = cfg.capture_width;
    uvc_cfg.vs_format.v_res = cfg.capture_height;
    ESP_LOGI(TAG, "Initializing UVC capture (MS2109 %dx%d)...",
             (int)uvc_cfg.vs_format.h_res, (int)uvc_cfg.vs_format.v_res);
    err = uvc_capture_init(&uvc_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UVC capture init failed: %d — MS2109 missing?", err);
        return;
    }

    /* 5. Capture test: wait 10s, grab one frame to SD card root */
    /*ESP_LOGI(TAG, "Waiting 10s for MS2109 signal lock...");
    vTaskDelay(pdMS_TO_TICKS(10000));
    uint8_t *test_frame = NULL;
    size_t test_len = 0;
    err = uvc_capture_one_frame(&test_frame, &test_len, 5000);
    if (err == ESP_OK && test_frame && test_len > 0) {
        ESP_LOGI(TAG, "Test frame captured: %zu bytes", test_len);
        uvc_capture_save_to_sd("test_capture.jpg", test_frame, test_len);
        free(test_frame);
    } else {
        ESP_LOGW(TAG, "Test frame capture failed: %d", err);
    }*/

    /* 4. Init WiFi (no-op on P4 until C6 integration) */
    ESP_LOGI(TAG, "Initializing WiFi...");
    err = wifi_manager_init(cfg.wifi_ssid, cfg.wifi_password);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi init returned %d — continuing without network", err);
    }

    /* 4b. Start time sync in the background.
     *
     * This used to block app_main() for up to 30s polling sntp_get_sync_status(),
     * which meant a single slow or unreachable NTP host stalled the entire boot
     * — and with CONFIG_LWIP_SNTP_MAX_SERVERS=1 the fallback server it set was
     * silently discarded, so pool.ntp.org was the only host ever tried. Now the
     * sync runs asynchronously and the WebUI reports its status. */
    if (wifi_manager_is_connected()) {
        static const char *const ntp_servers[] = {
            "ntp.aliyun.com",     /* reachable from CN networks */
            "cn.pool.ntp.org",
            "pool.ntp.org",
            NULL
        };
        ESP_LOGI(TAG, "Starting SNTP in background...");
        esp_err_t sntp_err = wifi_manager_start_sntp(ntp_servers);
        if (sntp_err != ESP_OK) {
            ESP_LOGW(TAG, "SNTP start returned %d", sntp_err);
        }
    }

    /* 5. Load system prompts from SD card */
    ESP_LOGI(TAG, "Loading system prompts...");
    err = system_prompts_init(cfg.sysprompt_dir);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "System prompts init failed: %d", err);
        return;
    }

    /* 6. Init storage managers.
     *
     * Sessions are the conversation windows: each one owns a task goal, a plan
     * and a trajectory on the SD card. At boot we resume the most recent one so
     * the WebUI has something to show before any task is started. */
    profile_mgr_init(cfg.profile_dir, cfg.default_profile);
    err = session_mgr_init(cfg.session_dir);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Session manager init failed: %d", err);
        return;
    }

    session_info_t recent[1];
    if (session_mgr_list(recent, 1) > 0) {
        ESP_LOGI(TAG, "Resuming most recent session: %s", recent[0].id);
        session_mgr_load(recent[0].id);
    } else {
        ESP_LOGI(TAG, "No sessions yet — creating a placeholder window");
        session_mgr_create("Waiting for task assignment...", NULL);
    }

    stm_init(cfg.history_max_len);

    /* 7. Init cloud client */
    cloud_client_config_t cc_cfg = {
        .timeout_sec = cfg.timeout,
        .max_retries = cfg.max_retries,
        .max_output_tokens = cfg.max_output_tokens,
        .enable_thinking = cfg.enable_thinking,
        .upload_max_dim = cfg.upload_max_dim,
        .save_frames = cfg.save_frames,
    };
    strncpy(cc_cfg.api_endpoint, cfg.api_endpoint, sizeof(cc_cfg.api_endpoint) - 1);
    strncpy(cc_cfg.api_key, cfg.api_key, sizeof(cc_cfg.api_key) - 1);
    strncpy(cc_cfg.model_name, cfg.model_name, sizeof(cc_cfg.model_name) - 1);
    strncpy(cc_cfg.reasoning_effort, cfg.reasoning_effort,
            sizeof(cc_cfg.reasoning_effort) - 1);
    strncpy(cc_cfg.reasoning_effort_escalated, cfg.reasoning_effort_escalated,
            sizeof(cc_cfg.reasoning_effort_escalated) - 1);

    err = cloud_client_init(&cc_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cloud client init failed: %d", err);
        return;
    }

    /* 8. Init HID device (keyboard + absolute mouse) */
    ESP_LOGI(TAG, "Initializing HID device...");
    err = hid_device_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HID device init failed: %d — continuing without HID", err);
    }
    hid_device_set_screen_size(cfg.output_width, cfg.output_height);

    /* Typing rhythm and Run-dialog timing. These are accuracy parameters, not
     * speed knobs — a dropped character costs a whole model round trip. */
    hid_type_profile_t type_profile = {
        .hold_ms       = cfg.type_hold_ms,
        .gap_ms        = cfg.type_gap_ms,
        .jitter_ms     = cfg.type_jitter_ms,
        .word_pause_ms = cfg.type_word_pause_ms,
    };
    hid_device_set_type_profile(&type_profile);
    action_executor_set_run_timing(cfg.run_dialog_ms, cfg.run_settle_ms);

    /* 8b. Run HID self-test (keyboard + mouse) */
    //hid_run_self_test();

    /* 8d. Run mouse test */
    //vTaskDelay(pdMS_TO_TICKS(5000)); // Wait a bit before starting test
    //mouse_run_test();

    /* 9. Init agent. Static because the main loop task holds a pointer to it
     * for the lifetime of the firmware, and it is ~600 bytes of frame. */
    static agent_ctx_t agent;
    err = agent_init(&agent,
                     cfg.max_actions,
                     cfg.capture_width,
                     cfg.capture_height,
                     cfg.output_width,
                     cfg.output_height,
                     cfg.system_prompt_interval,
                     false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Agent init failed: %d", err);
        return;
    }

    /* need_screen v2: allow the loop to stop polling the API while the model is
     * only waiting and the screen is provably unchanged. Disabled by default —
     * enable via config once the frame fingerprint has been validated. */
    agent_set_wait_skip_policy(&agent, cfg.allow_wait_skip,
                               cfg.max_blind_rounds, cfg.wait_settle_ms);

    /* Adaptive reasoning effort: cheap for routine steps, expensive while stuck. */
    agent_set_effort_policy(&agent, cfg.reasoning_effort,
                            cfg.reasoning_effort_escalated,
                            cfg.effort_escalate_after);

    /* Required: without it the active profile stays empty and profile_updates
     * land in a file named ".md". */
    agent_set_active_profile(&agent, cfg.default_profile);

    /* 10. Init button driver (GPIO0=start, GPIO1=pause/stop) */
    ESP_LOGI(TAG, "Initializing button driver...");
    button_driver_init();

    /* 11. Init LCD display task */
    ESP_LOGI(TAG, "Initializing LCD display...");
    display_task_init();

    /* 12. Start web server */
    ESP_LOGI(TAG, "Starting web server on port %u...", cfg.http_port);
    err = web_server_start(cfg.http_port, cfg.web_dir);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Web server start failed: %d", err);
    }

    /* 13. Create main loop task */
    BaseType_t task_ret = xTaskCreate(
        agent_main_loop_task,
        "agent_loop",
        16384,
        &agent,
        5,
        &agent.main_loop_task);

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create agent loop task");
        return;
    }

    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, "  Victrl initialized successfully");
    ESP_LOGI(TAG, "  Waiting for task via WebUI or button...");
    ESP_LOGI(TAG, "==========================================");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
