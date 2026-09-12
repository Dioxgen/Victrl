# storage_manager — 存储管理

## 简介

SD 卡数据管理组件，包含配置加载、Profile 管理、计划管理、短期记忆、任务日志。

## 子模块

| 文件 | 功能 |
|------|------|
| `config_loader.c` | 从 SD 卡 `/config.json` 读取运行时配置 |
| `profile_manager.c` | 多设备 Profile (.md 格式) 读取/追加 |
| `plan_manager.c` | 任务计划 JSON 保存/加载，里程碑追踪 |
| `short_term_mem.c` | 短期记忆环缓冲区（10 条上限，超量压缩合并） |
| `task_log.c` | 任务日志文件写入（/log/<task_id>/） |

## SD 卡路径结构

```
/sdcard/
├── config.json           # 运行时配置
├── sysprompt/
│   ├── full.txt          # 完整系统提示词
│   └── short.txt         # 精简系统提示词
├── profiles/             # 设备 Profile
├── plans/                # 任务计划 JSON
├── log/
│   └── <task_id>/        # 每次任务的日志+截图
│       ├── <task_id>.log
│       └── <task_id>_stepNNNN.jpg
└── web/
    └── index.html        # WebUI
```

## 任务日志内容

每步记录：
- 时间戳（NTP 同步后为北京时间）
- 提示词类型（full/short）
- 请求/响应字节数、API 耗时
- 动作摘要、观察、自评、推理过程
- 任务计划 JSON、完整模型响应

## 短期记忆

- 环缓冲区，默认 10 条
- 超量时合并最早两条为一条（分号分隔）
- Mutex 保护多线程访问（主循环写、WebUI 读）

## 注意事项

- 日志文件使用 `_IONBF` 无缓冲模式，每步写入后 `fsync` 强制落盘
- 日志在所有退出路径（done/stop/emergency/max_actions）均关闭
- FatFS 需开启 `FF_USE_LFN` 和足够的 `FF_FS_LOCK`
- 所有 FatFS 操作共享全局 `sd_access_mutex`
