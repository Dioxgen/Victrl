#include "jpeg_utils.h"

#include <string.h>
#include "driver/jpeg_decode.h"
#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "jpeg_utils";

/* Smallest ROI we will honour — see VIEW_MIN_EXTENT. */
#define VIEW_MIN_FRACTION VIEW_MIN_EXTENT

/* Round up to the 16-byte alignment the hardware codec applies. */
static inline uint32_t align16(uint32_t v)
{
    return (v + 15u) & ~15u;
}

/* ── cached codec resources ─────────────────────────────────────────── */

/*
 * The capture card delivers one fixed geometry for the whole run, so the codec
 * engines and the buffers they need are invariant — yet the first version of
 * this file created and destroyed both engines and allocated a fresh 6.2MB
 * PSRAM decode buffer on EVERY step:
 *
 *     jpeg_new_decoder_engine()            per step
 *     jpeg_alloc_decoder_mem(1920*1080*3)  per step, 6.2MB
 *     jpeg_decoder_process()               per step
 *     jpeg_del_decoder_engine()            per step
 *     free(6.2MB)                          per step
 *     ... then the same again for the encoder
 *
 * All of that was hidden inside the single `jpeg=180ms` timing field. Held
 * here instead, and rebuilt only when the frame geometry actually changes.
 *
 * These functions are NOT reentrant while the cache is in use, hence the lock.
 * Today the only caller is the agent task; the lock is here so that adding a
 * second caller (a preview path, say) cannot silently corrupt the codec state.
 */
static jpeg_decoder_handle_t s_dec = NULL;
static uint8_t  *s_dec_buf = NULL;
static size_t    s_dec_buf_size = 0;
static uint32_t  s_dec_w = 0, s_dec_h = 0;   /* padded geometry, also the stride */

static jpeg_encoder_handle_t s_enc = NULL;

/* Staging buffer for the encoder input when crop/scale has to repack pixels. */
static uint8_t *s_scratch = NULL;
static size_t   s_scratch_size = 0;

static SemaphoreHandle_t s_codec_lock = NULL;
static jpeg_stage_timing_t s_stages;

static bool codec_lock(void)
{
    if (!s_codec_lock) s_codec_lock = xSemaphoreCreateMutex();
    if (!s_codec_lock) return false;
    return xSemaphoreTake(s_codec_lock, portMAX_DELAY) == pdTRUE;
}

static void codec_unlock(void)
{
    if (s_codec_lock) xSemaphoreGive(s_codec_lock);
}

void jpeg_utils_release_cache(void)
{
    if (!codec_lock()) return;

    if (s_dec) { jpeg_del_decoder_engine(s_dec); s_dec = NULL; }
    free(s_dec_buf);      s_dec_buf = NULL;  s_dec_buf_size = 0;
    s_dec_w = s_dec_h = 0;

    if (s_enc) { jpeg_del_encoder_engine(s_enc); s_enc = NULL; }

    free(s_scratch);      s_scratch = NULL;  s_scratch_size = 0;

    codec_unlock();
}

const jpeg_stage_timing_t *jpeg_utils_last_stages(void)
{
    return &s_stages;
}

/* Decoder engine + full-resolution output buffer, valid for (pad_w, pad_h). */
static esp_err_t ensure_decoder(uint32_t pad_w, uint32_t pad_h)
{
    if (s_dec && s_dec_buf && s_dec_w == pad_w && s_dec_h == pad_h) {
        return ESP_OK;   /* the steady-state path */
    }

    /* Geometry changed (or first use): rebuild from scratch. */
    if (s_dec) { jpeg_del_decoder_engine(s_dec); s_dec = NULL; }
    free(s_dec_buf); s_dec_buf = NULL; s_dec_buf_size = 0;
    s_dec_w = s_dec_h = 0;

    jpeg_decode_memory_alloc_cfg_t dec_mem = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t got = 0;
    uint8_t *buf = (uint8_t *)jpeg_alloc_decoder_mem((size_t)pad_w * pad_h * 3u,
                                                     &dec_mem, &got);
    if (!buf) {
        ESP_LOGE(TAG, "Decoder buffer alloc failed (%u bytes)",
                 (unsigned)(pad_w * pad_h * 3u));
        return ESP_ERR_NO_MEM;
    }

    jpeg_decode_engine_cfg_t dec_eng = { .timeout_ms = -1 };
    jpeg_decoder_handle_t dec = NULL;
    esp_err_t ret = jpeg_new_decoder_engine(&dec_eng, &dec);
    if (ret != ESP_OK) { free(buf); return ret; }

    s_dec = dec;
    s_dec_buf = buf;
    s_dec_buf_size = got;
    s_dec_w = pad_w;
    s_dec_h = pad_h;
    ESP_LOGI(TAG, "Decoder cached for %lux%lu (%u bytes)",
             pad_w, pad_h, (unsigned)got);
    return ESP_OK;
}

static esp_err_t ensure_encoder(void)
{
    if (s_enc) return ESP_OK;

    jpeg_encode_engine_cfg_t enc_eng = { .timeout_ms = -1 };
    jpeg_encoder_handle_t enc = NULL;
    esp_err_t ret = jpeg_new_encoder_engine(&enc_eng, &enc);
    if (ret != ESP_OK) return ret;

    s_enc = enc;
    ESP_LOGI(TAG, "Encoder cached");
    return ESP_OK;
}

/* Grow-only scratch buffer, so the steady state allocates nothing. */
static uint8_t *scratch_get(size_t need)
{
    if (s_scratch && s_scratch_size >= need) return s_scratch;

    uint8_t *p = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    if (!p) return NULL;

    free(s_scratch);
    s_scratch = p;
    s_scratch_size = need;
    return s_scratch;
}

/* ── view geometry ──────────────────────────────────────────────────── */

bool view_is_full(const view_spec_t *v)
{
    if (!v) return true;
    return (v->y0 <= 0.0f && v->x0 <= 0.0f && v->y1 >= 1.0f && v->x1 >= 1.0f &&
            v->scale <= 1);
}

bool view_equal(const view_spec_t *a, const view_spec_t *b)
{
    if (!a || !b) return a == b;
    const float eps = 0.0005f;
    float d = a->y0 - b->y0; if (d < 0) d = -d; if (d > eps) return false;
    d = a->x0 - b->x0;       if (d < 0) d = -d; if (d > eps) return false;
    d = a->y1 - b->y1;       if (d < 0) d = -d; if (d > eps) return false;
    d = a->x1 - b->x1;       if (d < 0) d = -d; if (d > eps) return false;
    return a->scale == b->scale;
}

bool view_normalize(view_spec_t *v)
{
    if (!v) return false;

    /* Order the bounds: the model may send them either way round. */
    if (v->y0 > v->y1) { float t = v->y0; v->y0 = v->y1; v->y1 = t; }
    if (v->x0 > v->x1) { float t = v->x0; v->x0 = v->x1; v->x1 = t; }

    /* Clamp into the frame. */
    if (v->y0 < 0.0f) v->y0 = 0.0f;
    if (v->x0 < 0.0f) v->x0 = 0.0f;
    if (v->y1 > 1.0f) v->y1 = 1.0f;
    if (v->x1 > 1.0f) v->x1 = 1.0f;

    /* Minimum extent, growing symmetrically about the centre. */
    if (v->y1 - v->y0 < VIEW_MIN_FRACTION) {
        float c = (v->y0 + v->y1) * 0.5f;
        v->y0 = c - VIEW_MIN_FRACTION * 0.5f;
        v->y1 = c + VIEW_MIN_FRACTION * 0.5f;
        if (v->y0 < 0.0f) { v->y1 -= v->y0; v->y0 = 0.0f; }
        if (v->y1 > 1.0f) { v->y0 -= (v->y1 - 1.0f); v->y1 = 1.0f; }
    }
    if (v->x1 - v->x0 < VIEW_MIN_FRACTION) {
        float c = (v->x0 + v->x1) * 0.5f;
        v->x0 = c - VIEW_MIN_FRACTION * 0.5f;
        v->x1 = c + VIEW_MIN_FRACTION * 0.5f;
        if (v->x0 < 0.0f) { v->x1 -= v->x0; v->x0 = 0.0f; }
        if (v->x1 > 1.0f) { v->x0 -= (v->x1 - 1.0f); v->x1 = 1.0f; }
    }

    if (v->scale != 1 && v->scale != 2 && v->scale != 4) v->scale = 1;

    /* Reject a view that still has no area to show. */
    return (v->y1 > v->y0) && (v->x1 > v->x0);
}

void view_map_to_full(const view_spec_t *v, float x, float y, float *fx, float *fy)
{
    if (!v || view_is_full(v)) {
        if (fx) *fx = x;
        if (fy) *fy = y;
        return;
    }
    if (fx) *fx = v->x0 + x * (v->x1 - v->x0);
    if (fy) *fy = v->y0 + y * (v->y1 - v->y0);
}

/* ── codec plumbing ─────────────────────────────────────────────────── */

/*
 * Decode a JPEG into the CACHED RGB888 buffer.
 *
 * The hardware decoder always emits the JPEG's native resolution: it cannot
 * scale, and the buffer must be sized for the SOURCE padded to the driver's
 * 16-byte alignment (sizing it for a downscaled target made every decode fail
 * with JPEG_ERR_NO_MEM while the caller quietly fell back to the full frame).
 *
 * `stride_out` is the row stride in PIXELS, which may exceed the image width.
 *
 * The returned pointer is BORROWED from the codec cache: it stays valid only
 * until the next jpeg_utils call, and the caller must NOT free it. That is the
 * price of not allocating 6.2MB per step.
 */
static esp_err_t decode_rgb(const uint8_t *src, size_t src_len,
                            uint8_t **rgb_out, size_t *alloc_out,
                            uint32_t *w_out, uint32_t *h_out, uint32_t *stride_out)
{
    jpeg_decode_picture_info_t pic_info;
    esp_err_t ret = jpeg_decoder_get_info(src, src_len, &pic_info);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get picture info: %d", ret);
        return ret;
    }

    const uint32_t src_w = pic_info.width;
    const uint32_t src_h = pic_info.height;
    const uint32_t dec_w = align16(src_w);
    const uint32_t dec_h = align16(src_h);

    int64_t t_setup = esp_timer_get_time();
    ret = ensure_decoder(dec_w, dec_h);
    s_stages.setup_ms += (uint32_t)((esp_timer_get_time() - t_setup) / 1000);
    if (ret != ESP_OK) return ret;

    jpeg_decode_cfg_t dec_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };

    uint32_t actual_out = (uint32_t)s_dec_buf_size;
    ret = jpeg_decoder_process(s_dec, &dec_cfg, src, src_len,
                               s_dec_buf, s_dec_buf_size, &actual_out);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Decode failed: %d (src %lux%lu)", ret, src_w, src_h);
        return ret;
    }

    *rgb_out = s_dec_buf;
    if (alloc_out) *alloc_out = s_dec_buf_size;
    *w_out = src_w;
    *h_out = src_h;
    *stride_out = dec_w;
    return ESP_OK;
}

/*
 * Re-encode `rgb` as JPEG.
 *
 * The engine is cached, but the output buffer is still allocated per call and
 * handed to the caller to free: cloud_client base64-encodes it and frees it, so
 * keeping the existing ownership contract avoids a lifetime rule that would be
 * easy to get wrong later. The allocation is small next to the decode buffer
 * that used to be re-created every step.
 */
static esp_err_t encode_rgb(const uint8_t *rgb, uint32_t w, uint32_t h,
                            int quality, uint8_t **dst, size_t *dst_len)
{
    jpeg_encode_cfg_t enc_cfg = {
        .width         = w,
        .height        = h,
        .src_type      = JPEG_ENCODE_IN_FORMAT_RGB888,
        .sub_sample    = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = quality > 0 ? quality : 60,
    };

    int64_t t_setup = esp_timer_get_time();
    esp_err_t ret = ensure_encoder();
    s_stages.setup_ms += (uint32_t)((esp_timer_get_time() - t_setup) / 1000);
    if (ret != ESP_OK) return ret;

    jpeg_encode_memory_alloc_cfg_t enc_mem = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    size_t buf_size = 0;
    uint8_t *out = (uint8_t *)jpeg_alloc_encoder_mem(w * h / 2 + 8192, &enc_mem,
                                                     &buf_size);
    if (!out) {
        ESP_LOGE(TAG, "Encoder output alloc failed");
        return ESP_ERR_NO_MEM;
    }

    uint32_t actual = 0;
    ret = jpeg_encoder_process(s_enc, &enc_cfg, rgb, w * h * 3u,
                               out, buf_size, &actual);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Encode failed: %d", ret);
        free(out);
        return ret;
    }

    *dst = out;
    *dst_len = actual;
    return ESP_OK;
}

/*
 * Resample the source region (sx0,sy0,cw,ch) into a tightly packed dw x dh RGB
 * buffer.
 *
 * When dw == cw the scale is 1:1 horizontally, so rows are copied verbatim —
 * the case that matters most because it covers "crop without downscaling".
 * Otherwise each destination pixel is the box average of the source block that
 * maps onto it, which reads better to a vision model than nearest-neighbour.
 */
static void resample_region(const uint8_t *rgb, uint32_t stride,
                            uint32_t sx0, uint32_t sy0, uint32_t cw, uint32_t ch,
                            uint8_t *dst, uint32_t dw, uint32_t dh)
{
    if (dw == cw) {
        for (uint32_t y = 0; y < dh; y++) {
            uint32_t sy = sy0 + (uint32_t)((uint64_t)y * ch / dh);
            memcpy(dst + (size_t)y * dw * 3u,
                   rgb + ((size_t)sy * stride + sx0) * 3u,
                   (size_t)dw * 3u);
        }
        return;
    }

    for (uint32_t y = 0; y < dh; y++) {
        uint32_t ry0 = (uint32_t)((uint64_t)y * ch / dh);
        uint32_t ry1 = (uint32_t)((uint64_t)(y + 1) * ch / dh);
        if (ry1 <= ry0) ry1 = ry0 + 1;
        if (ry1 > ch) ry1 = ch;

        uint8_t *drow = dst + (size_t)y * dw * 3u;
        for (uint32_t x = 0; x < dw; x++) {
            uint32_t rx0 = (uint32_t)((uint64_t)x * cw / dw);
            uint32_t rx1 = (uint32_t)((uint64_t)(x + 1) * cw / dw);
            if (rx1 <= rx0) rx1 = rx0 + 1;
            if (rx1 > cw) rx1 = cw;

            uint32_t s0 = 0, s1 = 0, s2 = 0, n = 0;
            for (uint32_t ry = ry0; ry < ry1; ry++) {
                const uint8_t *p = rgb + ((size_t)(sy0 + ry) * stride + sx0 + rx0) * 3u;
                for (uint32_t rx = rx0; rx < rx1; rx++) {
                    s0 += p[0]; s1 += p[1]; s2 += p[2];
                    p += 3;
                    n++;
                }
            }
            if (n == 0) n = 1;
            drow[x * 3 + 0] = (uint8_t)(s0 / n);
            drow[x * 3 + 1] = (uint8_t)(s1 / n);
            drow[x * 3 + 2] = (uint8_t)(s2 / n);
        }
    }
}

/* ── public transforms ──────────────────────────────────────────────── */

esp_err_t jpeg_compress(const uint8_t *src, size_t src_len,
                         uint8_t **dst, size_t *dst_len,
                         int quality, int max_dim)
{
    if (!src || !src_len || !dst || !dst_len) return ESP_ERR_INVALID_ARG;
    *dst = NULL;
    *dst_len = 0;

    if (!codec_lock()) return ESP_ERR_NO_MEM;
    memset(&s_stages, 0, sizeof(s_stages));

    uint8_t *rgb = NULL;
    uint32_t w = 0, h = 0, stride = 0;
    int64_t t0 = esp_timer_get_time();
    esp_err_t ret = decode_rgb(src, src_len, &rgb, NULL, &w, &h, &stride);
    s_stages.decode_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (ret != ESP_OK) { codec_unlock(); return ret; }

    uint32_t ow = w, oh = h;
    if (max_dim > 0) {
        uint32_t larger = w > h ? w : h;
        if (larger > (uint32_t)max_dim) {
            ow = (uint32_t)((uint64_t)w * (uint32_t)max_dim / larger) & ~1u;
            oh = (uint32_t)((uint64_t)h * (uint32_t)max_dim / larger) & ~1u;
        }
    }
    if (ow < 16 || oh < 16) { ow = w; oh = h; }

    const uint8_t *enc_src = rgb;

    if (ow != w || oh != h || stride != w) {
        int64_t t1 = esp_timer_get_time();
        uint8_t *scaled = scratch_get((size_t)ow * oh * 3u);
        if (!scaled) { codec_unlock(); return ESP_ERR_NO_MEM; }
        resample_region(rgb, stride, 0, 0, w, h, scaled, ow, oh);
        s_stages.resample_ms = (uint32_t)((esp_timer_get_time() - t1) / 1000);
        enc_src = scaled;
    }

    t0 = esp_timer_get_time();
    ret = encode_rgb(enc_src, ow, oh, quality, dst, dst_len);
    s_stages.encode_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    codec_unlock();

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Re-encoded %lux%lu -> %lux%lu (q=%d): %u -> %u bytes",
                 w, h, ow, oh, quality, (unsigned)src_len, (unsigned)*dst_len);
    }
    return ret;
}

esp_err_t jpeg_crop_scale(const uint8_t *src, size_t src_len,
                          const view_spec_t *view, int quality,
                          uint8_t **dst, size_t *dst_len)
{
    if (!src || !src_len || !dst || !dst_len) return ESP_ERR_INVALID_ARG;
    *dst = NULL;
    *dst_len = 0;

    view_spec_t v = VIEW_SPEC_FULL;
    if (view) v = *view;
    if (!view_normalize(&v)) v = (view_spec_t)VIEW_SPEC_FULL;

    if (!codec_lock()) return ESP_ERR_NO_MEM;
    memset(&s_stages, 0, sizeof(s_stages));

    uint8_t *rgb = NULL;
    uint32_t w = 0, h = 0, stride = 0;
    int64_t t0 = esp_timer_get_time();
    esp_err_t ret = decode_rgb(src, src_len, &rgb, NULL, &w, &h, &stride);
    s_stages.decode_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (ret != ESP_OK) { codec_unlock(); return ret; }

    /* Crop rectangle in pixels, even-aligned and fully inside the image. */
    uint32_t cw = (uint32_t)((v.x1 - v.x0) * (float)w) & ~1u;
    uint32_t ch = (uint32_t)((v.y1 - v.y0) * (float)h) & ~1u;
    if (cw > w) cw = w & ~1u;
    if (ch > h) ch = h & ~1u;
    if (cw < 16) cw = (w >= 16) ? 16 : (w & ~1u);
    if (ch < 16) ch = (h >= 16) ? 16 : (h & ~1u);

    uint32_t cx = (uint32_t)(v.x0 * (float)w) & ~1u;
    uint32_t cy = (uint32_t)(v.y0 * (float)h) & ~1u;
    if (cx + cw > w) cx = (w > cw) ? (w - cw) : 0;
    if (cy + ch > h) cy = (h > ch) ? (h - ch) : 0;

    uint32_t ow = (cw / v.scale) & ~1u;
    uint32_t oh = (ch / v.scale) & ~1u;
    if (ow < 16 || oh < 16) { ow = cw; oh = ch; }   /* too small to downscale */

    const uint8_t *enc_src = NULL;

    /* Fast path: the whole frame with no scaling, and rows already packed. */
    if (cx == 0 && cw == w && ow == cw && oh == ch && stride == w) {
        enc_src = rgb + (size_t)cy * stride * 3u;
    } else {
        int64_t t1 = esp_timer_get_time();
        uint8_t *tight = scratch_get((size_t)ow * oh * 3u);
        if (!tight) { codec_unlock(); return ESP_ERR_NO_MEM; }
        resample_region(rgb, stride, cx, cy, cw, ch, tight, ow, oh);
        s_stages.resample_ms = (uint32_t)((esp_timer_get_time() - t1) / 1000);
        enc_src = tight;
    }

    ESP_LOGI(TAG, "View ROI px(%lu,%lu)-(%lu,%lu) of %lux%lu scale=%u -> %lux%lu",
             cx, cy, cx + cw, cy + ch, w, h, v.scale, ow, oh);

    t0 = esp_timer_get_time();
    ret = encode_rgb(enc_src, ow, oh, quality, dst, dst_len);
    s_stages.encode_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    codec_unlock();

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "View encoded: %u -> %u bytes", (unsigned)src_len,
                 (unsigned)*dst_len);
    }
    return ret;
}
