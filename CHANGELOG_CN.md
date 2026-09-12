# 更新日志

> **中文** | [English](CHANGELOG.md)

简洁记录改了什么、什么时候改的。日期是提交日期而非发布日期——本项目没有发布节奏；除非条目里明确写了，否则不声称某项改动已在硬件上验证过。

## 2026-09-12 — 组件文档审计

### 修复

`wifi_manager`：链路一断就清空 STA 地址，因此 `wifi_manager_is_connected()`（读的是 `s_ip_addr`）在掉线后不再报告“已连接”，屏幕和 WebUI 也不再显示过期地址。

`wifi_manager`：SNTP 服务器名只会写进 `time_status_t.servers` 真正拥有的槽位，且 `n_servers` 不会超过实际拷贝的数量；把 `CONFIG_LWIP_SNTP_MAX_SERVERS` 调到 3 以上不会再写越界——`web_server.c` 正是按 `n_servers` 索引这个数组的。

`storage_manager`：合并最旧两条的内存分配失败时，`stm_add()` 不再写到短期记忆数组末尾之外，改为丢弃最旧一条。`strdup()` 失败时也不再存进一个 NULL 空洞——那会让每个用 `strlen()` 遍历的读者崩溃。

`storage_manager`：`meta.json` 在每步重写后仍保留 `created` 字段，会话不再在第一次更新步数时丢掉创建时间。

`agent_core`：“单步”现在真的只跑一次就停，并且跳过步间休眠。STEP 到 PAUSED 的转换原本不可达——`agent_should_continue()` 对 STEP 返回真——所以单步的表现和 RUN 完全一样，永远不会暂停。

`sdmmc_driver`：挂载失败的日志改成真实存在的 Kconfig 选项名（`CONFIG_SDMMC_DRV_FORMAT_IF_MOUNT_FAILED`）。

### 变更

12 个组件的 README 全部对照源码审计并重写；每个组件现在都有与 `README_CN.md` 并列的英文 `README.md`。审计纠正了与代码不符的说法，包括并不存在的 `jpeg_get_info()`、`ESP_COLOR_FOURCC_BGR24`、`sd_access_mutex`，JPEG 质量默认值（是 60 不是 25），以磅而非像素描述的字体，以及过期的 UVC 流结构字段。

根目录 README 增加了指向各组件文档的入口。

### 删除

三个过期的 `README.md.old`。

### 已记录但有意不改的问题

`NV3007_driver`：`F9/F2 fix` 段落里的 `NV3007_WriteByte(0x17)` 是一个没有前置寄存器写的裸数据字节，`0xF9` 从未被寻址——很可能是漏了 `C8(0xF9,0x17)`。但面板现在初始化正常、显示正常，所以这段时序保持原样。

`wifi_manager`：`time_status_t.failures` 暴露在 `/api/status` 里，却从来没有人给它赋值。lwIP 的 SNTP 没有失败回调，要给出真实数值需要一个“已启动但一直没同步”的看门狗判定。字段暂时保留，但请把 0 理解为“未测量”，而不是“没有失败”。

## 2026-09-12 — 智能体回路跑在 ESP32-P4 上

回路从 PC 搬进了芯片。MS2109 采集卡的 UVC 取帧、P4 硬件 JPEG 编解码、TinyUSB 复合 HID 输出、SD 卡存储、C6 上的 ESP-Hosted WiFi、NV3007 状态屏和 WebUI 全部在设备上运行，PC 不再参与回路。

Linux/Python 形态的 MVP 降级到 `prototype/` 保留作参考；移植的代价见 `docs/原型.md`。

行尾用 `.gitattributes` 固定为 LF，让 SD 卡上的系统提示词在不同检出之间保持逐字节一致——API 的前缀缓存正依赖这一点。

## 2026-09-12 — 文档集合

所有文档都有中英对照：`Technical-Document` / `技术文档`、`Failure-Taxonomy` / `失败分类学`、`Input-Method-Troubles` / `输入法问题`、`Prototype` / `原型`、`MCU-Port` / `MCU移植`、`Compliance` / `合规与授权说明`。

硬件照片放进 `Images/`，架构图改写为 Mermaid 8 兼容语法，wiki 合并进 `docs/`。

## 2026-05 — Linux/Python 原型（V2.0）

最初的 MVP：回路跑在 PC 上，通过 USB 采集卡和 HID 设备操作目标机器。失败分类学和输入法问题的大部分结论都出自它。保留在 `prototype/` 下作参考。
