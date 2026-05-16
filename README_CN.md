# Victrl

## ——即插即用的 HID AI Agent

[中文|[English](README.md)]

当今 AI Agent 依赖于在目标设备上进行软件的基础配置以操控设备，Victrl 的目标是构建一个独立于被控设备操作系统的 AI Agent 纯硬件设备，通过 “视觉输入 + HID 输出” 的类人方式，实现对任意设备即插即用式的自动化操作。

Victrl 不依赖 OS API、Accessibility、ADB、VNC、RDP……而是完全模拟 “人类使用电脑” 的过程：

```
解析屏幕 → 操作键鼠 → 观察结果
```

整个项目本质上是在探索一种：Hardware AI Agent 的新型架构