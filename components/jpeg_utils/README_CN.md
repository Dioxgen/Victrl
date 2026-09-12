# jpeg_utils — 硬件 JPEG 编解码工具

## 简介

封装 ESP32-P4 硬件 JPEG 编解码器，提供 JPEG 解压→压缩流水线。

## API

### `jpeg_compress()`

解码 JPEG → RGB → 低质量重新编码。用于减小发给 LLM 的图片体积。

参数：
- `quality`：1~100，越低体积越小（默认 25）
- `max_dim`：0 不变，>0 等比缩放到最大边长

返回压缩后的 JPEG 数据（调用者负责 free）。

### `jpeg_get_info()`

快速读取 JPEG 宽高，不解码。

## 流程

1. `jpeg_decoder_get_info()` 获取原始宽高
2. 使用 `jpeg_alloc_decoder_mem()` 分配对齐的 RGB 缓冲区
3. 硬件解码器解码 JPEG → RGB888
4. 使用 `jpeg_alloc_encoder_mem()` 分配对齐的输出缓冲区
5. 硬件编码器编码 RGB → JPEG（指定 quality）

## 注意事项

- **必须**使用 `jpeg_alloc_decoder_mem()` / `jpeg_alloc_encoder_mem()` 分配缓冲区，不能用普通 malloc（数据对齐要求）
- 编解码器使用 SPIRAM 分配大缓冲区
- 解码器输出格式为 RGB888（`ESP_COLOR_FOURCC_BGR24`），注意 RGB 元素顺序设置为 BGR
- 编码器输入格式同样为 RGB888
