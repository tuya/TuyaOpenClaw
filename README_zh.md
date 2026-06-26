# TuyaOpenClaw

> **Ducky** —— TuyaOpen 的硬件吉祥物 —— 在每一块板子上运行 Claw。
> *（前身为 DuckyClaw。）*

TuyaOpenClaw 是基于 TuyaOpen C SDK 构建的硬件导向 AI Agent。它在边缘设备（Tuya T5AI、ESP32、Raspberry Pi、Linux）上运行 Claw 风格的 Agent 循环，通过 IM 通道（Telegram、Discord、飞书）与用户通信，并直接在设备端执行 MCP 风格的工具调用。

## 支持的硬件

| 类型 | 型号 / 平台 |
|------|------------|
| **MCU** | - Tuya T5AI 模组<br>- ESP32 系列 |
| **SoC** | - Raspberry Pi 4/5/CM4/CM5<br>- Linux ARM SoC（高通 / 瑞芯微 / 全志等）|
| **PC**  | - Linux Ubuntu |

---

本项目基于 TuyaOpen C SDK 构建，支持跨 ARM Cortex-M、ARM Cortex-A 乃至 x64 PC 的灵活部署，并提供丰富的即用型硬件驱动和 API。接入新硬件和外设如同搭积木般简单（传感器、显示屏、麦克风扬声器、摄像头，乃至 IoT 云接入）。

![GitHub Repo Banner](https://images.tuyacn.com/fe-static/docs/img/210f532a-0bb1-4ca5-9037-f5488958a709.jpg)

**你的自主 AI 伴侣。** 简化硬件集成，解锁无限控制可能。

> [!WARNING]
> **🚧 积极开发中** —— 项目处于高强度开发阶段，随时可能出现问题。现在运行可能影响最终体验。遇到任何问题请提 Issue。


## ❓ 为什么选择 TuyaOpenClaw？

大多数 AI Agent 框架虽然强大，但却繁琐。它们往往需要昂贵的订阅费、复杂的配置，并叠加在各种框架和 API 之上。TuyaOpenClaw 提供了一条不同的路。

基于商业级设备–云端 AI Agent 底座，TuyaOpenClaw 将设备端 Agent 与云端能力无缝融合。

只需一个 TuyaOpen Key，即可畅享涂鸦云平台及其新一代设备–云端 AI Agent。

轻量、易部署，适配几乎所有边缘硬件——从 WiFi MCU 到 ARM SoC，再到 Ubuntu PC。


| | TuyaOpenClaw | OpenClaw / MimiClaw / 其他 |
|---|---|---|
| **架构** | 基于 TuyaOpen C SDK 的硬件导向 OpenClaw；设备–云端融合 | OpenClaw：Node.js 全天候桌面/服务器 Agent。MimiClaw：ESP32-S3 裸机 C。其他：框架栈（Pi、Claude Code 等）|
| **部署目标** | MCU（T5AI、ESP32）、SoC（RPi 4/5、ARM Linux）、PC（Ubuntu）；单一代码库 | OpenClaw：Mac mini、Pi、VPS。MimiClaw：5 美元 ESP32-S3。其他：仅服务器/桌面 |
| **运行时** | TuyaOpen C；ARM Cortex-M/A、x64；MCU 无需 Node | OpenClaw：Node.js。MimiClaw：无 OS。其他：Node.js 22+、pnpm、完整 OS |
| **设备–云端** | 一个 TuyaOpen Key → 涂鸦云平台 + 设备；内置设备–云端 AI | OpenClaw：自托管，可选 API 订阅。MimiClaw：纯本地。其他：纯云端或 DIY |
| **消息通道** | Telegram、Discord、飞书（统一 IM 组件：消息总线、代理、TLS）| OpenClaw：WhatsApp、Telegram、Discord、iMessage、Slack 等。MimiClaw：Telegram、WebSocket、串口 CLI |
| **记忆** | MEMORY.md + 日记（YYYY-MM-DD.md）；会话管理器；移植自 MimiClaw 风格存储 | OpenClaw：全天候持久上下文、Obsidian/Raycast。MimiClaw：设备端 MEMORY.md |
| **工具** | CRON、FILE、IoT 设备控制（涂鸦）；EXEC（RPi）；MCP 风格设备工具 | OpenClaw：浏览器自动化、cron、ClawHub Skills。MimiClaw：ReAct + 网页搜索、时间、OTA、GPIO |
| **✨ 上手** | 单个 TuyaOpen Key；选板（T5AI/ESP32/RPi/Linux），配置 IM Token | OpenClaw：网关、OAuth、多步骤。MimiClaw：刷固件、API Key。其他：复杂依赖 |
| **✨ 成本** | 低成本；统一 Key 访问服务 | OpenClaw/MimiClaw：自备 Claude/OpenAI。其他：Claude Pro/Max（$20–200/月）或重度 API |
| **✨ IoT 设备控制** | 原生设备和 IoT 控制，可管理涂鸦生态中的其他设备 | ❌（通常无内置设备控制）|
| **✨ 语音（ASR）输入** | 特定板型支持硬件语音输入（ASR）| ❌（原生不支持语音输入）|


## 💡 设计理念

**Ducky 是 TuyaOpen 的硬件吉祥物** —— 一只能在任何环境中如鱼得水的鸭子，从最小的 MCU 到完整的桌面 Linux 系统。TuyaOpenClaw 继承了这种精神：一套代码，适配所有板子，零妥协。

项目名称本身就是完整的故事。**TuyaOpen** 是底座 —— 将芯片级硬件、IoT 云和 AI 统一于单一 Key 的商业级 C SDK。**Claw** 是 Agent 范式 —— 从 OpenClaw 借鉴的推理–工具调用–行动循环，经过改造以适应物理世界。Ducky 在中间，构成一个越用越聪明的全天候硬件伴侣。

**为什么是鸭子和爪子？** 鸭子无处不在、适应力强，几乎在任何环境中都能生存——正如 TuyaOpenClaw 的软件，可以从微控制器部署到桌面 Linux。"爪子"象征精准、灵巧，以及对真实世界设备和数字 Agent 的直接掌控。借助 TuyaOpen，硬件集成更加灵活，让你轻松将各种硬件能力直接接入 Agent。这个项目不是为了取代人，而是构建一个你完全掌控的、可靠的、持续学习进化的伴侣，并与真实的硬件接口相连。


### 核心理念

- **个人优先，非企业导向。** TuyaOpenClaw 为个人和创客而生。你的工作流和日常生活优先，没有官僚主义和企业冗余。
- **精简核心，插件扩展。** 必要 C 核心之外的一切——通道、提供商、工具——都是可替换和扩展的插件。
- **自调优与自适应。** 通过真实情节记忆和定期自我回顾持续学习，记忆衰减机制帮助它保持时效性。
- **IoT 控制记忆。** TuyaOpenClaw 将设备状态、操作和偏好记录在 IoT_MEMORY.md 中，构建你与涂鸦生态设备交互的丰富历史。这让 Agent 能够适应你的自动化习惯、设备特性和个人偏好，实现越用越智能的 IoT 控制。
- **对话式配置。** 无需厚重的配置文件：直接通过聊天完成 Agent 的设置和修改，硬件端和云端均适用。
- **鲜明的个性。** 每个实例都有独立配置和可选的硬件层，呈现有个性、有帮助的响应风格。
- **原生 C/TuyaOpen 构建。** 无 Node，无粘合的 Python 框架。从头为你实际使用的板子和平台而建。
- **极速启动。** 只需一个 TuyaOpen Key。按需选择模型和提供商，随时通过消息界面切换。
- **统一模型访问。** 凭借单一 TuyaOpen API Access Key，可接入 GPT、Claude、Deepseek、Qwen 等最新模型，为每项任务选择最佳模型，全部由涂鸦无缝托管和管理。
- **设备与云端 AI Agent。** TuyaOpenClaw 原生支持在设备本地运行的"设备 Agent"和以 Claw 方式运行于云端的"云端 Agent"。这种混合架构支持设备端动作、语音和 IoT 控制，同时提供高级云端智能、远程协调和在线 Skills / MCP 访问。你可以在设备端和云端之间灵活配置和迁移工作负载。

## ✨ 功能特性

- **统一消息输入：**
  通过统一代理 IM 接口，无缝集成 WhatsApp、Telegram、飞书等平台。

- **设备–云端混合 AI Agent：**
  由 TuyaOpen 驱动的集中式 AI Agent，通过 Claw 机制同时支持设备端和云端动作。

- **IoT 设备控制：**
  从 Agent 直接原生控制和管理涂鸦 IoT 智能设备。

- **音乐播放与音频：**
  内置音乐播放器。

- **硬件语音 ASR：**
  支持自动语音识别（ASR）的音频输入。

- **MCP 设备工具：**
  - **CRON 工具 MCP：** 调度设备任务和心跳。
  - **FILE 工具 MCP：** 支持设备上的文件操作和管理。
  - **IoT 设备控制工具 MCP：** 涂鸦连接设备的高级管理。
  - **EXEC 工具 MCP：** Raspberry Pi 上的远程代码执行/注入。

- **其他能力：**
  - SD 卡存储支持
  - 通过本地 `Agent.txt` 和 `memory.txt` 实现持久记忆
  - 灵活的网关切换，支持设备/云端 Agent 切换
  - 用于涂鸦认证和消息配置的简易 CLI


## 🏛️ 架构

![Architecture](https://images.tuyacn.com/fe-static/docs/img/62c1ad75-9f01-4911-9d30-c7bac463faec.png)

TuyaOpenClaw 架构将本地设备 Agent 和云端 Agent 统一在同一个系统下。其核心使用 TuyaOpen AI-Agent 框架处理消息、自动化和控制。本地硬件（Raspberry Pi、ESP32 或 Linux 设备）运行自己的 Claw 风格循环 Agent，能够通过 IoT 和硬件接口直接与设备通信。


## 🚀 快速开始

### 安装

```shell
git clone https://github.com/tuya/TuyaOpenClaw.git
cd TuyaOpenClaw
git submodule update --init
```

### 开发指南
- Tuya T5 MCU：[TuyaOpenClaw 快速开始（T5-AI）](https://tuyaopen.ai/docs/tuyaopenclaw/quick-start-T5AI)
- Raspberry Pi：[TuyaOpenClaw 快速开始（Raspberry Pi 5）](https://tuyaopen.ai/docs/tuyaopenclaw/quick-start-raspberry-pi-5)
- ESP32 MCU：[TuyaOpenClaw 快速开始（ESP32-S3）](https://tuyaopen.ai/docs/tuyaopenclaw/quick-start-ESP32S3)


## 🔌 插件 / Skills 开发

**Skill 格式**：Skills 是 `skills/` 目录下的 `.md` 文件。Agent 会获取它们的摘要并按照指令执行。使用以下结构：

```markdown
# Skill 标题

一句话描述。

## 何时使用
当用户询问 X / 当 Y 发生时。

## 如何使用
1. 调用 tool_A，参数为 ...
2. 然后调用 tool_B ...
3. 回复 ...

## 示例
用户："……" → 使用 get_current_time，然后 web_search "……"，然后回复 "……"
```

添加 Skill：在 skills 目录放入新的 `name.md`（例如通过 `write_file` 工具），或在 `skills/skill_loader.c` 中添加内置 Skill。参考已有 Skill 示例。

## 📁 项目结构

```
TuyaOpenClaw/
├── agent/                 # Agent 核心逻辑
│   ├── agent_loop.c/h     # Claw 风格 Agent 循环（推理、工具调用、上下文）
│   └── context_builder.c/h # 对话与系统上下文构建
├── config/                # 板型 / 平台配置（Kconfig 选择）
│   ├── TUYA_T5AI_BOARD_LCD_3.5_CAMERA.config  # Tuya T5AI 开发板
│   ├── RaspberryPi.config                     # Raspberry Pi
│   └── ESP32S3_BREAD_COMPACT_WIFI.config      # ESP32-S3
├── heartbeat/             # 定时心跳与保活
│   └── heartbeat.c/h
├── IM/                    # 统一消息层（消息总线 + 通道）
│   ├── bus/               # 消息总线（message_bus）
│   ├── channels/          # Telegram / Discord / 飞书通道
│   ├── proxy/             # HTTP 代理（TLS、云连接）
│   ├── cli/               # 串口 / 本地 CLI
│   ├── certs/             # 证书和 TLS 配置
│   ├── im_api.h / im_platform.h / im_config.h
│   └── README.md          # IM 组件文档
├── include/               # 应用级公共头文件
│   ├── app_im.h / tuyaopen_claw_chat.h / tuya_app_config.h
│   └── reset_netcfg.h
├── memory/                # 持久记忆与会话
│   ├── memory_manager.c/h # Agent.txt、memory.txt、IoT 记忆等
│   └── session_manager.c/h
├── src/                   # 应用入口与业务逻辑
│   ├── tuya_app_main.c    # TuyaOpen 应用入口，初始化，事件循环
│   ├── tuyaopen_claw_chat.c  # 聊天流程，设备/云端 Agent 调度
│   ├── app_im.c           # IM–Agent 桥接
│   ├── cli_cmd.c          # CLI 和配置（认证、IM 设置等）
│   └── reset_netcfg.c     # 网络配置重置
├── tools/                 # MCP 风格设备工具
│   ├── tool_cron.c/h      # CRON 调度与心跳
│   ├── tool_files.c/h     # 文件操作
│   ├── tools_register.c/h # 工具注册与调度
│   └── Kconfig
├── dist/                  # 构建输出（如 TuyaOpenClaw_1.0.x）
├── app_default.config     # 默认 Kconfig
└── TuyaOpen/              # TuyaOpen C SDK 子模块（平台、驱动、云端、AI 等）
```


## 🐛 问题反馈

请通过[新建 Issue](https://github.com/tuya/TuyaOpenClaw/issues) 反馈问题，提交前请确认该问题尚未被报告。欢迎任何改进贡献，感谢！🙏✨

## 🙏🙏🙏 关注我们

喜欢这个项目？请给个 Star！⭐⭐⭐⭐⭐

本项目离不开 TuyaOpen 开源 AI-Agent 框架的支撑，它让 AI-Agent 集成变得真正简单：
<a href="https://github.com/tuya/TuyaOpen/" target="_blank">
  <img src="https://img.shields.io/badge/TuyaOpen%20Repo-Visit-blue?logo=github" alt="TuyaOpen Repo"/>
</a>


## 📃 许可证

本项目遵循 [Apache License 2.0](https://www.apache.org/licenses/LICENSE-2.0)。

## 👥 致谢
- [TuyaOpen](https://github.com/tuya/TuyaOpen/) —— 本项目基于这个出色的硬件 AIoT OS 构建。
- [OpenClaw](https://github.com/openclaw/openclaw) —— 原始创意与灵感来源。
- [MimiClaw](https://github.com/memovai/mimiclaw) —— ESP32 本地 Agent 和 Skills 的原始灵感。

## 📝 作者

本项目由 [TuyaOpen 团队](https://tuyaopen.ai/) 创建，感谢所有[贡献者](https://github.com/tuya/TuyaOpenClaw/graphs/contributors)的帮助。

[![contributors](https://contrib.rocks/image?repo=tuya/TuyaOpenClaw)](https://github.com/tuya/TuyaOpenClaw/graphs/contributors)

---
