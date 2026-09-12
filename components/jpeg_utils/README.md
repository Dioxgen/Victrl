# jpeg_utils — hardware JPEG codec utilities

> **English** | [中文](README_CN.md)

## Overview

This component wraps the ESP32-P4 hardware JPEG codec and collapses the "decode → software crop/downscale → re-encode" pipeline into two entry points: `jpeg_compress()` for whole-frame downscaling, and `jpeg_crop_scale()` for rendering the region and magnification a `view_spec_t` describes. The hardware decoder can neither scale nor crop, so any ROI or downscale necessarily contains one full-resolution decode followed by a software box-average resample; the decoder engine, the encoder engine and their buffers are cached across calls and rebuilt only when the frame geometry changes.

## Public API

### View (ROI) type and helpers

| Symbol | Description |
| --- | --- |
| `view_spec_t` | View description: `y0`, `x0`, `y1`, `x1` are full-frame normalised coordinates (the visible region); `scale` is an integer downscale applied on top of the crop, restricted to `1` / `2` / `4`. |
| `VIEW_SPEC_FULL` | Full frame with `scale = 1`, usable directly as an initialiser. |
| `VIEW_MIN_EXTENT` | Smallest ROI extent per axis, `0.02f` (full-frame normalised). |
| `bool view_is_full(const view_spec_t *v)` | True when the bounds cover the whole frame and `scale <= 1`; `NULL` counts as the full frame. |
| `bool view_equal(const view_spec_t *a, const view_spec_t *b)` | Compares two views: `0.0005f` tolerance on the float components, exact comparison on `scale`. |
| `bool view_normalize(view_spec_t *v)` | Repairs a view in place: orders the bounds, clamps into `[0,1]`, enforces the minimum extent and restricts `scale` to `1` / `2` / `4`. Returns `false` when no usable region was supplied, in which case the caller should fall back to the full frame. |
| `void view_map_to_full(const view_spec_t *v, float x, float y, float *fx, float *fy)` | Maps a normalised point inside the view back to full-frame normalised coordinates (the inverse of the crop). |

### JPEG transforms

| Function | Description |
| --- | --- |
| `esp_err_t jpeg_compress(const uint8_t *src, size_t src_len, uint8_t **dst, size_t *dst_len, int quality, int max_dim)` | Decodes, box-downscales the longest edge to `max_dim`, re-encodes. `max_dim == 0` means "do not scale", but the frame is still decoded and re-encoded; callers wanting a pure pass-through should not call this function. |
| `esp_err_t jpeg_crop_scale(const uint8_t *src, size_t src_len, const view_spec_t *view, int quality, uint8_t **dst, size_t *dst_len)` | Crops to `view` and applies `view->scale`, then re-encodes. A `NULL` or unusable `view` is treated as the full frame. |

### Cache and timing

| Function | Description |
| --- | --- |
| `const jpeg_stage_timing_t *jpeg_utils_last_stages(void)` | Per-stage timing, in milliseconds, of the most recent `jpeg_compress()` / `jpeg_crop_scale()`: `setup_ms` (engine and buffer creation, a subset of the two below), `decode_ms` (full-resolution hardware decode), `resample_ms` (software crop + box-average downscale), `encode_ms` (hardware re-encode). |
| `void jpeg_utils_release_cache(void)` | Releases the cached decoder/encoder engines, the decode buffer and the scratch buffer; for diagnostics or shutdown. |

## Notes

- `*dst` comes from the encoder's memory allocator and **the caller is responsible for `free()`ing it**; on failure `*dst == NULL` and `*dst_len == 0`.
- All buffers are owned by the component internally; callers must not allocate them. `jpeg_alloc_decoder_mem()` / `jpeg_alloc_encoder_mem()` are not part of this component's public API, and the only thing the caller ever frees is the `*dst` above.
- The decode buffer is sized for the **source** geometry and padded to the driver's 16-byte alignment: it is `align16(w) * align16(h) * 3` bytes and its row stride in pixels equals the padded width. Sizing it for the downscaled target makes every decode fail with `JPEG_ERR_NO_MEM`.
- The hardware decoder can neither scale nor crop, so `jpeg_crop_scale()` always costs one full-resolution decode first; the ROI and `scale` only shrink the output, trading the same uplink bandwidth for higher effective resolution — which is the entire point of a zoom.
- The decoder engine, the encoder engine and the decode buffer are cached across calls and rebuilt only when the frame geometry (padded width and height) changes, so `setup_ms` should be near 0 after the first call.
- The decode buffer is returned as a **borrowed** pointer: its contents stay valid only until the next jpeg_utils call, it must not be `free()`d, and it must not be held across calls. The component is not reentrant; an internal mutex serialises calls, so concurrent callers block.
- Whenever pixels have to be repacked, an internal scratch buffer is used; it only ever grows, is explicitly `MALLOC_CAP_SPIRAM`, allocates nothing in the steady state and is released only by `jpeg_utils_release_cache()`.
- The downscale factor is quantised rather than rounded: `scale` accepts only `1`, `2` and `4`, and any other value is rewritten to `1` during normalisation.
- Regions have a floor: when an axis spans less than `0.02` (full-frame normalised), it is grown symmetrically about the centre and then clamped back inside the frame; at the pixel level the crop rectangle is even-aligned and at least 16 pixels wide and tall; and when the downscaled result would be under 16 pixels on either axis, the downscale is dropped and the crop is emitted at its own size.
- `quality <= 0` makes the hardware encoder use `60`; the component does not clamp `quality` to 1..100 (the project passes `UPLOAD_JPEG_QUALITY`).
- With `max_dim > 0`, the frame is only shrunk when its longest edge exceeds `max_dim` (never enlarged) and the output dimensions are even-aligned; if either axis would fall under 16 pixels, the original dimensions are kept.
- Fast path: `jpeg_crop_scale()` does not copy pixels for "full frame + `scale = 1` + row stride equal to the source width (i.e. the source width is already a multiple of 16)" — it hands the decode buffer to the encoder with a row offset — but it is **still** a full-resolution decode; `jpeg_compress()` likewise skips the resample when nothing is scaled and the row stride equals the source width.
- The decoder output is RGB888 with the RGB element order set to BGR (`JPEG_DEC_RGB_ELEMENT_ORDER_BGR`); the encoder input is RGB888 as well, sub-sampled as YUV422 (`JPEG_DOWN_SAMPLING_YUV422`).
