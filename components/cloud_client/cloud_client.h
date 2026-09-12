#pragma once

#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"
#include "jpeg_utils.h"   /* view_spec_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char api_endpoint[128];
    char api_key[128];
    char model_name[64];
    uint32_t timeout_sec;
    uint32_t max_retries;
    uint32_t max_output_tokens;
    bool enable_thinking;
    /* Reasoning budget passed as `reasoning.effort` on the Responses API:
     * "low" | "high" | "max".
     *
     * `reasoning_effort` is the ROUTINE budget; the agent escalates to
     * `reasoning_effort_escalated` while it is stuck. Reasoning tokens are
     * 30-50% of output and the API call is 60-85% of a step's wall clock, so
     * paying for deep thinking on every routine step is the single largest
     * avoidable cost — while a stuck agent genuinely benefits from it.
     * `enable_thinking = false` forces "low" for both. */
    char reasoning_effort[8];
    char reasoning_effort_escalated[8];
    /* Longest edge the uploaded frame is downscaled to before base64.
     *
     *   0    -> pass the MS2109 MJPEG frame through untouched. No decode, no
     *           re-encode, no multi-megabyte PSRAM churn: the cheapest path,
     *           and the model resizes the image server-side regardless.
     *   >0   -> decode, box-downscale to this longest edge, re-encode at
     *           UPLOAD_JPEG_QUALITY. Cuts upload bytes (~150KB -> ~60KB) at the
     *           cost of a full-resolution decode plus a software scale.
     *
     * Note the device's hardware decoder cannot scale, so any downscaling is a
     * software pass over the decoded pixels. */
    uint32_t upload_max_dim;
    /* Write each compressed frame to the SD card. Costs a ~150-250KB blocking
     * SD write on the agent loop's critical path, so off by default. */
    bool save_frames;
} cloud_client_config_t;

/*
 * Latency breakdown of one cloud_client_query() call, in milliseconds.
 * Fields are 0 when the phase did not run.
 */
typedef struct {
    uint32_t prep_ms;       /* format prompt + JPEG + base64 + JSON build */
    uint32_t compress_ms;   /*   subset: jpeg_compress()                  */
    uint32_t jpeg_setup_ms; /*     subset of the two below: codec setup   */
    uint32_t jpeg_decode_ms;/*     subset: hardware decode                */
    uint32_t jpeg_resample_ms; /*  subset: software crop + downscale      */
    uint32_t jpeg_encode_ms;/*     subset: hardware re-encode             */
    uint32_t base64_ms;     /*   subset: base64_encode()                  */
    uint32_t serialize_ms;  /*   subset: cJSON_PrintUnformatted()         */
    uint32_t connect_ms;    /*   TCP + TLS handshake inside perform()     */
    uint32_t http_ms;       /*   whole esp_http_client_perform()          */
    uint32_t parse_ms;      /*   response JSON parse + action validation  */
} cloud_client_timing_t;

esp_err_t cloud_client_init(const cloud_client_config_t *cfg);

/*
 * True when the prepared image payload can be reused FOR THIS VIEW.
 *
 * It is only valid for the same frame AND the same view: a crop of the previous
 * frame is not a crop of this one. Keeping the check inside cloud_client stops
 * the caller from skipping the capture and then ending up with no image at all.
 */
bool cloud_client_can_reuse_image(const view_spec_t *view);

/*
 * The exact image bytes last sent to the model (post crop/scale), so the WebUI
 * can show what the model actually saw. NULL until the first successful send.
 * Valid until the next query.
 */
const uint8_t *cloud_client_get_last_jpeg(size_t *out_len);

esp_err_t cloud_client_query(
    const uint8_t *jpeg_data,
    size_t jpeg_len,
    const char *system_prompt,
    const char *user_text,
    const char *plan_json,
    const char *history_text,
    const char *profile_text,
    const char *last_summary,
    const char *status_text,      /* code-maintained status bar; may be NULL */
    const char *effort,           /* reasoning effort override; NULL = config */
    const view_spec_t *view,      /* how the frame is shown; NULL = full frame */
    bool reuse_image,             /* reuse the previously prepared frame      */
    cJSON **out_response,
    uint32_t step_number,
    cloud_client_timing_t *out_timing);   /* may be NULL */

#ifdef __cplusplus
}
#endif
