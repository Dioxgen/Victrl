# jpeg_utils — 硬件 JPEG 编解码工具

> **中文** | [English](README.md)

## 简介

本组件封装 ESP32-P4 硬件 JPEG 编解码器，把“解码 → 软件裁剪/降采样 → 重编码”这条流水线收敛成两个入口：`jpeg_compress()` 负责整帧降采样，`jpeg_crop_scale()` 按 `view_spec_t` 描述的区域与倍率出图。硬件解码器既不能缩放也不能裁剪，所以任何 ROI 或降采样都必然包含一次全分辨率解码，再由软件做盒式平均重采样；解码引擎、编码引擎及其缓冲区跨调用缓存，只在帧几何变化时才重建。

## 公开 API

### 视图（ROI）类型与辅助函数

| 符号 | 说明 |
| --- | --- |
| `view_spec_t` | 视图描述：`y0`、`x0`、`y1`、`x1` 是全帧归一化坐标（可见区域），`scale` 是裁剪之后叠加的整数降采样倍率，取 `1`／`2`／`4`。 |
| `VIEW_SPEC_FULL` | 全帧、`scale = 1` 的初始值，可直接赋值初始化。 |
| `VIEW_MIN_EXTENT` | 单轴最小 ROI 跨度，`0.02f`（全帧归一化）。 |
| `bool view_is_full(const view_spec_t *v)` | 边界覆盖整个帧且 `scale <= 1` 时为真；`NULL` 视为全帧。 |
| `bool view_equal(const view_spec_t *a, const view_spec_t *b)` | 比较两个视图：浮点分量容差 `0.0005f`，`scale` 精确比较。 |
| `bool view_normalize(view_spec_t *v)` | 就地修正视图：排序边界、夹入 `[0,1]`、补足最小跨度、把 `scale` 限制为 `1`／`2`／`4`。返回 `false` 表示没有可用区域，调用方应回退到全帧。 |
| `void view_map_to_full(const view_spec_t *v, float x, float y, float *fx, float *fy)` | 把视图内的归一化点还原成全帧归一化坐标（裁剪的逆映射）。 |

### JPEG 变换

| 函数 | 说明 |
| --- | --- |
| `esp_err_t jpeg_compress(const uint8_t *src, size_t src_len, uint8_t **dst, size_t *dst_len, int quality, int max_dim)` | 解码、把最长边降采样到 `max_dim`、重编码。`max_dim == 0` 表示不缩放，但仍会解码并重编码；想原样透传就不要调用本函数。 |
| `esp_err_t jpeg_crop_scale(const uint8_t *src, size_t src_len, const view_spec_t *view, int quality, uint8_t **dst, size_t *dst_len)` | 按 `view` 裁剪并叠加 `view->scale`，再重编码。`view` 为 `NULL` 或归一化失败时按全帧处理。 |

### 缓存与计时

| 函数 | 说明 |
| --- | --- |
| `const jpeg_stage_timing_t *jpeg_utils_last_stages(void)` | 最近一次 `jpeg_compress()`／`jpeg_crop_scale()` 的分阶段耗时（毫秒）：`setup_ms`（引擎与缓冲区创建，是后两项的子集）、`decode_ms`（全分辨率硬件解码）、`resample_ms`（软件裁剪 + 盒式平均降采样）、`encode_ms`（硬件重编码）。 |
| `void jpeg_utils_release_cache(void)` | 释放缓存的解码/编码引擎、解码缓冲区与 scratch 缓冲区；用于诊断或关机。 |

## 注意事项

- `*dst` 由编码器内存分配器给出，**调用方负责 `free()`**；失败时 `*dst == NULL`、`*dst_len == 0`。
- 缓冲区全部由组件内部管理，调用方不要自行分配：`jpeg_alloc_decoder_mem()` / `jpeg_alloc_encoder_mem()` 不是本组件的公开 API，调用方唯一的释放责任就是上面的 `*dst`。
- 解码缓冲区按**源图**几何分配，并按驱动的 16 字节对齐补齐：大小为 `align16(w) * align16(h) * 3`，行跨距（以像素计）等于对齐后的宽度。按降采样后的目标尺寸分配会让每次解码都以 `JPEG_ERR_NO_MEM` 失败。
- 硬件解码器既不能缩放也不能裁剪，`jpeg_crop_scale()` 总是先做一次全分辨率解码；ROI 与 `scale` 只缩小输出，用同样的传输带宽换到更高的有效分辨率，这是缩放的唯一收益。
- 解码引擎、编码引擎与解码缓冲区跨调用缓存，只在帧几何（对齐后的宽高）变化时重建，因此首次调用之后 `setup_ms` 应接近 0。
- 解码缓冲区以**借用**形式返回：内容只在下一次 jpeg_utils 调用之前有效，不得 `free()`，也不得跨调用持有。组件不可重入，内部用互斥锁把调用串行化，并发调用会被阻塞。
- 重排像素时使用内部 scratch 缓冲区，只增不减且显式来自 `MALLOC_CAP_SPIRAM`，稳态下不再分配；只有 `jpeg_utils_release_cache()` 会释放它。
- 降采样倍率是量化而非就近取整：`scale` 只接受 `1`、`2`、`4`，其他值在归一化时被改为 `1`。
- 区域有下限：每轴 ROI 跨度不足 `0.02`（全帧归一化）时，以中心为基准对称补足后再夹回帧内；像素层面的裁剪矩形按偶数对齐且不小于 16 像素；若降采样后任一边不足 16 像素，则放弃降采样、按裁剪尺寸原样输出。
- `quality <= 0` 时硬件编码器改用 `60`；本组件不对 `quality` 做 1~100 的夹取（工程内实际传入 `UPLOAD_JPEG_QUALITY`）。
- `max_dim > 0` 时只在最长边超过 `max_dim` 的情况下缩小（不放大），输出宽高按偶数对齐；若任一边不足 16 像素则回退为原始尺寸。
- 快速路径：`jpeg_crop_scale()` 在“整帧 + `scale = 1` + 行跨距等于源宽（即源宽已是 16 的倍数）”时不复制像素，直接把解码缓冲区按行偏移交给编码器，但**仍然**是全分辨率解码；`jpeg_compress()` 在“不缩放且行跨距等于源宽”时同样跳过重采样。
- 解码输出为 RGB888，RGB 元素顺序设为 BGR（`JPEG_DEC_RGB_ELEMENT_ORDER_BGR`）；编码输入同为 RGB888，子采样为 YUV422（`JPEG_DOWN_SAMPLING_YUV422`）。
