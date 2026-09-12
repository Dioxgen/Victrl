#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How a frame is presented to the model.
 *
 * Everything the model returns is normalised to the image it was SHOWN, so any
 * non-default view has to be undone before those coordinates can drive the
 * mouse. `roi` is the visible region in FULL-FRAME normalised coordinates;
 * `scale` is an extra integer downscale applied after cropping.
 *
 * Note that `scale` needs no coordinate handling at all — normalised
 * coordinates are resolution independent. Only the ROI moves the origin, so
 * exactly one affine map (view_map_to_full) stands between what the model sees
 * and where the pointer goes. Keep it that way: this is the single place where
 * a systematic offset could be introduced.
 */
typedef struct {
    float   y0, x0, y1, x1;
    uint8_t scale;          /* 1 | 2 | 4 */
} view_spec_t;

#define VIEW_SPEC_FULL { .y0 = 0.0f, .x0 = 0.0f, .y1 = 1.0f, .x1 = 1.0f, .scale = 1 }

/*
 * Smallest ROI extent honoured, per axis, in full-frame normalised units.
 * Anything smaller is below the API's own upscale threshold, so the model would
 * gain no extra resolution — only a uselessly narrow field of view.
 */
#define VIEW_MIN_EXTENT 0.02f

/* True when the view is the untouched full frame. */
bool view_is_full(const view_spec_t *v);

bool view_equal(const view_spec_t *a, const view_spec_t *b);

/*
 * Repair a model-supplied view: order the bounds, clamp into [0,1], enforce a
 * minimum extent (a 2-pixel ROI is useless and produces a degenerate image), and
 * restrict the scale to 1/2/4. Returns false when nothing usable was supplied,
 * in which case the caller should fall back to the full frame.
 */
bool view_normalize(view_spec_t *v);

/*
 * Map a normalised point inside the view back to full-frame normalised
 * coordinates — the inverse of what the crop did.
 */
void view_map_to_full(const view_spec_t *v, float x, float y, float *fx, float *fy);

/* ── JPEG transforms ─────────────────────────────────────────────────── */

/*
 * Where the time inside a JPEG transform actually goes, in milliseconds.
 *
 * `compress_ms` in the step timing used to be one opaque number, which made it
 * impossible to tell a slow hardware decode from a slow software resample from
 * codec setup overhead — and therefore impossible to say whether the PPA
 * (which can only replace the resample) would help at all. These are the
 * sub-phases of the most recent call.
 *
 * Setup is included in decode_ms/encode_ms (it is a subset, not an extra
 * phase) and should be ~0 after the first frame: the decoder/encoder engines
 * and their buffers are cached across calls and rebuilt only when the frame
 * geometry changes.
 */
typedef struct {
    uint32_t setup_ms;      /* subset of the two below: engine/buffer creation   */
    uint32_t decode_ms;     /* hardware JPEG decode at full resolution            */
    uint32_t resample_ms;   /* software crop + box-average downscale              */
    uint32_t encode_ms;     /* hardware JPEG re-encode                            */
} jpeg_stage_timing_t;

/* Sub-phase breakdown of the most recent jpeg_compress()/jpeg_crop_scale(). */
const jpeg_stage_timing_t *jpeg_utils_last_stages(void);

/* Release the cached codec engines and buffers (diagnostics / shutdown). */
void jpeg_utils_release_cache(void);

/*
 * Decode, box-downscale the longest edge to `max_dim`, re-encode.
 * `max_dim == 0` means "do not scale", but this function still decodes and
 * re-encodes; callers wanting a pure pass-through should not call it at all.
 */
esp_err_t jpeg_compress(const uint8_t *src, size_t src_len,
                         uint8_t **dst, size_t *dst_len,
                         int quality, int max_dim);

/*
 * Crop to `view->roi`, apply `view->scale`, re-encode.
 *
 * The hardware decoder can neither crop nor scale, so this always costs a
 * full-resolution decode plus one software resample — but the result is a much
 * smaller image whose effective resolution (after the API's own server-side
 * resize) is higher, which is the entire point of a zoom.
 *
 * A full-frame scale-1 view is handled without copying the pixels.
 */
esp_err_t jpeg_crop_scale(const uint8_t *src, size_t src_len,
                          const view_spec_t *view, int quality,
                          uint8_t **dst, size_t *dst_len);

#ifdef __cplusplus
}
#endif
