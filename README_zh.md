# TuyaOpenClaw

> **Ducky** —— TuyaOpen 的硬件吉祥物 —— 在每一块板子上运行 Claw。
> *（前身为 DuckyClaw。）*

TuyaOpenClaw 是基于 TuyaOpen C SDK 构建的硬件导向 AI Agent。它在边缘设备（Tuya T5AI、ESP32、Raspberry Pi、Linux）上运行 Claw 风格的 Agent 循环，通过 IM 通道（Telegram、Discord、飞书、微信、QQ 机器人）与用户通信，并直接在设备端执行 MCP 风格的工具调用。

## 支持的硬件

| 类型 | 型号 / 平台 |
|------|------------|
| **MCU** | Tuya T5AI 模组（ATK、Waveshare、DshanPi 等变体），ESP32-S3 |
| **SoC** | Raspberry Pi 4/5/CM4/CM5，Linux ARM SoC（高通 / 瑞芯微 / 全志等）|
| **PC**  | Linux Ubuntu（x64）|

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
| **架构** | 基于 TuyaOpen C SDK 的硬件导向 Claw Agent；设备–云端融合 | OpenClaw：Node.js 全天候桌面/服务器 Agent。MimiClaw：ESP32-S3 裸机 C。其他：框架栈（Pi、Claude Code 等）|
| **部署目标** | MCU（T5AI、ESP32-S3）、SoC（RPi 4/5、ARM Linux）、PC（Ubuntu x64）；单一代码库 | OpenClaw：Mac mini、Pi、VPS。MimiClaw：5 美元 ESP32-S3。其他：仅服务器/桌面 |
| **运行时** | TuyaOpen C；ARM Cortex-M/A、x64；MCU 无需 Node | OpenClaw：Node.js。MimiClaw：无 OS。其他：Node.js 22+、pnpm、完整 OS |
| **设备–云端** | 一个 TuyaOpen Key → 涂鸦云平台 + 设备；内置设备–云端 AI | OpenClaw：自托管，可选 API 订阅。MimiClaw：纯本地。其他：纯云端或 DIY |
| **消息通道** | Telegram、Discord、飞书、微信（iLink）、QQ 机器人 —— 统一消息总线 + TLS 代理 | OpenClaw：WhatsApp、Telegram、Discord、iMessage、Slack 等。MimiClaw：Telegram、WebSocket、串口 CLI |
| **记忆** | MEMORY.md + 日记（YYYY-MM-DD.md）；JSONL 会话持久化；重启后继续 | OpenClaw：全天候持久上下文、Obsidian/Raycast。MimiClaw：设备端 MEMORY.md |
| **MCP 工具** | CRON、FILE、EXEC（RPi/Linux）、HW（GPIO/I2C/PWM）、Lua 脚本、OpenClaw 网关控制 | OpenClaw：浏览器自动化、cron、ClawHub Skills。MimiClaw：ReAct + 网页搜索、时间、OTA、GPIO |
| **✨ 上手** | 单个 TuyaOpen Key；选板（T5AI/ESP32/RPi/Linux），配置 IM Token | OpenClaw：网关、OAuth、多步骤。MimiClaw：刷固件、API Key。其他：复杂依赖 |
| **✨ 成本** | 低成本；统一 Key 访问服务 | OpenClaw/MimiClaw：自备 Claude/OpenAI。其他：Claude Pro/Max（$20–200/月）或重度 API |
| **✨ IoT 控制** | 原生设备和 IoT 控制，可管理涂鸦生态中的其他设备 | ❌（通常无内置设备控制）|
| **✨ 语音（ASR）** | 特定板型支持硬件语音输入（ASR）| ❌（原生不支持语音输入）|
| **✨ Lua 脚本** | 沙箱 Lua 5.5 运行时，含 GPIO/延时模块；通过对话编写 Agent 技能 | ❌ |


## 💡 设计理念

**Ducky 是 TuyaOpen 的硬件吉祥物** —— 一只能在任何环境中如鱼得水的鸭子，从最小的 MCU 到完整的桌面 Linux 系统。TuyaOpenClaw 继承了这种精神：一套代码，适配所有板子，零妥协。

项目名称本身就是完整的故事。**TuyaOpen** 是底座 —— 将芯片级硬件、IoT 云和 AI 统一于单一 Key 的商业级 C SDK。**Claw** 是 Agent 范式 —— 从 OpenClaw 借鉴的推理–工具调用–行动循环，经过改造以适应物理世界。Ducky 在中间，构成一个越用越聪明的全天候硬件伴侣。

**为什么是鸭子和爪子？** 鸭子无处不在、适应力强，几乎在任何环境中都能生存——正如 TuyaOpenClaw 的软件，可以从微控制器部署到桌面 Linux。"爪子"象征精准、灵巧，以及对真实世界设备和数字 Agent 的直接掌控。借助 TuyaOpen，硬件集成更加灵活，让你轻松将各种硬件能力直接接入 Agent。


### 核心理念

- **个人优先，非企业导向。** TuyaOpenClaw 为个人和创客而生。你的工作流和日常生活优先，没有官僚主义和企业冗余。
- **精简核心，插件扩展。** 必要 C 核心之外的一切——通道、提供商、工具——都是可替换和扩展的插件。
- **自调优与自适应。** 通过真实情节记忆和定期自我回顾持续学习，记忆衰减机制帮助它保持时效性。
- **对话式配置。** 无需厚重的配置文件：直接通过聊天完成 Agent 的设置和修改，硬件端和云端均适用。
- **鲜明的个性。** 每个实例都有独立的 SOUL.md 配置和可选的硬件层，呈现有个性、有帮助的响应风格。
- **原生 C/TuyaOpen 构建。** 无 Node，无粘合的 Python 框架。从头为你实际使用的板子和平台而建。
- **极速启动。** 只需一个 TuyaOpen Key。按需选择模型和提供商，随时通过消息界面切换。
- **统一模型访问。** 凭借单一 TuyaOpen API Access Key，可接入 GPT、Claude、Deepseek、Qwen 等最新模型，全部由涂鸦无缝托管和管理。
- **设备与云端 AI Agent。** 原生支持在设备本地运行的"设备 Agent"和以 Claw 方式运行于云端的"云端 Agent"。设备端动作、语音和 IoT 控制，与云端智能和在线 MCP 并行运作。


## ✨ 功能特性

- **5 个 IM 通道：**
  Telegram、Discord、飞书、微信（iLink）、QQ 机器人 —— 统一消息总线 + TLS HTTP 代理，Agent 循环无需感知通道差异。

- **设备–云端混合 AI Agent：**
  由 TuyaOpen 驱动的集中式 AI Agent，通过 Claw 机制同时支持设备端和云端动作（外循环等待消息，内循环最多 10 次工具调用迭代）。

- **6 个 MCP 风格设备工具：**
  - **CRON：** 调度设备任务、提醒和定时任务管理。
  - **FILE：** 在设备文件系统（SD 卡或 Flash）上读写/编辑/列目录。
  - **EXEC：** Raspberry Pi / Linux 上的远程 Shell 命令执行（通过对话执行系统命令）。
  - **HW：** 硬件外设控制 —— 从 Agent 指令直接操作 GPIO、I2C、PWM。
  - **Lua 脚本：** 沙箱 Lua 5.5 运行时；通过对话编写和运行脚本。模块：GPIO、延时、安全 OS API。
  - **OpenClaw 控制：** 网关控制，用于将设备 Agent 与 OpenClaw / PC Agent 桥接。

- **持久记忆：**
  长期记忆存储在 `MEMORY.md`，日记存储在 `YYYY-MM-DD.md`，个性配置在 `SOUL.md`，用户档案在 `USER.md` —— 全部存储在 Flash/SD 卡上，每轮对话自动加载。

- **会话管理器：**
  基于 JSONL 的会话持久化，重启后可继续之前的对话。

- **技能系统（Skills）：**
  Agent 技能是 `skills/` 目录下的 `.md` 文件。Agent 每轮获取技能摘要，并通过 `read_file` 工具按需加载完整技能内容。

- **编译时 Overlay：**
  `overlay/ai_components/` 对 `ai_agent` 和 `ai_mcp` 进行外科手术式补丁，无需修改 TuyaOpen SDK 子模块，保持上游代码干净。

- **PSRAM 支持：**
  `claw_malloc`/`claw_free` 包装器在外部 PSRAM 可用时自动将分配路由到 PSRAM，使 T5AI 板上能够使用大容量上下文缓冲区。

- **硬件语音 ASR：**
  特定 T5AI 板型（配备麦克风硬件）支持自动语音识别音频输入。

- **SD 卡与文件系统：**
  持久存储记忆、技能、会话和 Agent 生成的文件。

- **本地 CLI：**
  串口/本地 CLI，无需 IM 连接即可完成设置和调试。


## 🏛️ 架构

![Architecture](https://images.tuyacn.com/fe-static/docs/img/62c1ad75-9f01-4911-9d30-c7bac463faec.png)

TuyaOpenClaw 架构将本地设备 Agent 和云端 Agent 统一在同一个系统下。其核心使用 TuyaOpen AI-Agent 框架处理消息、自动化和控制。本地硬件（Raspberry Pi、ESP32、T5AI 或 Linux）运行 Claw 风格的 Agent 循环，通过 IM 通道与用户通信，并直接在设备端执行 MCP 工具。

**Agent 循环流程：**
1. 外循环阻塞在 `message_bus_pop_inbound()` —— 等待任意 IM 消息
2. 上下文构建器组装系统提示（SOUL.md、USER.md、MEMORY.md、近期日记、技能摘要、对话历史）
3. 内循环工具调用（最多 10 次迭代）：发送提示 → 等待 AI 轮次 → 检查工具调用 → 将结果反馈 → 循环至最终回答
4. 最终响应通过消息总线推送回发起 IM 通道


## 🚀 快速开始

### 克隆仓库

```shell
git clone https://github.com/tuya/TuyaOpenClaw.git
cd TuyaOpenClaw
git submodule update --init
```

### 配置密钥

```shell
cp include/tuya_app_config_secrets.h.example include/tuya_app_config_secrets.h
# 编辑该文件并填入：
#   TUYA_PRODUCT_ID, TUYA_OPENSDK_UUID, TUYA_OPENSDK_AUTHKEY  （涂鸦云凭证）
#   IM_SECRET_CHANNEL_MODE 及对应通道 Token（飞书/Telegram/Discord/微信/QQ 机器人）
#   CLAW_WS_AUTH_TOKEN, OPENCLAW_GATEWAY_*（网关配置，如需使用）
```

### 选择板型配置

```shell
# Tuya T5AI 开发板（3.5" LCD + 摄像头）
cp config/TUYA_T5AI_BOARD_LCD_3.5_CAMERA.config app_default.config

# 其他支持的板型：
# cp config/ATK_T5AI_MINI_BOARD_2.4LCD_CAMERA.config app_default.config
# cp config/WAVESHARE_T5AI_TOUCH_AMOLED_1_75.config app_default.config
# cp config/DshanPi_A1.config app_default.config
# cp config/TUYA_T5AI_CORE.config app_default.config
# cp config/RaspberryPi.config app_default.config
# cp config/ESP32S3_BREAD_COMPACT_WIFI.config app_default.config
```

### 编译

```shell
# 初始化 TuyaOpen 环境（创建 .venv，配置工具链）
. ./TuyaOpen/export.sh

# 编译（跳过交互式平台更新提示）
mkdir -p TuyaOpen/.cache && touch TuyaOpen/.cache/.dont_prompt_update_platform
tos.py build
```

编译产物输出至 `dist/` 目录。

### 开发指南
- Tuya T5AI：[TuyaOpenClaw 快速开始（T5-AI）](https://tuyaopen.ai/docs/tuyaopenclaw/quick-start-T5AI)
- Raspberry Pi：[TuyaOpenClaw 快速开始（Raspberry Pi 5）](https://tuyaopen.ai/docs/tuyaopenclaw/quick-start-raspberry-pi-5)
- ESP32-S3：[TuyaOpenClaw 快速开始（ESP32-S3）](https://tuyaopen.ai/docs/tuyaopenclaw/quick-start-ESP32S3)


## 🔌 技能（Skills）开发

技能是 `skills/` 目录下的 `.md` 文件。Agent 每轮获取技能摘要，并通过 `read_file` 工具按需加载完整内容。使用以下结构：

```markdown
# 技能标题

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

添加技能：在 `skills/` 目录放入新的 `name.md`（例如通过 `write_file` 工具从对话中创建），或在 `skills/skill_loader.c` 中注册内置技能。


## 📁 项目结构

```
TuyaOpenClaw/
├── agent/                    # Agent 核心逻辑
│   ├── agent_loop.c/h        # Claw 风格外循环+内循环（工具调用、上下文、信号量同步）
│   └── context_builder.c/h   # 每轮组装系统提示
├── components/
│   └── lua/                  # 嵌入式 Lua 5.5 沙箱运行时
│       ├── lua/              # Lua 5.5 核心源码（vendored）
│       ├── port/             # TuyaOpenClaw 胶合层：沙箱初始化、运行时、模块注册
│       └── modules/          # 硬件 Lua 模块（gpio、delay）
├── config/                   # 板型 / 平台 Kconfig 配置
│   ├── TUYA_T5AI_BOARD_LCD_3.5_CAMERA.config      # Tuya T5AI 3.5" LCD + 摄像头
│   ├── TUYA_T5AI_BOARD_LCD_3.5_CAMERA(NO_SDCARD).config
│   ├── TUYA_T5AI_CORE.config                       # Tuya T5AI 核心板（无显示）
│   ├── ATK_T5AI_MINI_BOARD_2.4LCD_CAMERA.config    # ATK 2.4" LCD 变体
│   ├── WAVESHARE_T5AI_TOUCH_AMOLED_1_75.config     # Waveshare 1.75" AMOLED
│   ├── DshanPi_A1.config                           # DshanPi A1
│   ├── RaspberryPi.config                          # Raspberry Pi（Linux）
│   └── ESP32S3_BREAD_COMPACT_WIFI.config           # ESP32-S3 面包板
├── cron_service/             # 后台定时任务调度器（独立于 MCP 工具接口）
│   └── cron_service.c/h
├── gateway/                  # WebSocket 网关集成
│   ├── ws_server.c/h         # 本地 WebSocket 服务端
│   └── acp_client.c/h        # ACP 客户端 → OpenClaw 网关
├── heartbeat/                # 定时心跳保活
│   └── heartbeat.c/h
├── IM/                       # 统一消息层
│   ├── bus/                  # 线程安全消息总线（入站/出站队列）
│   ├── channels/             # Telegram、Discord、飞书、微信（iLink）、QQ 机器人
│   ├── proxy/                # 云端 IM API 的 TLS HTTP 代理
│   ├── cli/                  # 本地串口 CLI 输入通道
│   └── certs/                # TLS 证书
├── include/                  # 应用级公共头文件
│   ├── tuya_app_config.h     # 默认配置（设备 ID、IM 模式等）
│   └── tuya_app_config_secrets.h.example  # 密钥模板（填写后已 gitignore）
├── memory/                   # 持久记忆与会话
│   ├── memory_manager.c/h    # Flash/SD 上的 MEMORY.md、日记、SOUL.md、USER.md
│   └── session_manager.c/h   # JSONL 会话持久化
├── overlay/
│   └── ai_components/        # ai_agent 和 ai_mcp 的编译时补丁（SDK 保持干净）
├── skills/                   # Agent 技能定义与加载器
│   └── skill_loader.c/h      # 扫描 skills/，为系统提示构建摘要
├── src/                      # 应用入口与业务逻辑
│   ├── tuya_app_main.c       # TuyaOpen 应用入口，初始化，事件循环
│   ├── tuyaopen_claw_chat.c  # AI 流式事件桥接（轮次结束时通知 Agent 循环）
│   ├── app_im.c              # IM–Agent 桥接
│   └── app_cli_cmd.c         # CLI 和运行时配置命令
├── tools/                    # MCP 风格设备工具
│   ├── tool_cron.c/h         # CRON：调度任务和提醒
│   ├── tool_files.c/h        # FILE：设备文件系统读写/编辑/列目录
│   ├── tool_exec.c/h         # EXEC：Shell 命令执行（仅 Linux/RPi）
│   ├── tool_hw.c/h           # HW：GPIO、I2C、PWM 硬件控制
│   ├── tool_lua.c/h          # LUA：运行沙箱 Lua 5.5 脚本
│   ├── tool_openclaw_ctrl.c/h # OpenClaw 网关控制
│   └── tools_register.c/h    # MCP 服务器初始化与工具注册
├── app_default.config        # 当前激活的 Kconfig（编译前从 config/ 复制）
├── CMakeLists.txt            # 构建图（Overlay 逻辑、Lua 组件、IM 组件）
└── TuyaOpen/                 # TuyaOpen C SDK 子模块（平台、驱动、云端、AI）
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
