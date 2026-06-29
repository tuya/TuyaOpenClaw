# TuyaOpenClaw

> **Ducky** — TuyaOpen's hardware mascot — runs the Claw on every board.
> *(Formerly known as DuckyClaw.)*

TuyaOpenClaw is a hardware-oriented AI agent built on the TuyaOpen C SDK. It runs a Claw-style agent loop on edge devices (Tuya T5AI, ESP32, Raspberry Pi, Linux) that communicates with users via IM channels (Telegram, Discord, Feishu, WeChat, QQ Bot) and executes MCP-style tools directly on the device.

## Deployable to Hardware

| Category | Models/Platforms |
|----------|-----------------|
| **MCUs** | Tuya T5AI Module (ATK, Waveshare, variants), ESP32-S3 |
| **SoCs** | Raspberry Pi 4/5/CM4/CM5, DshanPi, Linux ARM SoCs (Qualcomm/Rockchip/Allwinner etc.) |
| **PCs**  | Linux Ubuntu (x64) |

---

This project is built on top of the TuyaOpen C SDK, which offers flexible cross ARM Cortex-M and ARM Cortex-A, and even x64 PC deployments. It also offers a wide variety of ready-to-use hardware drivers and APIs, making integrating new hardware and peripherals as easy as building blocks (sensors, displays, speaker-mic audio, cameras, IoT cloud integrations).

![GitHub Repo Banner](https://images.tuyacn.com/fe-static/docs/img/cc7ba9a9-6dd3-4569-aed7-d5694191e659.png)

**Your autonomous AI companion.** Simplify Hardware Integration, Unlock Infinite Control Possibilities

> [!WARNING]
> **🚧 Under Active Development** — This project is in heavy development and things will break. Running it now may spoil the experience we're building for you. Please visit or open Issues if you encounter any problem.


## ❓ Why TuyaOpenClaw?
Most AI agent frameworks are powerful but cumbersome. They often come with costly subscriptions, complicated setups, and are layered on top of other frameworks and APIs. TuyaOpenClaw offers a different path.

Built on a commercial-grade device–cloud AI agent foundation, TuyaOpenClaw uniquely fuses on-device agents with the power of the cloud.

With a single TuyaOpen Key, you gain seamless access to the Tuya Cloud Platform and its next-generation device–cloud AI agents.

It's lightweight and effortlessly deployable to almost any edge hardware — from WiFi-connected MCUs to ARM SoCs to PC Ubuntu.


| | TuyaOpenClaw  | OpenClaw / MimiClaw / others |
|---|---|---|
| **Architecture** | Hardware-oriented Claw agent on TuyaOpen C SDK; device–cloud fusion | OpenClaw: Node.js, 24/7 desktop/server agent. MimiClaw: bare-metal C on ESP32-S3 only. Others: framework stacks (Pi, Claude Code, etc.) |
| **Deployment** | MCUs (Tuya T5AI, ESP32-S3), SoCs (RPi 4/5, ARM Linux), PC (Ubuntu x64); one codebase | OpenClaw: Mac mini, Pi, VPS. MimiClaw: single $5 ESP32-S3. Others: server/desktop only, no edge MCU |
| **Runtime** | TuyaOpen C; ARM Cortex-M, Cortex-A, x64; no Node on MCU | OpenClaw: Node.js. MimiClaw: no OS, no Node. Others: Node.js 22+, pnpm, full OS |
| **Device–cloud** | One TuyaOpen key → Tuya Cloud Platform + device; built-in device-cloud AI | OpenClaw: self-hosted, optional API subs. MimiClaw: local-only. Others: cloud-only or DIY |
| **Channels** | Telegram, Discord, Feishu, WeChat (iLink), QQ Bot — unified via message bus + TLS proxy | OpenClaw: WhatsApp, Telegram, Discord, iMessage, Slack, etc. MimiClaw: Telegram, WebSocket, serial CLI |
| **Memory** | MEMORY.md + daily notes (YYYY-MM-DD.md); session manager (JSONL); persistent across reboots | OpenClaw: 24/7 persistent context, Obsidian/Raycast. MimiClaw: on-device MEMORY.md. Others: flat conversation history |
| **MCP Tools** | CRON, FILE, EXEC (RPi/Linux), HW (GPIO/I2C/PWM), Lua scripting, OpenClaw gateway ctrl | OpenClaw: browser automation, cron, ClawHub skills. MimiClaw: ReAct + web search, time, OTA, GPIO. Others: ad-hoc or single provider |
| **✨ Setup** | Single TuyaOpen key; pick board config, configure IM tokens | OpenClaw: Gateway, OAuth, multi-step. MimiClaw: flash firmware, API keys. Others: complex onboarding |
| **✨ Cost** | Low cost; unified key access to services | OpenClaw/MimiClaw: bring your own Claude/OpenAI. Others: Claude Pro/Max ($20–200/mo) or API-heavy |
| **✨ IoT Control** | Native device and IoT control via Tuya ecosystem | ❌ (typically no built-in device control) |
| **✨ Voice (ASR)** | Hardware voice input (ASR) supported on select boards | ❌ (voice input not natively supported) |
| **✨ Lua Scripting** | Sandboxed Lua 5.5 runtime with GPIO/delay modules; write agent skills in Lua | ❌ |



## 💡 Philosophy

**Ducky is TuyaOpen's hardware mascot** — a duck that thrives in any environment, from the tiniest MCU to a full desktop Linux box. TuyaOpenClaw inherits that spirit: one codebase, every board, zero compromise.

The name tells the whole story. **TuyaOpen** is the foundation — the commercial-grade C SDK that bridges chip-level hardware, IoT cloud, and AI in a single unified key. **Claw** is the agent paradigm — the loop of reasoning, tool use, and action borrowed from OpenClaw, adapted for the physical world. Put Ducky in the middle and you get an always-on hardware companion that gets smarter the more you use it.

**Why a duck and a claw?** Ducks are omnipresent, adaptable, and thrive in almost any environment — just like TuyaOpenClaw's software, which you can deploy from microcontrollers to desktop Linux. The "claw" symbolizes precision, dexterity, and direct command over both your real-world devices and digital agents. With TuyaOpen, hardware integration is much more flexible, allowing you to easily connect a wide range of hardware features and capabilities directly into your agent.


### Core Principles

- **Personal-first, not enterprise.** TuyaOpenClaw is made for individuals and makers. Your workflows and day-to-day life come first — no bureaucracy or corporate bloat.
- **Lean core, plugin power.** Everything outside the essential C core — channels, providers, tools — is a plugin you can swap and extend.
- **Self-tuning and adaptive.** Learns via real episode memory and periodic self-review, with memory fading that keeps it relevant.
- **Config by conversation.** No thick config files: set up and modify your agent simply by chatting, both on hardware and in the cloud.
- **Distinctive personality.** Each instance has its own SOUL.md configuration and optional hardware layer for characterful, helpful responses.
- **Builds natively on C/TuyaOpen.** No Node, no Python frameworks glued on. Built from scratch for the boards and platforms you actually use.
- **Rapid to start.** Requires just a TuyaOpen key. Choose models and providers as needed — switch between them with the messaging interface anytime.
- **Unified model access.** With a single TuyaOpen API Access Key, you can tap into the latest models from GPT, Claude, Deepseek, Qwen, and more — all seamlessly hosted and managed by Tuya.
- **Device and Cloud AI Agents.** Natively supports both agents that run locally on your device ("device agents") and those that operate in the cloud ("cloud agents"). On-device actions, voice, and IoT control work alongside cloud intelligence and online MCPs.


## ✨ Features

- **5 IM Channels:**
  Telegram, Discord, Feishu, WeChat (iLink), and QQ Bot — all through a unified message bus and TLS HTTP proxy, no channel-specific code in the agent loop.

- **Device–Cloud Hybrid AI Agent:**
  Centralized AI agent powered by TuyaOpen, enabling both on-device and cloud-based actions with the Claw mechanism (outer loop for messages, inner tool loop up to 10 iterations).

- **6 MCP-Style Device Tools:**
  - **CRON:** Scheduled device tasks, reminders, and cron job management.
  - **FILE:** File read/write/edit/list on SD card or flash filesystem.
  - **EXEC:** Remote code execution on Raspberry Pi / Linux (spawn shell commands from chat).
  - **HW:** Hardware peripheral control — GPIO, I2C, PWM directly from agent instructions.
  - **Lua Scripting:** Sandboxed Lua 5.5 runtime; write and run scripts via chat. Modules: GPIO, delay, safe OS APIs.
  - **OpenClaw Ctrl:** Gateway control for bridging device agent with OpenClaw / PC agents.

- **Persistent Memory:**
  Long-term memory in `MEMORY.md`, daily notes in `YYYY-MM-DD.md`, personality in `SOUL.md`, user profile in `USER.md` — all stored on flash/SD and loaded each turn.

- **Session Manager:**
  JSONL-based session persistence across reboots. Picks up conversations where they left off.

- **Skills System:**
  Agent skills are `.md` files in `skills/`. The agent reads a summary each turn and loads full skill files on demand via the `read_file` tool.

- **Build-Time Overlay:**
  `overlay/ai_components/` surgically patches `ai_agent` and `ai_mcp` without modifying the TuyaOpen SDK submodule — keeping upstream clean.

- **PSRAM Support:**
  `claw_malloc`/`claw_free` wrappers automatically route allocations to external PSRAM when available, enabling large context buffers on T5AI boards.

- **Hardware Voice ASR:**
  Audio input with Automatic Speech Recognition supported on select T5AI boards with microphone hardware.

- **SD Card & Filesystem:**
  Persistent storage for memory, skills, sessions, and agent-generated files.

- **Local CLI:**
  Serial/local CLI for setup and debugging without IM connectivity.


## 🏛️ Architecture

![Architecture](https://images.tuyacn.com/fe-static/docs/img/62c1ad75-9f01-4911-9d30-c7bac463faec.png)

The TuyaOpenClaw architecture combines local device agents and cloud agents under a unified system. At its core, it uses the TuyaOpen AI-Agent framework to handle messaging, automation, and control. Local hardware (Raspberry Pi, ESP32, T5AI, or Linux) runs a Claw-style agent loop, communicating with users via IM channels and executing MCP tools directly on-device.

**Agent loop flow:**
1. Outer loop blocks on `message_bus_pop_inbound()` — waits for any IM message
2. Context builder assembles system prompt (SOUL.md, USER.md, MEMORY.md, recent daily notes, skills summary, conversation history)
3. Inner tool loop (up to 10 iterations): sends prompt → waits for AI turn → checks tool call → feeds result back → repeats until final answer
4. Final response is pushed back through the message bus to the originating IM channel


## 🚀 Quick Start

### Clone

```shell
git clone https://github.com/tuya/TuyaOpenClaw.git
cd TuyaOpenClaw
git submodule update --init
```

### Configure Secrets

```shell
cp include/tuya_app_config_secrets.h.example include/tuya_app_config_secrets.h
# Edit the file and fill in:
#   TUYA_PRODUCT_ID, TUYA_OPENSDK_UUID, TUYA_OPENSDK_AUTHKEY  (Tuya cloud credentials)
#   IM_SECRET_CHANNEL_MODE and matching channel tokens (Feishu/Telegram/Discord/WeChat/QQ Bot)
#   CLAW_WS_AUTH_TOKEN, OPENCLAW_GATEWAY_* (gateway config, if using)
```

### Select a Board Config

```shell
# Tuya T5AI dev board (3.5" LCD + camera)
cp config/TUYA_T5AI_BOARD_LCD_3.5_CAMERA.config app_default.config

# Other supported boards:
# cp config/ATK_T5AI_MINI_BOARD_2.4LCD_CAMERA.config app_default.config
# cp config/WAVESHARE_T5AI_TOUCH_AMOLED_1_75.config app_default.config
# cp config/DshanPi_A1.config app_default.config
# cp config/TUYA_T5AI_CORE.config app_default.config
# cp config/RaspberryPi.config app_default.config
# cp config/ESP32S3_BREAD_COMPACT_WIFI.config app_default.config
```

### Build

```shell
# Initialize TuyaOpen environment (creates .venv, sets up toolchains)
. ./TuyaOpen/export.sh

# Build (skip interactive platform update prompts)
mkdir -p TuyaOpen/.cache && touch TuyaOpen/.cache/.dont_prompt_update_platform
tos.py build
```

Output goes to the `dist/` directory.

### Development Guides
- Tuya T5AI: [TuyaOpenClaw Quick Start (T5-AI)](https://tuyaopen.ai/docs/duckyclaw/ducky-quick-start-T5AI)
- Raspberry Pi: [TuyaOpenClaw Quick Start (Raspberry Pi 5)](https://tuyaopen.ai/docs/duckyclaw/ducky-quick-start-raspberry-pi-5)
- ESP32-S3: [TuyaOpenClaw Quick Start (ESP32-S3)](https://tuyaopen.ai/docs/duckyclaw/ducky-quick-start-ESP32S3)


## 🔌 Skills Development

Skills are `.md` files in the `skills/` directory. The agent gets a summary of available skills and loads the full text on demand using the `read_file` tool. Use this structure:

```markdown
# Skill Title

One-line description.

## When to use
When the user asks about X / when Y happens.

## How to use
1. Call tool_A with ...
2. Then call tool_B ...
3. Reply with ...

## Example
User: "…" → use get_current_time, then web_search "…", then reply "…"
```

Add skills: put a new `name.md` in the `skills/` directory (e.g., via the `write_file` tool from chat), or register built-in skills in `skills/skill_loader.c`.


## 📁 Project Structure

```
TuyaOpenClaw/
├── agent/                    # Agent core logic
│   ├── agent_loop.c/h        # Claw-style outer+inner loop (tool use, context, semaphore sync)
│   └── context_builder.c/h   # Assembles system prompt each turn
├── components/
│   └── lua/                  # Embedded Lua 5.5 sandboxed runtime
│       ├── lua/              # Lua 5.5 core sources (vendored)
│       ├── port/             # TuyaOpenClaw glue: sandbox init, runtime, module registry
│       └── modules/          # Hardware Lua modules (gpio, delay)
├── config/                   # Board/platform Kconfig selections
│   ├── TUYA_T5AI_BOARD_LCD_3.5_CAMERA.config      # Tuya T5AI 3.5" LCD + camera
│   ├── TUYA_T5AI_BOARD_LCD_3.5_CAMERA(NO_SDCARD).config
│   ├── TUYA_T5AI_CORE.config                       # Tuya T5AI core (no display)
│   ├── ATK_T5AI_MINI_BOARD_2.4LCD_CAMERA.config    # ATK 2.4" LCD variant
│   ├── WAVESHARE_T5AI_TOUCH_AMOLED_1_75.config     # Waveshare 1.75" AMOLED
│   ├── DshanPi_A1.config                           # DshanPi A1
│   ├── RaspberryPi.config                          # Raspberry Pi (Linux)
│   └── ESP32S3_BREAD_COMPACT_WIFI.config           # ESP32-S3 breadboard
├── cron_service/             # Background cron scheduler (fires cron jobs)
│   └── cron_service.c/h
├── gateway/                  # WebSocket gateway integration
│   ├── ws_server.c/h         # Local WebSocket server
│   └── acp_client.c/h        # ACP client → OpenClaw gateway
├── heartbeat/                # Periodic heartbeat keepalive
│   └── heartbeat.c/h
├── IM/                       # Unified messaging layer
│   ├── bus/                  # Thread-safe message bus (inbound/outbound queues)
│   ├── channels/             # Telegram, Discord, Feishu, WeChat (iLink), QQ Bot
│   ├── proxy/                # TLS HTTP proxy for cloud IM APIs
│   ├── cli/                  # Local serial CLI input channel
│   └── certs/                # TLS certificates
├── include/                  # App-level public headers
│   ├── tuya_app_config.h     # Default config (device ID, IM mode, etc.)
│   └── tuya_app_config_secrets.h.example  # Secrets template (gitignored when filled)
├── memory/                   # Persistent memory and sessions
│   ├── memory_manager.c/h    # MEMORY.md, daily notes, SOUL.md, USER.md on flash/SD
│   └── session_manager.c/h   # JSONL session persistence
├── overlay/
│   └── ai_components/        # Build-time patches for ai_agent and ai_mcp (SDK stays clean)
├── skills/                   # Agent skill definitions and loader
│   └── skill_loader.c/h      # Scans skills/, builds summary for system prompt
├── src/                      # App entry and business logic
│   ├── tuya_app_main.c       # TuyaOpen app entry, init, event loop
│   ├── tuyaopen_claw_chat.c  # AI stream event bridge (notifies agent loop on turn end)
│   ├── app_im.c              # IM–Agent bridge
│   └── app_cli_cmd.c         # CLI and runtime config commands
├── tools/                    # MCP-style device tools
│   ├── tool_cron.c/h         # CRON: schedule tasks and reminders
│   ├── tool_files.c/h        # FILE: read/write/edit/list on device filesystem
│   ├── tool_exec.c/h         # EXEC: shell command execution (Linux/RPi only)
│   ├── tool_hw.c/h           # HW: GPIO, I2C, PWM hardware control
│   ├── tool_lua.c/h          # LUA: run sandboxed Lua 5.5 scripts
│   ├── tool_openclaw_ctrl.c/h # OpenClaw gateway control
│   └── tools_register.c/h    # MCP server init and tool registration
├── app_default.config        # Active Kconfig (copy from config/ before building)
├── CMakeLists.txt            # Build graph (overlay logic, Lua component, IM component)
└── TuyaOpen/                 # TuyaOpen C SDK submodule (platform, drivers, cloud, AI)
```


## 🐛 Issues

Please report any issues and bugs by [creating a new issue here](https://github.com/tuya/TuyaOpenClaw/issues), also make sure you're reporting an issue that doesn't exist. Any help to improve the project would be appreciated. Thanks! 🙏✨

## 🙏🙏🙏 Follow us

Like this project? Leave a star! ⭐⭐⭐⭐⭐

This project wouldn't be possible without the amazing TuyaOpen Open Source AI-Agent framework. It makes AI-Agent integration really easy.
<a href="https://github.com/tuya/TuyaOpen/" target="_blank">
  <img src="https://img.shields.io/badge/TuyaOpen%20Repo-Visit-blue?logo=github" alt="TuyaOpen Repo"/>
</a>


## 📃 License

This project is licensed under the [Apache License 2.0](https://www.apache.org/licenses/LICENSE-2.0).

## 👥 Credits
- [TuyaOpen](https://github.com/tuya/TuyaOpen/) — This project is built on top of this amazing hardware AIoT OS called TuyaOpen.
- [OpenClaw](https://github.com/openclaw/openclaw) — Original idea and inspiration.
- [MimiClaw](https://github.com/memovai/mimiclaw) — Original inspiration for ESP32 local agent and skills.

## 📝 Author

This project is created by [TuyaOpen Team](https://tuyaopen.ai/), with the help of awesome [contributors](https://github.com/tuya/TuyaOpenClaw/graphs/contributors).

[![contributors](https://contrib.rocks/image?repo=tuya/TuyaOpenClaw)](https://github.com/tuya/TuyaOpenClaw/graphs/contributors)

---
