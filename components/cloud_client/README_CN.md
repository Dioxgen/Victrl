# cloud_client — LLM API 客户端

## 简介

通过 HTTPS POST 调用火山方舟 Ark Responses API，发送屏幕截图 + 上下文，获取模型动作指令。

## 子模块

| 文件 | 功能 |
|------|------|
| `cloud_client.c` | HTTPS POST 发送/接收，重试逻辑 |
| `json_parser.c` | cJSON 解析模型响应，支持容错（markdown 代码块、截断 JSON） |
| `base64.c` | mbedtls Base64 编码 JPEG 帧 |
| `system_prompts.c` | 从 SD 卡加载系统提示词，运行时格式化占位符 |

## API 调用流程

1. 格式化系统提示词（替换 {device_profile_text}、{last_summary} 等占位符）
2. 调用 `jpeg_utils` 压缩 JPEG（质量可配，默认 < 80KB 不压缩）
3. Base64 编码压缩后的 JPEG
4. 构建 JSON 请求体（model + instructions + input + temperature + max_output_tokens）
5. HTTPS POST 到 `api_endpoint`（默认 `https://ark.cn-beijing.volces.com/api/v3/responses`）
6. 从响应中提取 `output[].content[].text`（output_text）→ cJSON 解析
7. 同时提取 `output[].summary[].text`（reasoning）

## 重试策略

最多 3 次重试，指数退避 2/4/8 秒。

## 注意事项

- 使用 HTTP/1.1 + keep-alive 关闭（每次新连接）
- TLS 输出缓冲区需 >= 16KB（mbedtls 配置），否则大请求体上传超时
- `max_output_tokens` 在 config.json 中配置（默认 16384）
- Thinking 模式通过 `api.enable_thinking` 控制
