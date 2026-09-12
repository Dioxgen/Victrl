# web_server — HTTP 服务器 + WebUI

## 简介

基于 esp_http_server 的轻量 HTTP/1.1 服务器，提供 Web Dashboard 和 REST API。

## 路由

| 路由 | 方法 | 功能 |
|------|------|------|
| `/` | GET | 从 SD 卡 `/web/index.html` 加载 WebUI |
| `/api/status` | GET | JSON 返回系统状态（状态/步数/WiFi/堆内存） |
| `/api/start` | POST | 开始任务（body: `{"task":"..."}`） |
| `/api/stop` | POST | 停止任务 |
| `/api/pause` | POST | 暂停任务 |
| `/api/resume` | POST | 恢复任务 |
| `/api/step` | POST | 单步执行 |
| `/api/emergency` | POST | 紧急停止 |
| `/api/profiles` | GET | 列出可用 Profile |
| `/api/plan` | GET | 当前任务计划 JSON |
| `/api/log` | GET | 最近动作日志（含状态/步数/任务名） |

## WebUI

单页 HTML Dashboard，2 秒自动刷新：
- 任务控制区（输入框 + 开始/暂停/继续/单步/停止/急停按钮）
- 系统状态（WiFi、IP、步数、堆内存）
- 任务计划里程碑
- 实时日志

## 注意事项

- 需要在 main 中先初始化 `esp_netif` + `esp_event_loop`，否则 HTTP 服务器无法创建 socket
- WebUI HTML 从 SD 卡 `/web/index.html` 加载，可随时替换无需重新编译
- `lwip_max_sockets` 配置为 16（WiFi + HTTP + Cloud Client 共用）
- 日志接口返回 JSON 转义后的内容（\n → \\n, " → \"）
