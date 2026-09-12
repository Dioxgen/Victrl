# cloud_client — LLM API 客户端

> **中文** | [English](README.md)

## 简介

通过 HTTPS 调用 Responses API，发送屏幕截图与上下文字符串，返回模型动作 JSON；客户端句柄常驻以复用 keep-alive 连接，并提供逐阶段耗时统计。

## 子模块

| 文件 | 功能 |
|------|------|
| `cloud_client.c` | 请求体组装、图像裁剪/缩放与复用、HTTPS 收发、重试与分阶段计时 |
| `json_parser.c` | 解析模型返回的动作 JSON：剥离 markdown 代码块、兼容旧扁平格式、丢弃非法动作条目、补齐默认字段 |
| `json_safe.h` | 防御式 cJSON 取值（`jget_str` / `jget_num` / `jget_int` / `jget_bool` / `jget_array_ok`），字段缺失、为 null 或类型不符时返回调用者给的默认值，绝不返回 NULL 字符串 |
| `base64.c` | 用 mbedtls 对 JPEG 做 Base64 编码，输出由 `malloc` 分配，调用者负责释放 |
| `system_prompts.c` | 从 SD 卡加载 `full.txt` / `short.txt` 系统提示词，不做任何占位符替换 |

## 公开 API

| 接口 | 说明 |
|------|------|
| `esp_err_t cloud_client_init(const cloud_client_config_t *cfg)` | 保存并校验配置：`api_endpoint` 与 `api_key` 必须非空，否则返回 `ESP_ERR_INVALID_ARG`；`max_retries` 上限为 3；非法的 `reasoning_effort` / `reasoning_effort_escalated` 分别回落到 `low` / `high` |
| `esp_err_t cloud_client_query(...)` | 主查询入口，共 15 个参数：图像数据与长度、`system_prompt`、`user_text`、`plan_json`、`history_text`、`profile_text`、`last_summary`、`status_text`、`effort`、`view`、`reuse_image`、`out_response`、`step_number`、`out_timing` |
| `bool cloud_client_can_reuse_image(const view_spec_t *view)` | 上次编码结果能否复用；只有缓存存在、长度大于 0 且 view 与缓存时一致才为真 |
| `const uint8_t *cloud_client_get_last_jpeg(size_t *out_len)` | 最近一次真正发给模型的 JPEG 字节（WebUI 的 `/api/snapshot` 用它显示模型看到的画面），首次成功发送前为 NULL |
| `cloud_client_timing_t` | 单次调用的分阶段耗时（毫秒）：`prep_ms`、`compress_ms`、`jpeg_setup_ms`、`jpeg_decode_ms`、`jpeg_resample_ms`、`jpeg_encode_ms`、`base64_ms`、`serialize_ms`、`connect_ms`、`http_ms`、`parse_ms`；未执行的阶段为 0 |

## 调用流程

1. 图像准备：`reuse_image` 且 `cloud_client_can_reuse_image()` 为真时直接使用缓存的 Base64；否则 view 不是全屏时调用 `jpeg_crop_scale()` 裁剪/缩放，view 为全屏且 `upload_max_dim > 0` 且帧大于 `COMPRESS_THRESHOLD_BYTES`（80000 字节）时调用 `jpeg_compress()` 缩放，其余情况原样上传；重编码质量是固定的 `UPLOAD_JPEG_QUALITY`（50），不可配置。
2. `base64_encode()` 编码，结果存入缓存，并按 `save_frames` 决定是否用 `task_log_save_frame()` 写 SD 卡。
3. 组装请求 JSON：`model`、`instructions`（原样使用 `system_prompt`，以保住前缀缓存）、`input[0].content[]`（一个 `input_text` 块加一个 `input_image` 块，`image_url` 为 `data:image/jpeg;base64,...`）、`temperature` 固定 0.1、`max_output_tokens`、`reasoning.effort`；可变内容（目标、状态、设备档案、上一步动作、计划、历史）由 `build_user_text()` 拼进用户消息。
4. UTF-8 兜底：`utf8_sanitize()` 扫描整个请求体，把非法字节替换为 `?` 并打警告日志。
5. 发送：POST 到 `api_endpoint`，带 `Content-Type: application/json` 与 `Authorization: Bearer <api_key>`。
6. 响应解析：HTTP 200 且数据接收完整时，遍历 `output[]`，取 `type == message` 中 `content[].type == output_text` 的 `text` 作为动作文本，取 `type == reasoning` 的 `summary[0].text`（缺失时退化到其 `content[].text`）作为思考内容并写入 `_reasoning`；动作文本交给 `json_parser_parse()`，成功后再附加 `_raw_response`、`_request_size`、`_response_size`。

## 注意事项

- 连接复用：HTTP 客户端句柄常驻，`keep_alive_enable` 为 true；只有传输出错、状态码非 200 或数据未收全时才 `client_teardown()` 主动断开（200 但解析失败时连接保留），因此 `connect_ms == 0` 表示这一步复用了已有连接。
- 重试：总尝试次数为 `max_retries + 1`（`max_retries` 被限制在 3 以内），退避表为 `{0, 2000, 4000, 8000}` 毫秒，即首次不等待、之后 2/4/8 秒；4xx 中只有 408 和 429 会重试，其余 4xx 判定为不可重试并直接放弃本步。
- 返回值不能作为唯一成功判据：HTTP 状态码非 200 或响应体解析失败时，函数仍可能返回 `ESP_OK`，调用者必须同时检查 `*out_response` 是否为 NULL（`agent_core` 正是这样判断的）。
- 解析容错：`json_parser_parse()` 先剥离 markdown 代码块，首次解析失败则截取第一个 `{` 到最后一个 `}` 重试；结果不是 JSON 对象、`actions` 不是数组、或缺少 `plan_update` 时返回 NULL，本步按失败重试；`actions` 中 `action_type` 缺失/为空/非字符串的条目会被就地删除，合法条目的 `action_type` 统一转小写。
- 默认值：`json_parser_set_action_defaults()` 补 `delay_after = 0.05`、`button = left`、`hold = 0`、`wait_seconds = 0.0`；`json_parser_set_response_defaults()` 补 `need_screen = true`、`sleep_before_next = 0.0`、`done = false`、`request_profile` 为空串、`profile_updates` 为空数组。
- 提示词：`system_prompts_init()` 必须读到 `sysprompt_dir` 下的 `full.txt`，否则返回 `ESP_FAIL`；`short.txt` 缺失时用 `full.txt` 兜底。提示词里没有占位符，不需要格式化。
- 请求体较大：因为带 Base64 图片，HTTP 客户端发送缓冲设为 `buffer_size_tx = 16384`、接收缓冲 `buffer_size = 4096`，构建树里的 `sdkconfig` 也把 `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN` 设为 16384（该文件不纳入版本控制）。
- 配置键都在 `config.json` 的 `api` 段，默认值来自 `storage_manager/config_loader.c`：`endpoint`（默认 `https://api.deepseek.com/responses`）、`key`、`model_name`、`timeout`、`max_retries`（默认 3）、`max_output_tokens`（默认 16384）、`enable_thinking`、`reasoning_effort`、`reasoning_effort_escalated`、`upload_max_dim`（`config_loader` 默认 1360，示例配置写 0）、`save_frames`。
- Thinking 没有真正的“关闭”：`enable_thinking = false` 只是把两档 effort 都压成 `low`，请求里发送的是 `reasoning.effort`（`low` / `high` / `max`）。
- `cloud_client_query()` 只强制要求 `out_response` 非空，其余指针参数可为 NULL：图像为空则不发送图片块，各文本参数走 `build_user_text()` 的兜底文案，`effort` 为空用配置值，`view` 为空按全屏，`out_timing` 为空则丢弃计时。
